#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include "defs.h"

static struct class class;
struct workqueue_struct *wkqueue;
int blk_major;

static struct {
	spinlock_t lock;
	struct list_head devs;
	DECLARE_BITMAP(devnums, DEVICES);
	unsigned long cache_pages;
} state;

/* It is an error to use this when the refcount may have already reached
   zero.  (The kref infrastructure does not handle this case.) */
void nexus_dev_get(struct nexus_dev *dev)
{
	if (class_device_get(dev->class_dev) == NULL)
		BUG();
}

/* @unlink is true if we should remove the sysfs entries - that is, if
   the character device is going away or the ctr has errored out.  This
   must be called with @unlink true exactly once per device.  The dev lock
   MUST NOT be held. */
void nexus_dev_put(struct nexus_dev *dev, int unlink)
{
	ndebug("dev_put, refs %d, unlink %d",
			atomic_read(&dev->class_dev->kobj.kref.refcount),
			unlink);
	BUG_ON(in_atomic());
	if (unlink)
		class_device_unregister(dev->class_dev);
	else
		class_device_put(dev->class_dev);
}

void user_get(struct nexus_dev *dev)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	dev->need_user++;
	ndebug("need_user now %u", dev->need_user);
}

void user_put(struct nexus_dev *dev)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	if (!--dev->need_user)
		wake_up_interruptible(&dev->waiting_users);
	ndebug("need_user now %u", dev->need_user);
}

int shutdown_dev(struct nexus_dev *dev, int force)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	if (dev_is_shutdown(dev))
		return -ENXIO;
	if (!force && dev->need_user != 0) {
		return -EBUSY;
	}
	debug("Shutting down chardev");
	spin_lock(&state.lock);
	list_del_init(&dev->lh_devs);
	spin_unlock(&state.lock);
	shutdown_usermsg(dev);
	return 0;
}

static void class_release_dummy(struct class *class)
{
	/* Dummy function: class is allocated statically, so we don't need
	   a destructor, but if we don't have one the kernel will sometimes
	   whine to the log */
	return;
}

static unsigned long get_system_page_count(void)
{
	struct sysinfo s;
	
	si_meminfo(&s);
	BUG_ON(s.mem_unit != PAGE_SIZE);
	return s.totalram;
}

static int nexus_open(struct inode *ino, struct file *filp)
{
	struct nexus_dev *dev;
	int found=0;
	
	spin_lock(&state.lock);
	list_for_each_entry(dev, &state.devs, lh_devs) {
		if (dev == ino->i_bdev->bd_disk->private_data) {
			found=1;
			/* Since it's still in the devs list, we know that
			   chardev still holds a reference. */
			nexus_dev_get(dev);
			break;
		}
	}
	spin_unlock(&state.lock);
	if (!found)
		return -ENODEV;
	
	if (mutex_lock_interruptible(&dev->lock)) {
		nexus_dev_put(dev, 0);
		return -ERESTARTSYS;
	}
	user_get(dev);
	mutex_unlock(&dev->lock);
	return 0;
}

static int nexus_release(struct inode *ino, struct file *filp)
{
	struct nexus_dev *dev=ino->i_bdev->bd_disk->private_data;
	
	/* Our return value is ignored, so we must use the uninterruptible
	   variant */
	mutex_lock(&dev->lock);
	user_put(dev);
	mutex_unlock(&dev->lock);
	nexus_dev_put(dev, 0);
	return 0;
}

static int alloc_devnum(void)
{
	int num;

	spin_lock(&state.lock);
	num=find_first_zero_bit(state.devnums, DEVICES);
	if (num != DEVICES)
		__set_bit(num, state.devnums);
	spin_unlock(&state.lock);
	if (num == DEVICES)
		return -1;
	else
		return num;
}

static void free_devnum(int devnum)
{
	spin_lock(&state.lock);
	__clear_bit(devnum, state.devnums);
	spin_unlock(&state.lock);
}

/* Workqueue callback */
static void nexus_add_disk(void *data)
{
	struct nexus_dev *dev=data;
	
	add_disk(dev->gendisk);
	nexus_dev_put(dev, 0);
}

/* Called by dev->class_dev's release callback */
static void nexus_dev_dtr(struct class_device *class_dev)
{
	struct nexus_dev *dev=class_get_devdata(class_dev);
	
	debug("Dtr called");
	BUG_ON(!dev_is_shutdown(dev));
	BUG_ON(!list_empty(&dev->requests));
	if (dev->gendisk) {
		if (dev->gendisk->flags & GENHD_FL_UP) {
			del_gendisk(dev->gendisk);
		} else {
			/* Disk was created but not yet added */
			put_disk(dev->gendisk);
		}
	}
	chunkdata_free_table(dev);
	transform_free(dev);
	if (dev->queue)
		blk_cleanup_queue(dev->queue);
	if (dev->chunk_bdev)
		close_bdev_excl(dev->chunk_bdev);
	spin_lock(&state.lock);
	state.cache_pages -= dev->cachesize * chunk_pages(dev);
	spin_unlock(&state.lock);
	free_devnum(dev->index);
	kfree(dev->class_dev);
	kfree(dev);
	module_put(THIS_MODULE);
}

static struct block_device_operations nexus_ops = {
	.owner =	THIS_MODULE,
	.open =		nexus_open,
	.release =	nexus_release,
};

struct nexus_dev *nexus_dev_ctr(char *devnode, unsigned chunksize,
			unsigned cachesize, sector_t offset, crypto_t suite,
			compress_t default_compress,
			compress_t supported_compress)
{
	struct nexus_dev *dev;
	sector_t capacity;
	unsigned long pages=get_system_page_count();
	int devnum;
	int ret;
	int ok;
	
	debug("Ctr starting");
	
	/* If the userspace process goes away right after the ctr returns, the
	   device will still exist until delayed_add_disk runs but the module
	   could be unloaded.  To get around this, we get an extra reference
	   to the module here and put it in the dtr. */
	if (!try_module_get(THIS_MODULE)) {
		ret=-ENOPKG;
		goto early_fail_module;
	}
	
	devnum=alloc_devnum();
	if (devnum < 0) {
		ret=-EMFILE;
		goto early_fail_devnum;
	}
	
	dev=kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL) {
		ret=-ENOMEM;
		goto early_fail_devalloc;
	}
	
	dev->class_dev=create_class_dev(&class, DEVICE_NAME "%c", 'a' + devnum);
	if (IS_ERR(dev->class_dev)) {
		ret=PTR_ERR(dev->class_dev);
		goto early_fail_classdev;
	}
	class_set_devdata(dev->class_dev, dev);
	/* Use class-wide release function */
	class_dev_set_release(dev->class_dev, NULL);
	
	/* Now we have refcounting, so all further errors should deallocate
	   through the destructor */
	mutex_init(&dev->lock);
	spin_lock_init(&dev->queue_lock);
	INIT_LIST_HEAD(&dev->requests);
	spin_lock_init(&dev->requests_lock);
	INIT_WORK(&dev->cb_run_requests, nexus_run_requests, dev);
	init_waitqueue_head(&dev->waiting_users);
	dev->index=devnum;
	dev->owner=current->uid;
	
	INIT_LIST_HEAD(&dev->lh_devs);
	spin_lock(&state.lock);
	list_add_tail(&dev->lh_devs, &state.devs);
	spin_unlock(&state.lock);
	
	if (chunksize < 512 || (chunksize & (chunksize - 1)) != 0) {
		log(KERN_ERR, "chunk size must be >= 512 and a power of 2");
		ret=-EINVAL;
		goto bad;
	}
	dev->chunksize=chunksize;
	if (cachesize < MIN_CONCURRENT_REQS * MAX_CHUNKS_PER_IO) {
		log(KERN_ERR, "cache size may not be smaller than %u",
				MIN_CONCURRENT_REQS * MAX_CHUNKS_PER_IO);
		ret=-EINVAL;
		goto bad;
	}
	if (cachesize * chunk_pages(dev) > pages * MAX_DEV_ALLOCATION_MULT /
				MAX_DEV_ALLOCATION_DIV) {
		log(KERN_ERR, "cache size may not be larger than %u/%u of "
					"system RAM", MAX_DEV_ALLOCATION_MULT,
					MAX_DEV_ALLOCATION_DIV);
		/* Abuse of return code, but userspace needs to be able to
		   distinguish this case */
		ret=-ENOSPC;
		goto bad;
	}
	dev->cachesize=cachesize;
	spin_lock(&state.lock);
	/* The dtr subtracts this off again only if chunksize and cachesize
	   are nonzero */
	state.cache_pages += cachesize * chunk_pages(dev);
	ok=(state.cache_pages <= pages * MAX_ALLOCATION_MULT
				/ MAX_ALLOCATION_DIV);
	spin_unlock(&state.lock);
	/* This is racy wrt multiple simultaneous ctr calls, but it's
	   conservatively racy: multiple ctr calls may fail when one of them
	   should have succeeded */
	if (!ok) {
		log(KERN_ERR, "will not allocate more than %u/%u of system RAM "
					"for cache", MAX_ALLOCATION_MULT,
					MAX_ALLOCATION_DIV);
		/* Abuse of return code, but userspace needs to be able to
		   distinguish this case */
		ret=-ENOSPC;
		goto bad;
	}
	debug("chunksize %u, cachesize %u, backdev %s, offset " SECTOR_FORMAT,
				chunksize, cachesize, devnode, offset);
	
	ndebug("Opening %s", devnode);
	dev->chunk_bdev=open_bdev_excl(devnode, 0, dev);
	if (IS_ERR(dev->chunk_bdev)) {
		log(KERN_ERR, "couldn't open %s", devnode);
		ret=PTR_ERR(dev->chunk_bdev);
		dev->chunk_bdev=NULL;
		goto bad;
	}
	/* This is how the BLKGETSIZE64 ioctl is implemented, but
	   bd_inode is labeled "will die" in fs.h */
	capacity=dev->chunk_bdev->bd_inode->i_size / 512;
	if (capacity <= offset) {
		log(KERN_ERR, "specified offset is >= disk capacity");
		ret=-EINVAL;
		goto bad;
	}
	dev->offset=offset;
	/* Make sure the capacity, after offset adjustment, is a multiple
	   of the chunksize */
	capacity=(capacity - offset) & ~(loff_t)(chunk_sectors(dev) - 1);
	debug("Chunk partition capacity: " SECTOR_FORMAT " MB", capacity >> 11);
	dev->chunks=chunk_of(dev, capacity);
	
	ndebug("Allocating queue");
	dev->queue=blk_init_queue(nexus_request, &dev->queue_lock);
	if (dev->queue == NULL) {
		log(KERN_ERR, "couldn't allocate request queue");
		ret=-ENOMEM;
		goto bad;
	}
	dev->queue->queuedata=dev;
	blk_queue_bounce_limit(dev->queue, BLK_BOUNCE_ANY);
	blk_queue_max_phys_segments(dev->queue, MAX_SEGS_PER_IO);
	/* By default, blk_rq_map_sg() coalesces physically adjacent pages
	   into the same segment, resulting in a segment that spans more
	   than one page but only points directly to the first struct page.
	   This works fine when scatterlist_copy() kmaps low memory but
	   will die if it kmaps high memory.  Instead, we tell blk_rq_map_sg()
	   not to cross page boundaries when coalescing segments. */
	blk_queue_segment_boundary(dev->queue, PAGE_SIZE - 1);
	/* blk_rq_map_sg() enforces a minimum boundary of PAGE_CACHE_SIZE.
	   If that ever becomes larger than PAGE_SIZE, the above call
	   won't do the right thing for us and we'll need to modify
	   scatterlist_copy() to divide each scatterlist entry into its
	   constituent pages. */
	BUILD_BUG_ON(PAGE_SIZE != PAGE_CACHE_SIZE);
	blk_queue_max_sectors(dev->queue,
				chunk_sectors(dev) * (MAX_CHUNKS_PER_IO - 1));
	
	ndebug("Allocating transforms");
	ret=transform_alloc(dev, suite, default_compress, supported_compress);
	if (ret) {
		log(KERN_ERR, "could not configure transforms");
		goto bad;
	}
	
	ndebug("Allocating chunkdata");
	ret=chunkdata_alloc_table(dev);
	if (ret) {
		log(KERN_ERR, "couldn't allocate chunkdata");
		goto bad;
	}
	
	ndebug("Allocating disk");
	dev->gendisk=alloc_disk(MINORS_PER_DEVICE);
	if (dev->gendisk == NULL) {
		log(KERN_ERR, "couldn't allocate gendisk");
		ret=-ENOMEM;
		goto bad_put_chunkdata;
	}
	dev->gendisk->major=blk_major;
	dev->gendisk->first_minor=devnum*MINORS_PER_DEVICE;
	dev->gendisk->minors=MINORS_PER_DEVICE;
	sprintf(dev->gendisk->disk_name, "%s", dev->class_dev->class_id);
	dev->gendisk->fops=&nexus_ops;
	dev->gendisk->queue=dev->queue;
	dev->gendisk->private_data=dev;
	set_capacity(dev->gendisk, capacity);
	
	ndebug("Adding disk");
	/* add_disk() initiates I/O to read the partition tables, so userspace
	   needs to be able to process key requests while it is running.
	   If we called add_disk() directly here, we would deadlock. */
	INIT_WORK(&dev->cb_add_disk, nexus_add_disk, dev);
	/* Make sure the dev isn't freed until add_disk() completes */
	nexus_dev_get(dev);
	/* We use the shared queue in order to prevent deadlock: if we
	   used our own queue, add_disk() would block its own I/O to the
	   partition table. */
	if (!schedule_work(&dev->cb_add_disk))
		BUG();
	
	return dev;
	
bad_put_chunkdata:
	/* Once chunkdata has been started, there's an extra reference to
	   the dev that needs to be released or it won't be freed. */
	nexus_dev_put(dev, 0);
bad:
	spin_lock(&state.lock);
	list_del_init(&dev->lh_devs);
	spin_unlock(&state.lock);
	nexus_dev_put(dev, 1);
	return ERR_PTR(ret);
	/* Until we have a refcount, we can't fail through the destructor */
early_fail_classdev:
	kfree(dev);
early_fail_devalloc:
	free_devnum(devnum);
early_fail_devnum:
	module_put(THIS_MODULE);
early_fail_module:
	return ERR_PTR(ret);
}

static int __init nexus_init(void)
{
	int ret;
	
	debug("===================================================");
	log(KERN_INFO, "loading (%s, rev %s)", svn_branch, svn_revision);
	
	spin_lock_init(&state.lock);
	INIT_LIST_HEAD(&state.devs);
	
	ret=request_start();
	if (ret)
		goto bad_request;
	
	/* class_create() doesn't let us preregister class attrs and doesn't
	   exist before 2.6.13 */
	class.name=DEVICE_NAME;
	class_set_owner(&class);
	class.class_release=class_release_dummy;
	class.release=nexus_dev_dtr;
	class.class_attrs=class_attrs;
	class.class_dev_attrs=class_dev_attrs;
	ret=class_register(&class);
	if (ret)
		goto bad_class;
	
	ret=chunkdata_start();
	if (ret) {
		log(KERN_ERR, "couldn't set up chunkdata");
		goto bad_chunkdata;
	}
	
	wkqueue=create_workqueue(SUBMIT_QUEUE);
	if (wkqueue == NULL) {
		ret=-ENOMEM;
		log(KERN_ERR, "couldn't start workqueue");
		goto bad_workqueue;
	}
	
	ret=thread_start();
	if (ret) {
		log(KERN_ERR, "couldn't start kernel threads");
		goto bad_thread;
	}
	
	ret=register_blkdev(0, MODULE_NAME);
	if (ret < 0) {
		log(KERN_ERR, "block driver registration failed");
		goto bad_blkdev;
	}
	blk_major=ret;
	
	ret=chardev_start();
	if (ret) {
		log(KERN_ERR, "couldn't register chardev");
		goto bad_chrdev;
	}
	
	return 0;

bad_chrdev:
	if (unregister_blkdev(blk_major, MODULE_NAME))
		log(KERN_ERR, "block driver unregistration failed");
bad_blkdev:
	thread_shutdown();
bad_thread:
	destroy_workqueue(wkqueue);
bad_workqueue:
	chunkdata_shutdown();
bad_chunkdata:
	class_unregister(&class);
bad_class:
	request_shutdown();
bad_request:
	return ret;
}

static void __exit nexus_shutdown(void)
{
	log(KERN_INFO, "unloading");
	
	chardev_shutdown();
	
	if (unregister_blkdev(blk_major, MODULE_NAME))
		log(KERN_ERR, "block driver unregistration failed");
	
	thread_shutdown();
	
	destroy_workqueue(wkqueue);
	
	chunkdata_shutdown();
	
	class_unregister(&class);
	
	request_shutdown();
}

module_init(nexus_init);
module_exit(nexus_shutdown);

MODULE_AUTHOR("Benjamin Gilbert <bgilbert@cs.cmu.edu>");
MODULE_DESCRIPTION("OpenISR virtual block device");
/* We must use a GPL-compatible license to use the crypto API */
MODULE_LICENSE("GPL");
