/* 
 * Nexus - convergently encrypting virtual disk driver for the OpenISR (TM)
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

#include <linux/kthread.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/notifier.h>
#include <linux/interrupt.h>
#include "defs.h"

/* XXX percpu vars */
static struct {
	MUTEX lock;
	struct task_struct *task[NR_CPUS];
	int count;
	struct nexus_tfm_state ts[NR_CPUS];
	unsigned suite_users[NEXUS_NR_CRYPTO];
	unsigned compress_users[NEXUS_NR_COMPRESS];
} threads;

static struct {
	spinlock_t lock;       /* may be taken in interrupt context */
	struct list_head list[NR_CALLBACKS];
	wait_queue_head_t wq;
} queues;

static struct {
	spinlock_t lock;
	struct bio *head;
	struct bio *tail;
	wait_queue_head_t wq;
} pending_io;

static struct {
	spinlock_t lock;
	struct list_head list;
	wait_queue_head_t wq;
} pending_requests;

static struct task_struct *io_thread;
static struct task_struct *request_thread;


/* This will always run on the processor to which it is bound, *except* during
   hot-unplug of that CPU (or in a corner case on <= 2.6.9), when it will run
   on an arbitrary processor. */
static int nexus_thread(void *data)
{
	struct nexus_tfm_state *ts=data;
	enum callback type;
	struct list_head *entry;
	DEFINE_WAIT(wait);
	
	while (!kthread_should_stop()) {
		spin_lock_irq(&queues.lock);
		for (type=0; type<NR_CALLBACKS; type++) {
			if (!list_empty(&queues.list[type])) {
				entry=queues.list[type].next;
				list_del_init(entry);
				spin_unlock_irq(&queues.lock);
				
				switch (type) {
				case CB_COMPLETE_IO:
					chunkdata_complete_io(entry);
					break;
				case CB_UPDATE_CHUNK:
					run_chunk(entry);
					break;
				case CB_CRYPTO:
					chunk_tfm(ts, entry);
					break;
				case NR_CALLBACKS:
					BUG();
				}
				cond_resched();
				goto next;
			}
		}
		
		/* No pending callbacks */
		prepare_to_wait_exclusive(&queues.wq, &wait,
					TASK_INTERRUPTIBLE);
		spin_unlock_irq(&queues.lock);
		if (!kthread_should_stop())
			schedule();
		finish_wait(&queues.wq, &wait);
next:
		try_to_freeze();
	}
	return 0;
}

void schedule_callback(enum callback type, struct list_head *entry)
{
	unsigned long flags;
	
	BUG_ON(type < 0 || type >= NR_CALLBACKS);
	BUG_ON(!list_empty(entry));
	spin_lock_irqsave(&queues.lock, flags);
	list_add_tail(entry, &queues.list[type]);
	spin_unlock_irqrestore(&queues.lock, flags);
	wake_up_interruptible(&queues.wq);
}

/* Helper thread to submit I/O.  We don't want to do this in the per-CPU
   thread because it's allowed to block if there are already too many
   outstanding requests to the chunk store, and we want to be able to continue
   to do crypto and service other requests while we wait. */
/* Technically we could spawn one thread per device so that a blocked queue
   for one chunk store won't affect unrelated devices, but we have too many
   threads already.  This can be changed later if it becomes a problem. */
static int nexus_io_thread(void *ignored)
{
	struct bio *bio;
	DEFINE_WAIT(wait);
	
	while (!kthread_should_stop()) {
		spin_lock(&pending_io.lock);
		bio=pending_io.head;
		if (bio != NULL) {
			pending_io.head=bio->bi_next;
			bio->bi_next=NULL;
		} else {
			prepare_to_wait_exclusive(&pending_io.wq, &wait,
						TASK_INTERRUPTIBLE);
		}
		spin_unlock(&pending_io.lock);
		
		if (bio != NULL) {
			generic_make_request(bio);
		} else {
			if (!kthread_should_stop())
				schedule();
			finish_wait(&queues.wq, &wait);
		}
		try_to_freeze();
	}
	return 0;
}

/* We don't want to have to do a memory allocation to queue I/O, so we need
   to use the linked list mechanism already included in struct bio.
   Unfortunately, this is just a "next" pointer rather than a
   struct list_head. */
void schedule_io(struct bio *bio)
{
	BUG_ON(bio->bi_next != NULL);
	/* We don't use _bh or _irq spinlock variants */
	BUG_ON(in_interrupt());
	spin_lock(&pending_io.lock);
	if (pending_io.head == NULL) {
		pending_io.head=bio;
		pending_io.tail=bio;
	} else {
		pending_io.tail->bi_next=bio;
		pending_io.tail=bio;
	}
	spin_unlock(&pending_io.lock);
	wake_up_interruptible(&pending_io.wq);
}

/* Helper thread to run request queues.  Request queues are different from
   other types of callbacks: the request queue code needs to be able to return
   callbacks to the head of the queue if an allocation failure occurs, and
   this operation must always preserve queue order; it needs to be able to
   delay walking the queue if there's an out-of-memory condition; and we
   need to be able to process one dev's requests even if another dev is out of
   chunkdata buffers.  Therefore, we use a two-stage queue walk: there's a
   per-dev request list, and one callback processes the entire per-dev list at
   once (in request.c).

   In order to ensure that allocation failures do not reorder requests in a
   particular dev's list, we must make sure that only one thread can process
   a dev's request list at a time.  We could do this in the per-CPU crypto
   threads using a per-dev lock, but then we'd have to choose between:
   complex code, race conditions, or allowing crypto threads to uselessly
   block on a dev mutex when they could be getting work done.  For simplicity,
   therefore, we only allow one thread to be processing request queues at a
   time.  There's no clean way to do that within the per-CPU thread
   architecture, so we have a special singleton thread for this purpose.  This
   is separate from the I/O thread because we still want to process incoming
   requests even if our underlying chunk store's request queue is full. */
static int nexus_request_thread(void *ignored)
{
	struct list_head *entry;
	DEFINE_WAIT(wait);
	
	while (!kthread_should_stop()) {
		spin_lock_irq(&pending_requests.lock);
		if (!list_empty(&pending_requests.list)) {
			entry=pending_requests.list.next;
			list_del_init(entry);
			spin_unlock_irq(&pending_requests.lock);
			nexus_run_requests(entry);
		} else {
			prepare_to_wait_exclusive(&pending_requests.wq, &wait,
						TASK_INTERRUPTIBLE);
			spin_unlock_irq(&pending_requests.lock);
			if (!kthread_should_stop())
				schedule();
			finish_wait(&pending_requests.wq, &wait);
		}
		try_to_freeze();
	}
	return 0;
}

void schedule_request_callback(struct list_head *entry)
{
	unsigned long flags;
	
	BUG_ON(!list_empty(entry));
	spin_lock_irqsave(&pending_requests.lock, flags);
	list_add_tail(entry, &pending_requests.list);
	spin_unlock_irqrestore(&pending_requests.lock, flags);
	wake_up_interruptible(&pending_requests.wq);
}

/* Only for debug use via sysfs */
void wake_all_threads(void)
{
	log(KERN_NOTICE, "Unwedging threads");
	wake_up_all(&queues.wq);
	wake_up_all(&pending_io.wq);
	wake_up_all(&pending_requests.wq);
}

#define nexus_suite nexus_crypto

#define DEFINE_ALLOC_ON_ALL(TYPE)					\
static int alloc_##TYPE##_on_all(enum nexus_##TYPE arg)			\
{									\
	int cpu;							\
	int count;							\
	int err;							\
									\
	BUG_ON(!mutex_is_locked(&threads.lock));			\
	debug(DBG_THREAD, "Allocating " #TYPE " %s...",			\
				TYPE##_info(arg)->user_name);		\
	for (cpu=0, count=0; cpu<NR_CPUS; cpu++) {			\
		/* We care about which threads are running, not which	\
		   CPUs are online */					\
		if (threads.task[cpu] == NULL)				\
			continue;					\
		err=TYPE##_add(&threads.ts[cpu], arg);			\
		if (err)						\
			goto bad;					\
		count++;						\
	}								\
	debug(DBG_THREAD, "...allocated on %d cpus", count);		\
	return 0;							\
									\
bad:									\
	while (--cpu >= 0) {						\
		if (threads.task[cpu] != NULL)				\
			TYPE##_remove(&threads.ts[cpu], arg);		\
	}								\
	return err;							\
}

#define DEFINE_FREE_ON_ALL(TYPE)					\
static void free_##TYPE##_on_all(enum nexus_##TYPE arg)			\
{									\
	int cpu;							\
	int count;							\
									\
	BUG_ON(!mutex_is_locked(&threads.lock));			\
	debug(DBG_THREAD, "Freeing " #TYPE " %s...",			\
				TYPE##_info(arg)->user_name);		\
	for (cpu=0, count=0; cpu<NR_CPUS; cpu++) {			\
		if (threads.task[cpu] == NULL)				\
			continue;					\
		TYPE##_remove(&threads.ts[cpu], arg);			\
		count++;						\
	}								\
	debug(DBG_THREAD, "...freed on %d cpus", count);				\
}

DEFINE_ALLOC_ON_ALL(suite)
DEFINE_FREE_ON_ALL(suite)
DEFINE_ALLOC_ON_ALL(compress)
DEFINE_FREE_ON_ALL(compress)

#undef DEFINE_ALLOC_ON_ALL
#undef DEFINE_FREE_ON_ALL
#undef nexus_suite

static int alloc_all_on_cpu(int cpu)
{
	enum nexus_crypto suite=0;
	enum nexus_compress alg=0;
	int suite_count;
	int alg_count;
	int err;
	
	BUG_ON(!mutex_is_locked(&threads.lock));
	
	for (suite_count=0; suite<NEXUS_NR_CRYPTO; suite++) {
		if (threads.suite_users[suite]) {
			err=suite_add(&threads.ts[cpu], suite);
			if (err)
				goto bad;
			suite_count++;
		}
	}
	for (alg_count=0; alg<NEXUS_NR_COMPRESS; alg++) {
		if (threads.compress_users[alg]) {
			err=compress_add(&threads.ts[cpu], alg);
			if (err)
				goto bad;
			alg_count++;
		}
	}
	debug(DBG_THREAD, "Allocated %d suites and %d compression algorithms "
				"for cpu %d", suite_count, alg_count, cpu);
	return 0;
	
bad:
	/* gcc makes enums unsigned.  Rather than making assumptions, we test
	   for both signed and unsigned underflow. */
	while (--alg >= 0 && alg < NEXUS_NR_COMPRESS) {
		if (threads.compress_users[alg])
			compress_remove(&threads.ts[cpu], alg);
	}
	while (--suite >= 0 && suite < NEXUS_NR_CRYPTO) {
		if (threads.suite_users[suite])
			suite_remove(&threads.ts[cpu], suite);
	}
	return err;
}

static void free_all_on_cpu(int cpu)
{
	enum nexus_crypto suite;
	enum nexus_compress alg;
	int suite_count;
	int alg_count;
	
	BUG_ON(!mutex_is_locked(&threads.lock));
	
	for (suite=0, suite_count=0; suite<NEXUS_NR_CRYPTO; suite++) {
		if (threads.suite_users[suite]) {
			suite_remove(&threads.ts[cpu], suite);
			suite_count++;
		}
	}
	for (alg=0, alg_count=0; alg<NEXUS_NR_COMPRESS; alg++) {
		if (threads.compress_users[alg]) {
			compress_remove(&threads.ts[cpu], alg);
			alg_count++;
		}
	}
	debug(DBG_THREAD, "Freed %d suites and %d compression algorithms for "
				"cpu %d", suite_count, alg_count, cpu);
}

int thread_register(struct nexus_dev *dev)
{
	enum nexus_compress alg;
	int err;
	
	err=transform_validate(dev);
	if (err)
		return err;
	
	/* We could use the interruptible variant and fail the device ctr
	   if we get a signal, but that seems sorta stupid. */
	mutex_lock(&threads.lock);
	
	/* Register suite */
	if (threads.suite_users[dev->suite] == 0) {
		err=alloc_suite_on_all(dev->suite);
		if (err)
			goto bad;
	}
	threads.suite_users[dev->suite]++;
	
	/* Register compression */
	for (alg=0; alg<NEXUS_NR_COMPRESS; alg++) {
		if (dev->supported_compression & (1 << alg)) {
			if (threads.compress_users[alg] == 0) {
				err=alloc_compress_on_all(alg);
				if (err)
					goto bad_dealloc;
			}
			threads.compress_users[alg]++;
		}
	}
	mutex_unlock(&threads.lock);
	
	if (test_and_set_bit(__DEV_THR_REGISTERED, &dev->flags))
		BUG();
	return 0;
	
bad_dealloc:
	/* gcc makes enums unsigned.  Rather than making assumptions, we test
	   for both signed and unsigned underflow. */
	while (--alg >= 0 && alg < NEXUS_NR_COMPRESS) {
		if (dev->supported_compression & (1 << alg)) {
			if (--threads.compress_users[alg] == 0)
				free_compress_on_all(alg);
		}
	}
	if (--threads.suite_users[dev->suite] == 0)
		free_suite_on_all(dev->suite);
bad:
	mutex_unlock(&threads.lock);
	return err;
}

void thread_unregister(struct nexus_dev *dev)
{
	enum nexus_compress alg;
	
	/* Avoid corrupting refcounts if the registration failed earlier */
	if (!test_and_clear_bit(__DEV_THR_REGISTERED, &dev->flags))
		return;
	
	mutex_lock(&threads.lock);
	
	/* Unregister suite */
	if (--threads.suite_users[dev->suite] == 0)
		free_suite_on_all(dev->suite);
	
	/* Unregister compression */
	for (alg=0; alg<NEXUS_NR_COMPRESS; alg++) {
		if (dev->supported_compression & (1 << alg)) {
			if (--threads.compress_users[alg] == 0)
				free_compress_on_all(alg);
		}
	}
	
	mutex_unlock(&threads.lock);
}

static int cpu_start(int cpu)
{
	struct task_struct *thr;
	int err;
	
	BUG_ON(!mutex_is_locked(&threads.lock));
	if (threads.task[cpu] != NULL) {
		/* This may happen in some hotplug cases.  Ignore the duplicate
		   start request. */
		return 0;
	}
	
	debug(DBG_THREAD, "Onlining CPU %d", cpu);
	err=alloc_all_on_cpu(cpu);
	if (err) {
		debug(DBG_THREAD, "Failed to allocate transforms for CPU %d",
					cpu);
		return err;
	}
	thr=kthread_create(nexus_thread, &threads.ts[cpu], KTHREAD_NAME "/%d",
				cpu);
	if (IS_ERR(thr)) {
		free_all_on_cpu(cpu);
		return PTR_ERR(thr);
	}
	threads.task[cpu]=thr;
	threads.count++;
	kthread_bind(thr, cpu);
	/* Give the thread a lower priority than garden-variety interactive
	   processes so that we don't kill their scheduling latency */
	set_user_nice(thr, 5);
	wake_up_process(thr);
	return 0;
}

static void cpu_stop(int cpu)
{
	BUG_ON(!mutex_is_locked(&threads.lock));
	if (threads.task[cpu] == NULL)
		return;
	
	debug(DBG_THREAD, "Offlining CPU %d", cpu);
	kthread_stop(threads.task[cpu]);
	debug(DBG_THREAD, "...done");
	free_all_on_cpu(cpu);
	threads.task[cpu]=NULL;
	threads.count--;
}

/* Runs in process context; can sleep */
static int cpu_callback(struct notifier_block *nb, unsigned long action,
			void *data)
{
	int cpu=(long)data;
	
	/* Any of these handlers may run before thread_start has actually
	   started any threads, so they must not make assumptions about the
	   state of the system. */
	mutex_lock(&threads.lock);
	switch (action) {
	case CPU_ONLINE:
		/* CPU is already up */
		if (cpu_start(cpu))
			log(KERN_ERR, "Failed to start thread for CPU %d", cpu);
		break;
	case CPU_DOWN_PREPARE:
		/* This is only called for >= 2.6.10 */
		if (threads.count == 1 && threads.task[cpu] != NULL) {
			/* This is the last CPU on which we have a running
			   thread, since we were unable to start a thread
			   for a new CPU at some point in the past.  Cancel
			   the shutdown. */
			log(KERN_ERR, "Refusing to stop CPU %d: it is running "
						"our last worker thread", cpu);
			mutex_unlock(&threads.lock);
			return NOTIFY_BAD;
		}
		break;
	case CPU_DEAD:
		/* CPU is already down */
		if (threads.count == 1) {
			/* CPU_DOWN_PREPARE will prevent this for kernels
			   >= 2.6.10.  For older kernels, we kludge: just
			   allow the thread to keep running.  It will continue
			   to use the downed CPU's data structure but will
			   run on any available processor.  If more hotplug
			   events follow, this thread will compete for CPU time
			   with any thread which is *supposed* to be running
			   on a given CPU.  However, correctness should not
			   be compromised.  This is not worth fixing
			   properly. */
			log(KERN_NOTICE, "Disabled CPU %d, which was running "
						"our last worker thread", cpu);
			log(KERN_NOTICE, "Leaving "KTHREAD_NAME"/%d running "
						"without CPU affinity", cpu);
			break;
		}
		cpu_stop(cpu);
		/* Make sure someone takes over any work the downed thread
		   was about to do */
		wake_up_interruptible(&queues.wq);
		break;
	}
	mutex_unlock(&threads.lock);
	return NOTIFY_OK;
}

static struct notifier_block cpu_notifier = {
	.notifier_call = cpu_callback
};

#if !defined(CONFIG_HOTPLUG_CPU) && \
			LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) && \
			LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
/* 2.6.18 and 2.6.19 have a bug with !CONFIG_HOTPLUG_CPU that causes the
   compiler to complain that cpu_notifier is unused.  Avoid the warning. */
struct notifier_block *__dummy_nb=&cpu_notifier;
#endif

void thread_shutdown(void)
{
	int cpu;
	
	/* unregister_hotcpu_notifier must be called unlocked, in case the
	   notifier chain is currently running */
	unregister_hotcpu_notifier(&cpu_notifier);
	mutex_lock(&threads.lock);
	for_each_possible_cpu(cpu)
		cpu_stop(cpu);
	mutex_unlock(&threads.lock);
	
	if (io_thread != NULL) {
		debug(DBG_THREAD, "Stopping I/O thread");
		kthread_stop(io_thread);
		debug(DBG_THREAD, "...done");
		io_thread=NULL;
	}
	if (request_thread != NULL) {
		debug(DBG_THREAD, "Stopping request thread");
		kthread_stop(request_thread);
		debug(DBG_THREAD, "...done");
		request_thread=NULL;
	}
}

int __init thread_start(void)
{
	struct task_struct *thr;
	int cpu;
	int ret=0;
	int i;
	
	spin_lock_init(&queues.lock);
	for (i=0; i<NR_CALLBACKS; i++)
		INIT_LIST_HEAD(&queues.list[i]);
	init_waitqueue_head(&queues.wq);
	mutex_init(&threads.lock);
	spin_lock_init(&pending_io.lock);
	init_waitqueue_head(&pending_io.wq);
	spin_lock_init(&pending_requests.lock);
	INIT_LIST_HEAD(&pending_requests.list);
	init_waitqueue_head(&pending_requests.wq);
	
	/* Lock-ordering issues dictate the order of these calls.  (2.6.19
	   takes the hotplug lock in register_hotcpu_notifier(), and we must
	   take the hotplug lock before threads.lock for consistency with
	   cpu_callback().)  As a result, we may get a callback before we
	   actually start any threads. */
	register_hotcpu_notifier(&cpu_notifier);
	lock_cpu_hotplug();
	mutex_lock(&threads.lock);
	for_each_online_cpu(cpu) {
		ret=cpu_start(cpu);
		if (ret)
			break;
	}
	mutex_unlock(&threads.lock);
	unlock_cpu_hotplug();
	if (ret)
		goto bad;
	
	debug(DBG_THREAD, "Starting singleton threads");
	thr=kthread_create(nexus_io_thread, NULL, IOTHREAD_NAME);
	if (IS_ERR(thr)) {
		ret=PTR_ERR(thr);
		goto bad;
	}
	io_thread=thr;
	wake_up_process(thr);
	thr=kthread_create(nexus_request_thread, NULL, REQTHREAD_NAME);
	if (IS_ERR(thr)) {
		ret=PTR_ERR(thr);
		goto bad;
	}
	/* Make sure the request thread doesn't have a higher priority than
	   interactive processes.  This is not hugely necessary but seems to
	   improve scheduling latency a little bit. */
	set_user_nice(thr, 0);
	request_thread=thr;
	wake_up_process(thr);
	
	return 0;
	
bad:
	thread_shutdown();
	return ret;
}
