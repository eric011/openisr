/* sysfs.c - sysfs attribute functions */

/* 
 * Nexus - convergently encrypting virtual disk driver for the OpenISR (R)
 *         system
 * 
 * Copyright (C) 2006-2007 Carnegie Mellon University
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.  A copy of the GNU General Public License
 * should have been distributed along with this program in the file
 * LICENSE.GPL.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/device.h>
#include "defs.h"

static ssize_t drv_show_version(struct class *c, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", NEXUS_INTERFACE_VERSION);
}

static ssize_t drv_show_release(struct class *c, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", isr_release);
}

static ssize_t drv_show_revision(struct class *c, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", rcs_revision);
}

#ifdef DEBUG
static ssize_t drv_show_debug(struct class *c, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n", debug_mask);
}

static ssize_t drv_store_debug(struct class *c, const char *buf, size_t len)
{
	char *endp;
	unsigned tmp;
	
	/* Make sure simple_strtoul() won't overrun the buffer */
	if (len >= PAGE_SIZE)
		return -EINVAL;
	BUG_ON(buf[len] != 0);
	
	tmp=simple_strtol(buf, &endp, 0);
	if (endp[0] != '\n' || endp[1] != 0)
		return -EINVAL;
	debug_mask=tmp;
	return len;
}
#endif /* DEBUG */

struct class_attribute class_attrs[] = {
	__ATTR(version, S_IRUGO, drv_show_version, NULL),
	__ATTR(release, S_IRUGO, drv_show_release, NULL),
	__ATTR(revision, S_IRUGO, drv_show_revision, NULL),
#ifdef DEBUG
	__ATTR(debug_mask, S_IRUGO|S_IWUSR, drv_show_debug, drv_store_debug),
#endif
	__ATTR_NULL
};



/* For these functions, the caller holds a reference to the kdevice, so we
   know the nexus_dev is valid.  These functions cannot run until after device
   initialization has finished, but may run before the gendisk is live. */

static declare_kdevice_show(dev_show_ident, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%s\n", dev->ident);
}

static declare_kdevice_show(dev_show_owner, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->owner);
}

static declare_kdevice_show(dev_show_chunksize, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->chunksize);
}

static declare_kdevice_show(dev_show_cachesize, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->cachesize);
}

static declare_kdevice_show(dev_show_offset, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%llu\n", (u64)dev->offset << 9);
}

static declare_kdevice_show(dev_show_states, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	int i;
	int count=0;
	
	/* We don't take the device mutex.  This allows the state dump to be
	   inconsistent, but also permits dumping if someone died holding
	   the mutex */
	for (i=0; i<CD_NR_STATES; i++) {
		count += snprintf(buf+count, PAGE_SIZE-count, "%s%u",
					i ? " " : "",
					dev->stats.state_count[i]);
	}
	count += snprintf(buf+count, PAGE_SIZE-count, "\n");
	return count;
}

static declare_kdevice_show(dev_show_state_times, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	int i;
	int count=0;
	unsigned time;
	unsigned samples;
	
	/* -ERESTARTSYS doesn't work here */
	mutex_lock(&dev->lock);
	for (i=0; i<CD_NR_STATES; i++) {
		time=dev->stats.state_time_us[i];
		samples=dev->stats.state_time_samples[i];
		count += snprintf(buf+count, PAGE_SIZE-count, "%s%u",
					i ? " " : "",
					samples ? time / samples : 0);
	}
	count += snprintf(buf+count, PAGE_SIZE-count, "\n");
	mutex_unlock(&dev->lock);
	return count;
}

static declare_kdevice_store(dev_store_state_times, kdevice, buf, len)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	int i;
	
	/* -ERESTARTSYS doesn't work here */
	mutex_lock(&dev->lock);
	for (i=0; i<CD_NR_STATES; i++) {
		dev->stats.state_time_us[i]=0;
		dev->stats.state_time_samples[i]=0;
	}
	mutex_unlock(&dev->lock);
	return len;
}

static declare_kdevice_show(dev_show_suite, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%s\n",
				suite_info(dev->suite)->user_name);
}

static declare_kdevice_show(dev_show_compression, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%s\n",
			compress_info(dev->default_compression)->user_name);
}

static declare_kdevice_show(dev_show_cache_hits, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.cache_hits);
}

static declare_kdevice_show(dev_show_cache_misses, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.cache_misses);
}

static declare_kdevice_show(dev_show_cache_fails, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n",
				dev->stats.cache_alloc_failures);
}

static declare_kdevice_show(dev_show_chunk_errors, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.chunk_errors);
}

static declare_kdevice_show(dev_show_chunk_reads, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.chunk_reads);
}

static declare_kdevice_show(dev_show_chunk_writes, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.chunk_writes);
}

static declare_kdevice_show(dev_show_comp_ratio, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	u64 bytes;
	unsigned writes;
	unsigned scaled_pct;
	
	/* -ERESTARTSYS doesn't work here */
	mutex_lock(&dev->lock);
	bytes=dev->stats.data_bytes_written;
	writes=dev->stats.chunk_writes;
	mutex_unlock(&dev->lock);
	if (writes == 0)
		return snprintf(buf, PAGE_SIZE, "n/a\n");
	do_div(bytes, writes);
	scaled_pct=bytes;
	scaled_pct = (scaled_pct * 1000) / dev->chunksize;
	return snprintf(buf, PAGE_SIZE, "%u.%u\n", scaled_pct / 10,
				scaled_pct % 10);
}

static declare_kdevice_show(dev_show_whole_writes, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.whole_chunk_updates);
}

static declare_kdevice_show(dev_show_discards, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.encrypted_discards);
}

static declare_kdevice_show(dev_show_sect_read, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.sectors_read);
}

static declare_kdevice_show(dev_show_sect_written, kdevice, buf)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	return snprintf(buf, PAGE_SIZE, "%u\n", dev->stats.sectors_written);
}

static declare_kdevice_store(dev_store_action, kdevice, buf, len)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	int ret;
	
	if (!strcmp(buf, "kick-cache\n")) {
		/* -ERESTARTSYS doesn't work here */
		mutex_lock(&dev->lock);
		run_all_chunks(dev);
		mutex_unlock(&dev->lock);
	} else if (!strcmp(buf, "kick-threads\n")) {
		/* XXX technically this shouldn't go in the per-device
		   sysfs directory */
		wake_all_threads();
	} else if (!strcmp(buf, "kick-elevator\n")) {
		kick_elevator(dev);
	} else if (!strcmp(buf, "kill\n")) {
		ret=-ENXIO;
		mutex_lock(&dev->lock);
		if (!dev_is_shutdown(dev)) {
			log(KERN_NOTICE, "Forcing shutdown of " DEVICE_NAME
						"%c", 'a' + dev->devnum);
			ret=shutdown_dev(dev, 1);
		}
		mutex_unlock(&dev->lock);
		if (ret)
			return ret;
	} else if (!strcmp(buf, "sync\n")) {
		/* This returns -ERESTARTSYS if interrupted by a signal, but,
		   of course, we can't do anything about it. */
		nexus_sync(dev);
	} else if (!strcmp(buf, "drop-cache\n")) {
		mutex_lock(&dev->lock);
		chunkdata_invalidate_all(dev);
		mutex_unlock(&dev->lock);
	} else {
		return -EINVAL;
	}
	return len;
}

kdevice_attribute_t kdevice_attrs[] = {
	__ATTR(ident, S_IRUGO, dev_show_ident, NULL),
	__ATTR(owner, S_IRUGO, dev_show_owner, NULL),
	__ATTR(chunk_size, S_IRUGO, dev_show_chunksize, NULL),
	__ATTR(cache_entries, S_IRUGO, dev_show_cachesize, NULL),
	__ATTR(header_length, S_IRUGO, dev_show_offset, NULL),
	__ATTR(states, S_IRUGO, dev_show_states, NULL),
	__ATTR(state_times, S_IRUGO|S_IWUGO, dev_show_state_times,
				dev_store_state_times),
	__ATTR(encryption, S_IRUGO, dev_show_suite, NULL),
	__ATTR(compression, S_IRUGO, dev_show_compression, NULL),
	__ATTR(cache_hits, S_IRUGO, dev_show_cache_hits, NULL),
	__ATTR(cache_misses, S_IRUGO, dev_show_cache_misses, NULL),
	__ATTR(cache_alloc_failures, S_IRUGO, dev_show_cache_fails, NULL),
	__ATTR(chunk_errors, S_IRUGO, dev_show_chunk_errors, NULL),
	__ATTR(chunk_reads, S_IRUGO, dev_show_chunk_reads, NULL),
	__ATTR(chunk_writes, S_IRUGO, dev_show_chunk_writes, NULL),
	__ATTR(compression_ratio_pct, S_IRUGO, dev_show_comp_ratio, NULL),
	__ATTR(whole_chunk_updates, S_IRUGO, dev_show_whole_writes, NULL),
	__ATTR(chunk_encrypted_discards, S_IRUGO, dev_show_discards, NULL),
	__ATTR(sectors_read, S_IRUGO, dev_show_sect_read, NULL),
	__ATTR(sectors_written, S_IRUGO, dev_show_sect_written, NULL),
	__ATTR(action, S_IWUSR, NULL, dev_store_action),
	__ATTR_NULL
};
