/*
 * fs/fs-writeback.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains all the functions related to writing back and waiting
 * upon dirty inodes against superblocks, and writing back dirty
 * pages against inodes.  ie: data writeback.  Writeout of the
 * inode itself is not handled here.
 *
 * 10Apr2002	Andrew Morton
 *		Split out of fs/inode.c
 *		Additions for address_space-based writeback
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/kthread.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/tracepoint.h>
#include <linux/device.h>
#include <linux/memcontrol.h>
#include "internal.h"
#if defined(OPLUS_FEATURE_IOMONITOR) && defined(CONFIG_IOMONITOR)
#include <linux/iomonitor/iomonitor.h>
#endif /*OPLUS_FEATURE_IOMONITOR*/

/*
 * 4MB minimal write chunk size
 */
#define MIN_WRITEBACK_PAGES	(4096UL >> (PAGE_SHIFT - 10))

struct wb_completion {
	atomic_t		cnt;
};

/*
 * Passed into wb_writeback(), essentially a subset of writeback_control
 */
struct wb_writeback_work {
	long nr_pages;
	struct super_block *sb;
	unsigned long *older_than_this;
	enum writeback_sync_modes sync_mode;
	unsigned int tagged_writepages:1;
	unsigned int for_kupdate:1;
	unsigned int range_cyclic:1;
	unsigned int for_background:1;
	unsigned int for_sync:1;	/* sync(2) WB_SYNC_ALL writeback */
	unsigned int auto_free:1;	/* free on completion */
	enum wb_reason reason;		/* why was writeback initiated? */

	struct list_head list;		/* pending work list */
	struct wb_completion *done;	/* set if the caller waits */
};

/*
 * If one wants to wait for one or more wb_writeback_works, each work's
 * ->done should be set to a wb_completion defined using the following
 * macro.  Once all work items are issued with wb_queue_work(), the caller
 * can wait for the completion of all using wb_wait_for_completion().  Work
 * items which are waited upon aren't freed automatically on completion.
 */
#define DEFINE_WB_COMPLETION_ONSTACK(cmpl)				\
	struct wb_completion cmpl = {					\
		.cnt		= ATOMIC_INIT(1),			\
	}


/*
 * If an inode is constantly having its pages dirtied, but then the
 * updates stop dirtytime_expire_interval seconds in the past, it's
 * possible for the worst case time between when an inode has its
 * timestamps updated and when they finally get written out to be two
 * dirtytime_expire_intervals.  We set the default to 12 hours (in
 * seconds), which means most of the time inodes will have their
 * timestamps written to disk after 12 hours, but in the worst case a
 * few inodes might not their timestamps updated for 24 hours.
 */
unsigned int dirtytime_expire_interval = 12 * 60 * 60;

static inline struct inode *wb_inode(struct list_head *head)
{
	return list_entry(head, struct inode, i_io_list);
}

/*
 * Include the creation of the trace points after defining the
 * wb_writeback_work structure and inline functions so that the definition
 * remains local to this file.
 */
#define CREATE_TRACE_POINTS
#include <trace/events/writeback.h>

EXPORT_TRACEPOINT_SYMBOL_GPL(wbc_writepage);

static bool wb_io_lists_populated(struct bdi_writeback *wb)
{
	if (wb_has_dirty_io(wb)) {
		return false;
	} else {
		set_bit(WB_has_dirty_io, &wb->state);
		WARN_ON_ONCE(!wb->avg_write_bandwidth);
		atomic_long_add(wb->avg_write_bandwidth,
				&wb->bdi->tot_write_bandwidth);
		return true;
	}
}

static void wb_io_lists_depopulated(struct bdi_writeback *wb)
{
	if (wb_has_dirty_io(wb) && list_empty(&wb->b_dirty) &&
	    list_empty(&wb->b_io) && list_empty(&wb->b_more_io)) {
		clear_bit(WB_has_dirty_io, &wb->state);
		WARN_ON_ONCE(atomic_long_sub_return(wb->avg_write_bandwidth,
					&wb->bdi->tot_write_bandwidth) < 0);
	}
}

/**
 * inode_io_list_move_locked - move an inode onto a bdi_writeback IO list
 * @inode: inode to be moved
 * @wb: target bdi_writeback
 * @head: one of @wb->b_{dirty|io|more_io|dirty_time}
 *
 * Move @inode->i_io_list to @list of @wb and set %WB_has_dirty_io.
 * Returns %true if @inode is the first occupant of the !dirty_time IO
 * lists; otherwise, %false.
 */
static bool inode_io_list_move_locked(struct inode *inode,
				      struct bdi_writeback *wb,
				      struct list_head *head)
{
	assert_spin_locked(&wb->list_lock);

	list_move(&inode->i_io_list, head);

	/* dirty_time doesn't count as dirty_io until expiration */
	if (head != &wb->b_dirty_time)
		return wb_io_lists_populated(wb);

	wb_io_lists_depopulated(wb);
	return false;
}

/**
 * inode_io_list_del_locked - remove an inode from its bdi_writeback IO list
 * @inode: inode to be removed
 * @wb: bdi_writeback @inode is being removed from
 *
 * Remove @inode which may be on one of @wb->b_{dirty|io|more_io} lists and
 * clear %WB_has_dirty_io if all are empty afterwards.
 */
static void inode_io_list_del_locked(struct inode *inode,
				     struct bdi_writeback *wb)
{
	assert_spin_locked(&wb->list_lock);

	list_del_init(&inode->i_io_list);
	wb_io_lists_depopulated(wb);
}

static void wb_wakeup(struct bdi_writeback *wb)
{
	spin_lock_bh(&wb->work_lock);
	if (test_bit(WB_registered, &wb->state))
		mod_delayed_work(bdi_wq, &wb->dwork, 0);
	spin_unlock_bh(&wb->work_lock);
}

static void finish_writeback_work(struct bdi_writeback *wb,
				  struct wb_writeback_work *work)
{
	struct wb_completion *done = work->done;

	if (work->auto_free)
		kfree(work);
	if (done && atomic_dec_and_test(&done->cnt))
		wake_up_all(&wb->bdi->wb_waitq);
}

static void wb_queue_work(struct bdi_writeback *wb,
			  struct wb_writeback_work *work)
{
	trace_writeback_queue(wb, work);

	if (work->done)
		atomic_inc(&work->done->cnt);

	spin_lock_bh(&wb->work_lock);

	if (test_bit(WB_registered, &wb->state)) {
		list_add_tail(&work->list, &wb->work_list);
		mod_delayed_work(bdi_wq, &wb->dwork, 0);
	} else
		finish_writeback_work(wb, work);

	spin_unlock_bh(&wb->work_lock);
}

/**
 * wb_wait_for_completion - wait for completion of bdi_writeback_works
 * @bdi: bdi work items were issued to
 * @done: target wb_completion
 *
 * Wait for one or more work items issued to @bdi with their ->done field
 * set to @done, which should have been defined with
 * DEFINE_WB_COMPLETION_ONSTACK().  This function returns after all such
 * work items are completed.  Work items which are waited upon aren't freed
 * automatically on completion.
 */
static void wb_wait_for_completion(struct backing_dev_info *bdi,
				   struct wb_completion *done)
{
	atomic_dec(&done->cnt);		/* put down the initial count */
	wait_event(bdi->wb_waitq, !atomic_read(&done->cnt));
}

#ifdef CONFIG_CGROUP_WRITEBACK

/* parameters for foreign inode detection, see wb_detach_inode() */
#define WB_FRN_TIME_SHIFT	13	/* 1s = 2^13, upto 8 secs w/ 16bit */
#define WB_FRN_TIME_AVG_SHIFT	3	/* avg = avg * 7/8 + new * 1/8 */
#define WB_FRN_TIME_CUT_DIV	2	/* ignore rounds < avg / 2 */
#define WB_FRN_TIME_PERIOD	(2 * (1 << WB_FRN_TIME_SHIFT))	/* 2s */

#define WB_FRN_HIST_SLOTS	16	/* inode->i_wb_frn_history is 16bit */
#define WB_FRN_HIST_UNIT	(WB_FRN_TIME_PERIOD / WB_FRN_HIST_SLOTS)
					/* each slot's duration is 2s / 16 */
#define WB_FRN_HIST_THR_SLOTS	(WB_FRN_HIST_SLOTS / 2)
					/* if foreign slots >= 8, switch */
#define WB_FRN_HIST_MAX_SLOTS	(WB_FRN_HIST_THR_SLOTS / 2 + 1)
					/* one round can affect upto 5 slots */

static atomic_t isw_nr_in_flight = ATOMIC_INIT(0);
static struct workqueue_struct *isw_wq;

void __inode_attach_wb(struct inode *inode, struct page *page)
{
	struct backing_dev_info *bdi = inode_to_bdi(inode);
	struct bdi_writeback *wb = NULL;

	if (inode_cgwb_enabled(inode)) {
		struct cgroup_subsys_state *memcg_css;

		if (page) {
			memcg_css = mem_cgroup_css_from_page(page);
			wb = wb_get_create(bdi, memcg_css, GFP_ATOMIC);
		} else {
			/* must pin memcg_css, see wb_get_create() */
			memcg_css = task_get_css(current, memory_cgrp_id);
			wb = wb_get_create(bdi, memcg_css, GFP_ATOMIC);
			css_put(memcg_css);
		}
	}

	if (!wb)
		wb = &bdi->wb;

	/*
	 * There may be multiple instances of this function racing to
	 * update the same inode.  Use cmpxchg() to tell the winner.
	 */
	if (unlikely(cmpxchg(&inode->i_wb, NULL, wb)))
		wb_put(wb);
}

/**
 * locked_inode_to_wb_and_lock_list - determine a locked inode's wb and lock it
 * @inode: inode of interest with i_lock held
 *
 * Returns @inode's wb with its list_lock held.  @inode->i_lock must be
 * held on entry and is released on return.  The returned wb is guaranteed
 * to stay @inode's associated wb until its list_lock is released.
 */
static struct bdi_writeback *
locked_inode_to_wb_and_lock_list(struct inode *inode)
	__releases(&inode->i_lock)
	__acquires(&wb->list_lock)
{
	while (true) {
		struct bdi_writeback *wb = inode_to_wb(inode);

		/*
		 * inode_to_wb() association is protected by both
		 * @inode->i_lock and @wb->list_lock but list_lock nests
		 * outside i_lock.  Drop i_lock and verify that the
		 * association hasn't changed after acquiring list_lock.
		 */
		wb_get(wb);
		spin_unlock(&inode->i_lock);
		spin_lock(&wb->list_lock);

		/* i_wb may have changed inbetween, can't use inode_to_wb() */
		if (likely(wb == inode->i_wb)) {
			wb_put(wb);	/* @inode already has ref */
			return wb;
		}

		spin_unlock(&wb->list_lock);
		wb_put(wb);
		cpu_relax();
		spin_lock(&inode->i_lock);
	}
}

/**
 * inode_to_wb_and_lock_list - determine an inode's wb and lock it
 * @inode: inode of interest
 *
 * Same as locked_inode_to_wb_and_lock_list() but @inode->i_lock isn't held
 * on entry.
 */
static struct bdi_writeback *inode_to_wb_and_lock_list(struct inode *inode)
	__acquires(&wb->list_lock)
{
	spin_lock(&inode->i_lock);
	return locked_inode_to_wb_and_lock_list(inode);
}

struct inode_switch_wbs_context {
	struct inode		*inode;
	struct bdi_writeback	*new_wb;

	struct rcu_head		rcu_head;
	struct work_struct	work;
};

static void bdi_down_write_wb_switch_rwsem(struct backing_dev_info *bdi)
{
	down_write(&bdi->wb_switch_rwsem);
}

static void bdi_up_write_wb_switch_rwsem(struct backing_dev_info *bdi)
{
	up_write(&bdi->wb_switch_rwsem);
}

static void inode_switch_wbs_work_fn(struct work_struct *work)
{
	struct inode_switch_wbs_context *isw =
		container_of(work, struct inode_switch_wbs_context, work);
	struct inode *inode = isw->inode;
	struct backing_dev_info *bdi = inode_to_bdi(inode);
	struct address_space *mapping = inode->i_mapping;
	struct bdi_writeback *old_wb = inode->i_wb;
	struct bdi_writeback *new_wb = isw->new_wb;
	struct radix_tree_iter iter;
	bool switched = false;
	void **slot;

	/*
	 * If @inode switches cgwb membership while sync_inodes_sb() is
	 * being issued, sync_inodes_sb() might miss it.  Synchronize.
	 */
	down_read(&bdi->wb_switch_rwsem);

	/*
	 * By the time control reaches here, RCU grace period has passed
	 * since I_WB_SWITCH assertion and all wb stat update transactions
	 * between unlocked_inode_to_wb_begin/end() are guaranteed to be
	 * synchronizing against the i_pages lock.
	 *
	 * Grabbing old_wb->list_lock, inode->i_lock and the i_pages lock
	 * gives us exclusion against all wb related operations on @inode
	 * including IO list manipulations and stat updates.
	 */
	if (old_wb < new_wb) {
		spin_lock(&old_wb->list_lock);
		spin_lock_nested(&new_wb->list_lock, SINGLE_DEPTH_NESTING);
	} else {
		spin_lock(&new_wb->list_lock);
		spin_lock_nested(&old_wb->list_lock, SINGLE_DEPTH_NESTING);
	}
	spin_lock(&inode->i_lock);
	xa_lock_irq(&mapping->i_pages);

	/*
	 * Once I_FREEING is visible under i_lock, the eviction path owns
	 * the inode and we shouldn't modify ->i_io_list.
	 */
	if (unlikely(inode->i_state & I_FREEING))
		goto skip_switch;

	/*
	 * Count and transfer stats.  Note that PAGECACHE_TAG_DIRTY points
	 * to possibly dirty pages while PAGECACHE_TAG_WRITEBACK points to
	 * pages actually under writeback.
	 */
	radix_tree_for_each_tagged(slot, &mapping->i_pages, &iter, 0,
				   PAGECACHE_TAG_DIRTY) {
		struct page *page = radix_tree_deref_slot_protected(slot,
						&mapping->i_pages.xa_lock);
		if (likely(page) && PageDirty(page)) {
			dec_wb_stat(old_wb, WB_RECLAIMABLE);
			inc_wb_stat(new_wb, WB_RECLAIMABLE);
		}
	}

	radix_tree_for_each_tagged(slot, &mapping->i_pages, &iter, 0,
				   PAGECACHE_TAG_WRITEBACK) {
		struct page *page = radix_tree_deref_slot_protected(slot,
						&mapping->i_pages.xa_lock);
		if (likely(page)) {
			WARN_ON_ONCE(!PageWriteback(page));
			dec_wb_stat(old_wb, WB_WRITEBACK);
			inc_wb_stat(new_wb, WB_WRITEBACK);
		}
	}

	wb_get(new_wb);

	/*
	 * Transfer to @new_wb's IO list if necessary.  The specific list
	 * @inode was on is ignored and the inode is put on ->b_dirty which
	 * is always correct including from ->b_dirty_time.  The transfer
	 * preserves @inode->dirtied_when ordering.
	 */
	if (!list_empty(&inode->i_io_list)) {
		struct inode *pos;

		inode_io_list_del_locked(inode, old_wb);
		inode->i_wb = new_wb;
		list_for_each_entry(pos, &new_wb->b_dirty, i_io_list)
			if (time_after_eq(inode->dirtied_when,
					  pos->dirtied_when))
				break;
		inode_io_list_move_locked(inode, new_wb, pos->i_io_list.prev);
	} else {
		inode->i_wb = new_wb;
	}

	/* ->i_wb_frn updates may race wbc_detach_inode() but doesn't matter */
	inode->i_wb_frn_winner = 0;
	inode->i_wb_frn_avg_time = 0;
	inode->i_wb_frn_history = 0;
	switched = true;
skip_switch:
	/*
	 * Paired with load_acquire in unlocked_inode_to_wb_begin() and
	 * ensures that the new wb is visible if they see !I_WB_SWITCH.
	 */
	smp_store_release(&inode->i_state, inode->i_state & ~I_WB_SWITCH);

	xa_unlock_irq(&mapping->i_pages);
	spin_unlock(&inode->i_lock);
	spin_unlock(&new_wb->list_lock);
	spin_unlock(&old_wb->list_lock);

	up_read(&bdi->wb_switch_rwsem);

	if (switched) {
		wb_wakeup(new_wb);
		wb_put(old_wb);
	}
	wb_put(new_wb);

	iput(inode);
	kfree(isw);

	atomic_dec(&isw_nr_in_flight);
}

static void inode_switch_wbs_rcu_fn(struct rcu_head *rcu_head)
{
	struct inode_switch_wbs_context *isw = container_of(rcu_head,
				struct inode_switch_wbs_context, rcu_head);

	/* needs to grab bh-unsafe locks, bounce to work item */
	INIT_WORK(&isw->work, inode_switch_wbs_work_fn);
	queue_work(isw_wq, &isw->work);
}

/**
 * inode_switch_wbs - change the wb association of an inode
 * @inode: target inode
 * @new_wb_id: ID of the new wb
 *
 * Switch @inode's wb association to the wb identified by @new_wb_id.  The
 * switching is performed asynchronously and may fail silently.
 */
static void inode_switch_wbs(struct inode *inode, int new_wb_id)
{
	struct backing_dev_info *bdi = inode_to_bdi(inode);
	struct cgroup_subsys_state *memcg_css;
	struct inode_switch_wbs_context *isw;

	/* noop if seems to be already in progress */
	if (inode->i_state & I_WB_SWITCH)
		return;

	/*
	 * Avoid starting new switches while sync_inodes_sb() is in
	 * progress.  Otherwise, if the down_write protected issue path
	 * blocks heavily, we might end up starting a large number of
	 * switches which will block on the rwsem.
	 */
	if (!down_read_trylock(&bdi->wb_switch_rwsem))
		return;

	isw = kzalloc(sizeof(*isw), GFP_ATOMIC);
	if (!isw)
		goto out_unlock;

	/* find and pin the new wb */
	rcu_read_lock();
	memcg_css = css_from_id(new_wb_id, &memory_cgrp_subsys);
	if (memcg_css)
		isw->new_wb = wb_get_create(bdi, memcg_css, GFP_ATOMIC);
	rcu_read_unlock();
	if (!isw->new_wb)
		goto out_free;

	/* while holding I_WB_SWITCH, no one else can update the association */
	spin_lock(&inode->i_lock);
	if (!(inode->i_sb->s_flags & SB_ACTIVE) ||
	    inode->i_state & (I_WB_SWITCH | I_FREEING) ||
	    inode_to_wb(inode) == isw->new_wb) {
		spin_unlock(&inode->i_lock);
		goto out_free;
	}
	inode->i_state |= I_WB_SWITCH;
	__iget(inode);
	spin_unlock(&inode->i_lock);

	isw->inode = inode;

	/*
	 * In addition to synchronizing among switchers, I_WB_SWITCH tells
	 * the RCU protected stat update paths to grab the i_page
	 * lock so that stat transfer can synchronize against them.
	 * Let's continue after I_WB_SWITCH is guaranteed to be visible.
	 */
	call_rcu(&isw->rcu_head, inode_switch_wbs_rcu_fn);

	atomic_inc(&isw_nr_in_flight);

	goto out_unlock;

out_free:
	if (isw->new_wb)
		wb_put(isw->new_wb);
	kfree(isw);
out_unlock:
	up_read(&bdi->wb_switch_rwsem);
}

/**
 * wbc_attach_and_unlock_inode - associate wbc with target inode and unlock it
 * @wbc: writeback_control of interest
 * @inode: target inode
 *
 * @inode is locked and about to be written back under the control of @wbc.
 * Record @inode's writeback context into @wbc and unlock the i_lock.  On
 * writeback completion, wbc_detach_inode() should be called.  This is used
 * to track the cgroup writeback context.
 */
void wbc_attach_and_unlock_inode(struct writeback_control *wbc,
				 struct inode *inode)
{
	if (!inode_cgwb_enabled(inode)) {
		spin_unlock(&inode->i_lock);
		return;
	}

	wbc->wb = inode_to_wb(inode);
	wbc->inode = inode;

	wbc->wb_id = wbc->wb->memcg_css->id;
	wbc->wb_lcand_id = inode->i_wb_frn_winner;
	wbc->wb_tcand_id = 0;
	wbc->wb_bytes = 0;
	wbc->wb_lcand_bytes = 0;
	wbc->wb_tcand_bytes = 0;

	wb_get(wbc->wb);
	spin_unlock(&inode->i_lock);

	/*
	 * A dying wb indicates that either the blkcg associated with the
	 * memcg changed or the associated memcg is dying.  In the first
	 * case, a replacement wb should already be available and we should
	 * refresh the wb immediately.  In the second case, trying to
	 * refresh will keep failing.
	 */
	if (unlikely(wb_dying(wbc->wb) && !css_is_dying(wbc->wb->memcg_css)))
		inode_switch_wbs(inode, wbc->wb_id);
}

/**
 * wbc_detach_inode - disassociate wbc from inode and perform foreign detection
 * @wbc: writeback_control of the just finished writeback
 *
 * To be called after a writeback attempt of an inode finishes and undoes
 * wbc_attach_and_unlock_inode().  Can be called under any context.
 *
 * As concurrent write sharing of an inode is expected to be very rare and
 * memcg only tracks page ownership on first-use basis severely confining
 * the usefulness of such sharing, cgroup writeback tracks ownership
 * per-inode.  While the support for concurrent write sharing of an inode
 * is deemed unnecessary, an inode being written to by different cgroups at
 * different points in time is a lot more common, and, more importantly,
 * charging only by first-use can too readily lead to grossly incorrect
 * behaviors (single foreign page can lead to gigabytes of writeback to be
 * incorrectly attributed).
 *
 * To resolve this issue, cgroup writeback detects the majority dirtier of
 * an inode and transfers the ownership to it.  To avoid unnnecessary
 * oscillation, the detection mechanism keeps track of history and gives
 * out the switch verdict only if the foreign usage pattern is stable over
 * a certain amount of time and/or writeback attempts.
 *
 * On each writeback attempt, @wbc tries to detect the majority writer
 * using Boyer-Moore majority vote algorithm.  In addition to the byte
 * count from the majority voting, it also counts the bytes written for the
 * current wb and the last round's winner wb (max of last round's current
 * wb, the winner from two rounds ago, and the last round's majority
 * candidate).  Keeping track of the historical winner helps the algorithm
 * to semi-reliably detect the most active writer even when it's not the
 * absolute majority.
 *
 * Once the winner of the round is determined, whether the winner is
 * foreign or not and how much IO time the round consumed is recorded in
 * inode->i_wb_frn_history.  If the amount of recorded foreign IO time is
 * over a certain threshold, the switch verdict is given.
 */
void wbc_detach_inode(struct writeback_control *wbc)
{
	struct bdi_writeback *wb = wbc->wb;
	struct inode *inode = wbc->inode;
	unsigned long avg_time, max_bytes, max_time;
	u16 history;
	int max_id;

	if (!wb)
		return;

	history = inode->i_wb_frn_history;
	avg_time = inode->i_wb_frn_avg_time;

	/* pick the winner of this round */
	if (wbc->wb_bytes >= wbc->wb_lcand_bytes &&
	    wbc->wb_bytes >= wbc->wb_tcand_bytes) {
		max_id = wbc->wb_id;
		max_bytes = wbc->wb_bytes;
	} else if (wbc->wb_lcand_bytes >= wbc->wb_tcand_bytes) {
		max_id = wbc->wb_lcand_id;
		max_bytes = wbc->wb_lcand_bytes;
	} else {
		max_id = wbc->wb_tcand_id;
		max_bytes = wbc->wb_tcand_bytes;
	}

	/*
	 * Calculate the amount of IO time the winner consumed and fold it
	 * into the running average kept per inode.  If the consumed IO
	 * time is lower than avag / WB_FRN_TIME_CUT_DIV, ignore it for
	 * deciding whether to switch or not.  This is to prevent one-off
	 * small dirtiers from skewing the verdict.
	 */
	max_time = DIV_ROUND_UP((max_bytes >> PAGE_SHIFT) << WB_FRN_TIME_SHIFT,
				wb->avg_write_bandwidth);
	if (avg_time)
		avg_time += (max_time >> WB_FRN_TIME_AVG_SHIFT) -
			    (avg_time >> WB_FRN_TIME_AVG_SHIFT);
	else
		avg_time = max_time;	/* immediate catch up on first run */

	if (max_time >= avg_time / WB_FRN_TIME_CUT_DIV) {
		int slots;

		/*
		 * The switch verdict is reached if foreign wb's consume
		 * more than a certain proportion of IO time in a
		 * WB_FRN_TIME_PERIOD.  This is loosely tracked by 16 slot
		 * history mask where each bit represents one sixteenth of
		 * the period.  Determine the number of slots to shift into
		 * history from @max_time.
		 */
		slots = min(DIV_ROUND_UP(max_time, WB_FRN_HIST_UNIT),
			    (unsigned long)WB_FRN_HIST_MAX_SLOTS);
		history <<= slots;
		if (wbc->wb_id != max_id)
			history |= (1U << slots) - 1;

		/*
		 * Switch if the current wb isn't the consistent winner.
		 * If there are multiple closely competing dirtiers, the
		 * inode may switch across them repeatedly over time, which
		 * is okay.  The main goal is avoiding keeping an inode on
		 * the wrong wb for an extended period of time.
		 */
		if (hweight32(history) > WB_FRN_HIST_THR_SLOTS)
			inode_switch_wbs(inode, max_id);
	}

	/*
	 * Multiple instances of this function may race to update the
	 * following fields but we don't mind occassional inaccuracies.
	 */
	inode->i_wb_frn_winner = max_id;
	inode->i_wb_frn_avg_time = min(avg_time, (unsigned long)U16_MAX);
	inode->i_wb_frn_history = history;

	wb_put(wbc->wb);
	wbc->wb = NULL;
}

/**
 * wbc_account_io - account IO issued during writeback
 * @wbc: writeback_control of the writeback in progress
 * @page: page being written out
 * @bytes: number of bytes being written out
 *
 * @bytes from @page are about to written out during the writeback
 * controlled by @wbc.  Keep the book for foreign inode detection.  See
 * wbc_detach_inode().
 */
void wbc_account_io(struct writeback_control *wbc, struct page *page,
		    size_t bytes)
{
	struct cgroup_subsys_state *css;
	int id;

	/*
	 * pageout() path doesn't attach @wbc to the inode being written
	 * out.  This is intentional as we don't want the function to block
	 * behind a slow cgroup.  Ultimately, we want pageout() to kick off
	 * regular writeback instead of writing things out itself.
	 */
	if (!wbc->wb)
		return;

	css = mem_cgroup_css_from_page(page);
	/* dead cgroups shouldn't contribute to inode ownership arbitration */
	if (!(css->flags & CSS_ONLINE))
		return;

	id = css->id;

	if (id == wbc->wb_id) {
		wbc->wb_bytes += bytes;
		return;
	}

	if (id == wbc->wb_lcand_id)
		wbc->wb_lcand_bytes += bytes;

	/* Boyer-Moore majority vote algorithm */
	if (!wbc->wb_tcand_bytes)
		wbc->wb_tcand_id = id;
	if (id == wbc->wb_tcand_id)
		wbc->wb_tcand_bytes += bytes;
	else
		wbc->wb_tcand_bytes -= min(bytes, wbc->wb_tcand_bytes);
}
EXPORT_SYMBOL_GPL(wbc_account_io);

/**
 * inode_congested - test whether an inode is congested
 * @inode: inode to test for congestion (may be NULL)
 * @cong_bits: mask of WB_[a]sync_congested bits to test
 *
 * Tests whether @inode is congested.  @cong_bits is the mask of congestion
 * bits to test and the return value is the mask of set bits.
 *
 * If cgroup writeback is enabled for @inode, the congestion state is
 * determined by whether the cgwb (cgroup bdi_writeback) for the blkcg
 * associated with @inode is congested; otherwise, the root wb's congestion
 * state is used.
 *
 * @inode is allowed to be NULL as this function is often called on
 * mapping->host which is NULL for the swapper space.
 */
int inode_congested(struct inode *inode, int cong_bits)
{
	/*
	 * Once set, ->i_wb never becomes NULL while the inode is alive.
	 * Start transaction iff ->i_wb is visible.
	 */
	if (inode && inode_to_wb_is_valid(inode)) {
		struct bdi_writeback *wb;
		struct wb_lock_cookie lock_cookie = {};
		bool congested;

		wb = unlocked_inode_to_wb_begin(inode, &lock_cookie);
		congested = wb_congested(wb, cong_bits);
		unlocked_inode_to_wb_end(inode, &lock_cookie);
		return congested;
	}

	return wb_congested(&inode_to_bdi(inode)->wb, cong_bits);
}
EXPORT_SYMBOL_GPL(inode_congested);

/**
 * wb_split_bdi_pages - split nr_pages to write according to bandwidth
 * @wb: target bdi_writeback to split @nr_pages to
 * @nr_pages: number of pages to write for the whole bdi
 *
 * Split @wb's portion of @nr_pages according to @wb's write bandwidth in
 * relation to the total write bandwidth of all wb's w/ dirty inodes on
 * @wb->bdi.
 */
static long wb_split_bdi_pages(struct bdi_writeback *wb, long nr_pages)
{
	unsigned long this_bw = wb->avg_write_bandwidth;
	unsigned long tot_bw = atomic_long_read(&wb->bdi->tot_write_bandwidth);

	if (nr_pages == LONG_MAX)
		return LONG_MAX;

	/*
	 * This may be called on clean wb's and proportional distribution
	 * may not make sense, just use the original @nr_pages in those
	 * cases.  In general, we wanna err on the side of writing more.
	 */
	if (!tot_bw || this_bw >= tot_bw)
		return nr_pages;
	else
		return DIV_ROUND_UP_ULL((u64)nr_pages * this_bw, tot_bw);
}

/**
 * bdi_split_work_to_wbs - split a wb_writeback_work to all wb's of a bdi
 * @bdi: target backing_dev_info
 * @base_work: wb_writeback_work to issue
 * @skip_if_busy: skip wb's which already have writeback in progress
 *
 * Split and issue @base_work to all wb's (bdi_writeback's) of @bdi which
 * have dirty inodes.  If @base_work->nr_page isn't %LONG_MAX, it's
 * distributed to the busy wbs according to each wb's proportion in the
 * total active write bandwidth of @bdi.
 */
static void bdi_split_work_to_wbs(struct backing_dev_info *bdi,
				  struct wb_writeback_work *base_work,
				  bool skip_if_busy)
{
	struct bdi_writeback *last_wb = NULL;
	struct bdi_writeback *wb = list_entry(&bdi->wb_list,
					      struct bdi_writeback, bdi_node);

	might_sleep();
restart:
	rcu_read_lock();
	list_for_each_entry_continue_rcu(wb, &bdi->wb_list, bdi_node) {
		DEFINE_WB_COMPLETION_ONSTACK(fallback_work_done);
		struct wb_writeback_work fallback_work;
		struct wb_writeback_work *work;
		long nr_pages;

		if (last_wb) {
			wb_put(last_wb);
			last_wb = NULL;
		}

		/* SYNC_ALL writes out I_DIRTY_TIME too */
		if (!wb_has_dirty_io(wb) &&
		    (base_work->sync_mode == WB_SYNC_NONE ||
		     list_empty(&wb->b_dirty_time)))
			continue;
		if (skip_if_busy && writeback_in_progress(wb))
			continue;

		nr_pages = wb_split_bdi_pages(wb, base_work->nr_pages);

		work = kmalloc(sizeof(*work), GFP_ATOMIC);
		if (work) {
			*work = *base_work;
			work->nr_pages = nr_pages;
			work->auto_free = 1;
			wb_queue_work(wb, work);
			continue;
		}

		/* alloc failed, execute synchronously using on-stack fallback */
		work = &fallback_work;
		*work = *base_work;
		work->nr_pages = nr_pages;
		work->auto_free = 0;
		work->done = &fallback_work_done;

		wb_queue_work(wb, work);

		/*
		 * Pin @wb so that it stays on @bdi->wb_list.  This allows
		 * continuing iteration from @wb after dropping and
		 * regrabbing rcu read lock.
		 */
		wb_get(wb);
		last_wb = wb;

		rcu_read_unlock();
		wb_wait_for_completion(bdi, &fallback_work_done);
		goto restart;
	}
	rcu_read_unlock();

	if (last_wb)
		wb_put(last_wb);
}

/**
 * cgroup_writeback_umount - flush inode wb switches for umount
 *
 * This function is called when a super_block is about to be destroyed and
 * flushes in-flight inode wb switches.  An inode wb switch goes through
 * RCU and then workqueue, so the two need to be flushed in order to ensure
 * that all previously scheduled switches are finished.  As wb switches are
 * rare occurrences and synchronize_rcu() can take a while, perform
 * flushing iff wb switches are in flight.
 */
void cgroup_writeback_umount(void)
{
	if (atomic_read(&isw_nr_in_flight)) {
		/*
		 * Use rcu_barrier() to wait for all pending callbacks to
		 * ensure that all in-flight wb switches are in the workqueue.
		 */
		rcu_barrier();
		flush_workqueue(isw_wq);
	}
}

static int __init cgroup_writeback_init(void)
{
	isw_wq = alloc_workqueue("inode_switch_wbs", 0, 0);
	if (!isw_wq)
		return -ENOMEM;
	return 0;
}
fs_initcall(cgroup_writeback_init);

#else	/* CONFIG_CGROUP_WRITEBACK */

static void bdi_down_write_wb_switch_rwsem(struct backing_dev_info *bdi) { }
static void bdi_up_write_wb_switch_rwsem(struct backing_dev_info *bdi) { }

static struct bdi_writeback *
locked_inode_to_wb_and_lock_list(struct inode *inode)
	__releases(&inode->i_lock)
	__acquires(&wb->list_lock)
{
	struct bdi_writeback *wb = inode_to_wb(inode);

	spin_unlock(&inode->i_lock);
	spin_lock(&wb->list_lock);
	return wb;
}

static struct bdi_writeback *inode_to_wb_and_lock_list(struct inode *inode)
	__acquires(&wb->list_lock)
{
	struct bdi_writeback *wb = inode_to_wb(inode);

	spin_lock(&wb->list_lock);
	return wb;
}

static long wb_split_bdi_pages(struct bdi_writeback *wb, long nr_pages)
{
	return nr_pages;
}

static void bdi_split_work_to_wbs(struct backing_dev_info *bdi,
				  struct wb_writeback_work *base_work,
				  bool skip_if_busy)
{
	might_sleep();

	if (!skip_if_busy || !writeback_in_progress(&bdi->wb)) {
		base_work->auto_free = 0;
		wb_queue_work(&bdi->wb, base_work);
	}
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

/*
 * Add in the number of potentially dirty inodes, because each inode
 * write can dirty pagecache in the underlying blockdev.
 */
static unsigned long get_nr_dirty_pages(void)
{
	return global_node_page_state(NR_FILE_DIRTY) +
		global_node_page_state(NR_UNSTABLE_NFS) +
		get_nr_dirty_inodes();
}

static void wb_start_writeback(struct bdi_writeback *wb, enum wb_reason reason)
{
	if (!wb_has_dirty_io(wb))
		return;

	/*
	 * All callers of this function want to start writeback of all
	 * dirty pages. Places like vmscan can call this at a very
	 * high frequency, causing pointless allocations of tons of
	 * work items and keeping the flusher threads busy retrieving
	 * that work. Ensure that we only allow one of them pending and
	 * inflight at the time.
	 */
	if (test_bit(WB_start_all, &wb->state) ||
	    test_and_set_bit(WB_start_all, &wb->state))
		return;

	wb->start_all_reason = reason;
	wb_wakeup(wb);
}

/**
 * wb_start_background_writeback - start background writeback
 * @wb: bdi_writback to write from
 *
 * Description:
 *   This makes sure WB_SYNC_NONE background writeback happens. When
 *   this function returns, it is only guaranteed that for given wb
 *   some IO is happening if we are over background dirty threshold.
 *   Caller need not hold sb s_umount semaphore.
 */
void wb_start_background_writeback(struct bdi_writeback *wb)
{
	/*
	 * We just wake up the flusher thread. It will perform background
	 * writeback as soon as there is no other work to do.
	 */
	trace_writeback_wake_background(wb);
	wb_wakeup(wb);
}

/*
 * Remove the inode from the writeback list it is on.
 */
void inode_io_list_del(struct inode *inode)
{
	struct bdi_writeback *wb;

	wb = inode_to_wb_and_lock_list(inode);
	inode_io_list_del_locked(inode, wb);
	spin_unlock(&wb->list_lock);
}

/*
 * mark an inode as under writeback on the sb
 */
void sb_mark_inode_writeback(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	unsigned long flags;

	if (list_empty(&inode->i_wb_list)) {
		spin_lock_irqsave(&sb->s_inode_wblist_lock, flags);
		if (list_empty(&inode->i_wb_list)) {
			list_add_tail(&inode->i_wb_list, &sb->s_inodes_wb);
			trace_sb_mark_inode_writeback(inode);
		}
		spin_unlock_irqrestore(&sb->s_inode_wblist_lock, flags);
	}
}

/*
 * clear an inode as under writeback on the sb
 */
void sb_clear_inode_writeback(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	unsigned long flags;

	if (!list_empty(&inode->i_wb_list)) {
		spin_lock_irqsave(&sb->s_inode_wblist_lock, flags);
		if (!list_empty(&inode->i_wb_list)) {
			list_del_init(&inode->i_wb_list);
			trace_sb_clear_inode_writeback(inode);
		}
		spin_unlock_irqrestore(&sb->s_inode_wblist_lock, flags);
	}
}

/*
 * Redirty an inode: set its when-it-was dirtied timestamp and move it to the
 * furthest end of its superblock's dirty-inode list.
 *
 * Before stamping the inode's ->dirtied_when, we check to see whether it is
 * already the most-recently-dirtied inode on the b_dirty list.  If that is
 * the case then the inode must have been redirtied while it was being written
 * out and we don't reset its dirtied_when.
 */
static void redirty_tail(struct inode *inode, struct bdi_writeback *wb)
{
	if (!list_empty(&wb->b_dirty)) {
		struct inode *tail;

		tail = wb_inode(wb->b_dirty.next);
		if (time_before(inode->dirtied_when, tail->dirtied_when))
			inode->dirtied_when = jiffies;
	}
	inode_io_list_move_locked(inode, wb, &wb->b_dirty);
}

/*
 * requeue inode for re-scanning after bdi->b_io list is exhausted.
 */
static void requeue_io(struct inode *inode, struct bdi_writeback *wb)
{
	inode_io_list_move_locked(inode, wb, &wb->b_more_io);
}

static void inode_sync_complete(struct inode *inode)
{
	inode->i_state &= ~I_SYNC;
	/* If inode is clean an unused, put it into LRU now... */
	inode_add_lru(inode);
	/* Waiters must see I_SYNC cleared before being woken up */
	smp_mb();
	wake_up_bit(&inode->i_state, __I_SYNC);
}

static bool inode_dirtied_after(struct inode *inode, unsigned long t)
{
	bool ret = time_after(inode->dirtied_when, t);
#ifndef CONFIG_64BIT
	/*
	 * For inodes being constantly redirtied, dirtied_when can get stuck.
	 * It _appears_ to be in the future, but is actually in distant past.
	 * This test is necessary to prevent such wrapped-around relative times
	 * from permanently stopping the whole bdi writeback.
	 */
	ret = ret && time_before_eq(inode->dirtied_when, jiffies);
#endif
	return ret;
}

#define EXPIRE_DIRTY_ATIME 0x0001

/*
 * Move expired (dirtied before work->older_than_this) dirty inodes from
 * @delaying_queue to @dispatch_queue.
 */
static int move_expired_inodes(struct list_head *delaying_queue,
			       struct list_head *dispatch_queue,
			       int flags,
			       struct wb_writeback_work *work)
{
	unsigned long *older_than_this = NULL;
	unsigned long expire_time;
	LIST_HEAD(tmp);
	struct list_head *pos, *node;
	struct super_block *sb = NULL;
	struct inode *inode;
	int do_sb_sort = 0;
	int moved = 0;

	if ((flags & EXPIRE_DIRTY_ATIME) == 0)
		older_than_this = work->older_than_this;
	else if (!work->for_sync) {
		expire_time = jiffies - (dirtytime_expire_interval * HZ);
		older_than_this = &expire_time;
	}
	while (!list_empty(delaying_queue)) {
		inode = wb_inode(delaying_queue->prev);
		if (older_than_this &&
		    inode_dirtied_after(inode, *older_than_this))
			break;
		list_move(&inode->i_io_list, &tmp);
		moved++;
		if (flags & EXPIRE_DIRTY_ATIME)
			set_bit(__I_DIRTY_TIME_EXPIRED, &inode->i_state);
		if (sb_is_blkdev_sb(inode->i_sb))
			continue;
		if (sb && sb != inode->i_sb)
			do_sb_sort = 1;
		sb = inode->i_sb;
	}

	/* just one sb in list, splice to dispatch_queue and we're done */
	if (!do_sb_sort) {
		list_splice(&tmp, dispatch_queue);
		goto out;
	}

	/* Move inodes from one superblock together */
	while (!list_empty(&tmp)) {
		sb = wb_inode(tmp.prev)->i_sb;
		list_for_each_prev_safe(pos, node, &tmp) {
			inode = wb_inode(pos);
			if (inode->i_sb == sb)
				list_move(&inode->i_io_list, dispatch_queue);
		}
	}
out:
	return moved;
}

/*
 * Queue all expired dirty inodes for io, eldest first.
 * Before
 *         newly dirtied     b_dirty    b_io    b_more_io
 *         =============>    gf         edc     BA
 * After
 *         newly dirtied     b_dirty    b_io    b_more_io
 *         =============>    g          fBAedc
 *                                           |
 *                                           +--> dequeue for IO
 */
static void queue_io(struct bdi_writeback *wb, struct wb_writeback_work *work)
{
	int moved;

	assert_spin_locked(&wb->list_lock);
	list_splice_init(&wb->b_more_io, &wb->b_io);
	moved = move_expired_inodes(&wb->b_dirty, &wb->b_io, 0, work);
	moved += move_expired_inodes(&wb->b_dirty_time, &wb->b_io,
				     EXPIRE_DIRTY_ATIME, work);
	if (moved)
		wb_io_lists_populated(wb);
	trace_writeback_queue_io(wb, work, moved);
}

static int write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret;

	if (inode->i_sb->s_op->write_inode && !is_bad_inode(inode)) {
		trace_writeback_write_inode_start(inode, wbc);
		ret = inode->i_sb->s_op->write_inode(inode, wbc);
		trace_writeback_write_inode(inode, wbc);
		return ret;
	}
	return 0;
}

/*
 * Wait for writeback on an inode to complete. Called with i_lock held.
 * Caller must make sure inode cannot go away when we drop i_lock.
 */
static void __inode_wait_for_writeback(struct inode *inode)
	__releases(inode->i_lock)
	__acquires(inode->i_lock)
{
	DEFINE_WAIT_BIT(wq, &inode->i_state, __I_SYNC);
	wait_queue_head_t *wqh;

	wqh = bit_waitqueue(&inode->i_state, __I_SYNC);
	while (inode->i_state & I_SYNC) {
		spin_unlock(&inode->i_lock);
		__wait_on_bit(wqh, &wq, bit_wait,
			      TASK_UNINTERRUPTIBLE);
		spin_lock(&inode->i_lock);
	}
}

/*
 * Wait for writeback on an inode to complete. Caller must have inode pinned.
 */
void inode_wait_for_writeback(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	__inode_wait_for_writeback(inode);
	spin_unlock(&inode->i_lock);
}

/*
 * Sleep until I_SYNC is cleared. This function must be called with i_lock
 * held and drops it. It is aimed for callers not holding any inode reference
 * so once i_lock is dropped, inode can go away.
 */
static void inode_sleep_on_writeback(struct inode *inode)
	__releases(inode->i_lock)
{
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = bit_waitqueue(&inode->i_state, __I_SYNC);
	int sleep;

	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	sleep = inode->i_state & I_SYNC;
	spin_unlock(&inode->i_lock);
	if (sleep)
		schedule();
	finish_wait(wqh, &wait);
}

/*
 * Find proper writeback list for the inode depending on its current state and
 * possibly also change of its state while we were doing writeback.  Here we
 * handle things such as livelock prevention or fairness of writeback among
 * inodes. This function can be called only by flusher thread - noone else
 * processes all inodes in writeback lists and requeueing inodes behind flusher
 * thread's back can have unexpected consequences.
 */
static void requeue_inode(struct inode *inode, struct bdi_writeback *wb,
			  struct writeback_control *wbc)
{
	if (inode->i_state & I_FREEING)
		return;

	/*
	 * Sync livelock prevention. Each inode is tagged and synced in one
	 * shot. If still dirty, it will be redirty_tail()'ed below.  Update
	 * the dirty time to prevent enqueue and sync it again.
	 */
	if ((inode->i_state & I_DIRTY) &&
	    (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages))
		inode->dirtied_when = jiffies;

	if (wbc->pages_skipped) {
		/*
		 * writeback is not making progress due to locked
		 * buffers. Skip this inode for now.
		 */
		redirty_tail(inode, wb);
		return;
	}

	if (mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY)) {
		/*
		 * We didn't write back all the pages.  nfs_writepages()
		 * sometimes bales out without doing anything.
		 */
		if (wbc->nr_to_write <= 0) {
			/* Slice used up. Queue for next turn. */
			requeue_io(inode, wb);
		} else {
			/*
			 * Writeback blocked by something other than
			 * congestion. Delay the inode for some time to
			 * avoid spinning on the CPU (100% iowait)
			 * retrying writeback of the dirty page/inode
			 * that cannot be performed immediately.
			 */
			redirty_tail(inode, wb);
		}
	} else if (inode->i_state & I_DIRTY) {
		/*
		 * Filesystems can dirty the inode during writeback operations,
		 * such as delayed allocation during submission or metadata
		 * updates after data IO completion.
		 */
		redirty_tail(inode, wb);
	} else if (inode->i_state & I_DIRTY_TIME) {
		inode->dirtied_when = jiffies;
		inode_io_list_move_locked(inode, wb, &wb->b_dirty_time);
	} else {
		/* The inode is clean. Remove from writeback lists. */
		inode_io_list_del_locked(inode, wb);
	}
}

/*
 * Write out an inode and its dirty pages. Do not update the writeback list
 * linkage. That is left to the caller. The caller is also responsible for
 * setting I_SYNC flag and calling inode_sync_complete() to clear it.
 */
static int
__writeback_single_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct address_space *mapping = inode->i_mapping;
	long nr_to_write = wbc->nr_to_write;
	unsigned dirty;
	int ret;

	WARN_ON(!(inode->i_state & I_SYNC));

	trace_writeback_single_inode_start(inode, wbc, nr_to_write);

	ret = do_writepages(mapping, wbc);

	/*
	 * Make sure to wait on the data before writing out the metadata.
	 * This is important for filesystems that modify metadata on data
	 * I/O completion. We don't do it for sync(2) writeback because it has a
	 * separate, external IO completion path and ->sync_fs for guaranteeing
	 * inode metadata is written back correctly.
	 */
	if (wbc->sync_mode == WB_SYNC_ALL && !wbc->for_sync) {
		int err = filemap_fdatawait(mapping);
		if (ret == 0)
			ret = err;
	}

	/*
	 * Some filesystems may redirty the inode during the writeback
	 * due to delalloc, clear dirty metadata flags right before
	 * write_inode()
	 */
	spin_lock(&inode->i_lock);

	dirty = inode->i_state & I_DIRTY;
	if (inode->i_state & I_DIRTY_TIME) {
		if ((dirty & I_DIRTY_INODE) ||
		    wbc->sync_mode == WB_SYNC_ALL ||
		    unlikely(inode->i_state & I_DIRTY_TIME_EXPIRED) ||
		    unlikely(time_after(jiffies,
					(inode->dirtied_time_when +
					 dirtytime_expire_interval * HZ)))) {
			dirty |= I_DIRTY_TIME | I_DIRTY_TIME_EXPIRED;
			trace_writeback_lazytime(inode);
		}
	} else
		inode->i_state &= ~I_DIRTY_TIME_EXPIRED;
	inode->i_state &= ~dirty;

	/*
	 * Paired with smp_mb() in __mark_inode_dirty().  This allows
	 * __mark_inode_dirty() to test i_state without grabbing i_lock -
	 * either they see the I_DIRTY bits cleared or we see the dirtied
	 * inode.
	 *
	 * I_DIRTY_PAGES is always cleared together above even if @mapping
	 * still has dirty pages.  The flag is reinstated after smp_mb() if
	 * necessary.  This guarantees that either __mark_inode_dirty()
	 * sees clear I_DIRTY_PAGES or we see PAGECACHE_TAG_DIRTY.
	 */
	smp_mb();

	if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
		inode->i_state |= I_DIRTY_PAGES;

	spin_unlock(&inode->i_lock);

	if (dirty & I_DIRTY_TIME)
		mark_inode_dirty_sync(inode);
	/* Don't write the inode if only I_DIRTY_PAGES was set */
	if (dirty & ~I_DIRTY_PAGES) {
		int err = write_inode(inode, wbc);
		if (ret == 0)
			ret = err;
	}
	trace_writeback_single_inode(inode, wbc, nr_to_write);
	return ret;
}

/*
 * Write out an inode's dirty pages. Either the caller has an active reference
 * on the inode or the inode has I_WILL_FREE set.
 *
 * This function is designed to be called for writing back one inode which
 * we go e.g. from filesystem. Flusher thread uses __writeback_single_inode()
 * and does more profound writeback list handling in writeback_sb_inodes().
 */
static int writeback_single_inode(struct inode *inode,
				  struct writeback_control *wbc)
{
	struct bdi_writeback *wb;
	int ret = 0;

	spin_lock(&inode->i_lock);
	if (!atomic_read(&inode->i_count))
		WARN_ON(!(inode->i_state & (I_WILL_FREE|I_FREEING)));
	else
		WARN_ON(inode->i_state & I_WILL_FREE);

	if (inode->i_state & I_SYNC) {
		if (wbc->sync_mode != WB_SYNC_ALL)
			goto out;
		/*
		 * It's a data-integrity sync. We must wait. Since callers hold
		 * inode reference or inode has I_WILL_FREE set, it cannot go
		 * away under us.
		 */
		__inode_wait_for_writeback(inode);
	}
	WARN_ON(inode->i_state & I_SYNC);
	/*
	 * Skip inode if it is clean and we have no outstanding writeback in
	 * WB_SYNC_ALL mode. We don't want to mess with writeback lists in this
	 * function since flusher thread may be doing for example sync in
	 * parallel and if we move the inode, it could get skipped. So here we
	 * make sure inode is on some writeback list and leave it there unless
	 * we have completely cleaned the inode.
	 */
	if (!(inode->i_state & I_DIRTY_ALL) &&
	    (wbc->sync_mode != WB_SYNC_ALL ||
	     !mapping_tagged(inode->i_mapping, PAGECACHE_TAG_WRITEBACK)))
		goto out;
	inode->i_state |= I_SYNC;
	wbc_attach_and_unlock_inode(wbc, inode);

	ret = __writeback_single_inode(inode, wbc);

	wbc_detach_inode(wbc);

	wb = inode_to_wb_and_lock_list(inode);
	spin_lock(&inode->i_lock);
	/*
	 * If inode is clean, remove it from writeback lists. Otherwise don't
	 * touch it. See comment above for explanation.
	 */
	if (!(inode->i_state & I_DIRTY_ALL))
		inode_io_list_del_locked(inode, wb);
	spin_unlock(&wb->list_lock);
	inode_sync_complete(inode);
out:
	spin_unlock(&inode->i_lock);
	return ret;
}

static long writeback_chunk_size(struct bdi_writeback *wb,
				 struct wb_writeback_work *work)
{
	long pages;

	/*
	 * WB_SYNC_ALL mode does livelock avoidance by syncing dirty
	 * inodes/pages in one big loop. Setting wbc.nr_to_write=LONG_MAX
	 * here avoids calling into writeback_inodes_wb() more than once.
	 *
	 * The intended call sequence for WB_SYNC_ALL writeback is:
	 *
	 *      wb_writeback()
	 *          writeback_sb_inodes()       <== called only once
	 *              write_cache_pages()     <== called once for each inode
	 *                   (quickly) tag currently dirty pages
	 *                   (maybe slowly) sync all tagged pages
	 */
	if (work->sync_mode == WB_SYNC_ALL || work->tagged_writepages)
		pages = LONG_MAX;
	else {
		pages = min(wb->avg_write_bandwidth / 2,
			    global_wb_domain.dirty_limit / DIRTY_SCOPE);
		pages = min(pages, work->nr_pages);
		pages = round_down(pages + MIN_WRITEBACK_PAGES,
				   MIN_WRITEBACK_PAGES);
	}

	return pages;
}

/*
 * Write a portion of b_io inodes which belong to @sb.
 *
 * Return the number of pages and/or inodes written.
 *
 * NOTE! This is called with wb->list_lock held, and will
 * unlock and relock that for each inode it ends up doing
 * IO for.
 */
static long writeback_sb_inodes(struct super_block *sb,
				struct bdi_writeback *wb,
				struct wb_writeback_work *work)
{
	struct writeback_control wbc = {
		.sync_mode		= work->sync_mode,
		.tagged_writepages	= work->tagged_writepages,
		.for_kupdate		= work->for_kupdate,
		.for_background		= work->for_background,
		.for_sync		= work->for_sync,
		.range_cyclic		= work->range_cyclic,
		.range_start		= 0,
		.range_end		= LLONG_MAX,
	};
	unsigned long start_time = jiffies;
	long write_chunk;
	long wrote = 0;  /* count both pages and inodes */

	while (!list_empty(&wb->b_io)) {
		struct inode *inode = wb_inode(wb->b_io.prev);
		struct bdi_writeback *tmp_wb;

		if (inode->i_sb != sb) {
			if (work->sb) {
				/*
				 * We only want to write back data for this
				 * superblock, move all inodes not belonging
				 * to it back onto the dirty list.
				 */
				redirty_tail(inode, wb);
				continue;
			}

			/*
			 * The inode belongs to a different superblock.
			 * Bounce back to the caller to unpin this and
			 * pin the next superblock.
			 */
			break;
		}

		/*
		 * Don't bother with new inodes or inodes being freed, first
		 * kind does not need periodic writeout yet, and for the latter
		 * kind writeout is handled by the freer.
		 */
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_NEW | I_FREEING | I_WILL_FREE)) {
			spin_unlock(&inode->i_lock);
			redirty_tail(inode, wb);
			continue;
		}
		if ((inode->i_state & I_SYNC) && wbc.sync_mode != WB_SYNC_ALL) {
			/*
			 * If this inode is locked for writeback and we are not
			 * doing writeback-for-data-integrity, move it to
			 * b_more_io so that writeback can proceed with the
			 * other inodes on s_io.
			 *
			 * We'll have another go at writing back this inode
			 * when we completed a full scan of b_io.
			 */
			spin_unlock(&inode->i_lock);
			requeue_io(inode, wb);
			trace_writeback_sb_inodes_requeue(inode);
			continue;
		}
		spin_unlock(&wb->list_lock);

		/*
		 * We already requeued the inode if it had I_SYNC set and we
		 * are doing WB_SYNC_NONE writeback. So this catches only the
		 * WB_SYNC_ALL case.
		 */
		if (inode->i_state & I_SYNC) {
			/* Wait for I_SYNC. This function drops i_lock... */
			inode_sleep_on_writeback(inode);
			/* Inode may be gone, start again */
			spin_lock(&wb->list_lock);
			continue;
		}
		inode->i_state |= I_SYNC;
		wbc_attach_and_unlock_inode(&wbc, inode);

		write_chunk = writeback_chunk_size(wb, work);
		wbc.nr_to_write = write_chunk;
		wbc.pages_skipped = 0;

		/*
		 * We use I_SYNC to pin the inode in memory. While it is set
		 * evict_inode() will wait so the inode cannot be freed.
		 */
		__writeback_single_inode(inode, &wbc);

		wbc_detach_inode(&wbc);
		work->nr_pages -= write_chunk - wbc.nr_to_write;
		wrote += write_chunk - wbc.nr_to_write;

		if (need_resched()) {
			/*
			 * We're trying to balance between building up a nice
			 * long list of IOs to improve our merge rate, and
			 * getting those IOs out quickly for anyone throttling
			 * in balance_dirty_pages().  cond_resched() doesn't
			 * unplug, so get our IOs out the door before we
			 * give up the CPU.
			 */
			blk_flush_plug(current);
			cond_resched();
		}

		/*
		 * Requeue @inode if still dirty.  Be careful as @inode may
		 * have been switched to another wb in the meantime.
		 */
		tmp_wb = inode_to_wb_and_lock_list(inode);
		spin_lock(&inode->i_lock);
		if (!(inode->i_state & I_DIRTY_ALL))
			wrote++;
		requeue_inode(inode, tmp_wb, &wbc);
		inode_sync_complete(inode);
		spin_unlock(&inode->i_lock);

		if (unlikely(tmp_wb != wb)) {
			spin_unlock(&tmp_wb->list_lock);
			spin_lock(&wb->list_lock);
		}

		/*
		 * bail out to wb_writeback() often enough to check
		 * background threshold and other termination conditions.
		 */
		if (wrote) {
			if (time_is_before_jiffies(start_time + HZ / 10UL))
				break;
			if (work->nr_pages <= 0)
				break;
		}
	}
	return wrote;
}

static long __writeback_inodes_wb(struct bdi_writeback *wb,
				  struct wb_writeback_work *work)
{
	unsigned long start_time = jiffies;
	long wrote = 0;

	while (!list_empty(&wb->b_io)) {
		struct inode *inode = wb_inode(wb->b_io.prev);
		struct super_block *sb = inode->i_sb;

		if (!trylock_super(sb)) {
			/*
			 * trylock_super() may fail consistently due to
			 * s_umount being grabbed by someone else. Don't use
			 * requeue_io() to avoid busy retrying the inode/sb.
			 */
			redirty_tail(inode, wb);
			continue;
		}
		wrote += writeback_sb_inodes(sb, wb, work);
		up_read(&sb->s_umount);

		/* refer to the same tests at the end of writeback_sb_inodes */
		if (wrote) {
			if (time_is_before_jiffies(start_time + HZ / 10UL))
				break;
			if (work->nr_pages <= 0)
				break;
		}
	}
	/* Leave any unwritten inodes on b_io */
	return wrote;
}

static long writeback_inodes_wb(struct bdi_writeback *wb, long nr_pages,
				enum wb_reason reason)
{
	struct wb_writeback_work work = {
		.nr_pages	= nr_pages,
		.sync_mode	= WB_SYNC_NONE,
		.range_cyclic	= 1,
		.reason		= reason,
	};
	struct blk_plug plug;

	blk_start_plug(&plug);
	spin_lock(&wb->list_lock);
	if (list_empty(&wb->b_io))
		queue_io(wb, &work);
	__writeback_inodes_wb(wb, &work);
	spin_unlock(&wb->list_lock);
	blk_finish_plug(&plug);

	return nr_pages - work.nr_pages;
}

/*
 * Explicit flushing or periodic writeback of "old" data.
 *
 * Define "old": the first time one of an inode's pages is dirtied, we mark the
 * dirtying-time in the inode's address_space.  So this periodic writeback code
 * just walks the superblock inode list, writing back any inodes which are
 * older than a specific point in time.
 *
 * Try to run once per dirty_writeback_interval.  But if a writeback event
 * takes longer than a dirty_writeback_interval interval, then leave a
 * one-second gap.
 *
 * older_than_this takes precedence over nr_to_write.  So we'll only write back
 * all dirty pages if they are all attached to "old" mappings.
 */
static long wb_writeback(struct bdi_writeback *wb,
			 struct wb_writeback_work *work)
{
	unsigned long wb_start = jiffies;
	long nr_pages = work->nr_pages;
	unsigned long oldest_jif;
	struct inode *inode;
	long progress;
	struct blk_plug plug;

	oldest_jif = jiffies;
	work->older_than_this = &oldest_jif;

	blk_start_plug(&plug);
	spin_lock(&wb->list_lock);
	for (;;) {
		/*
		 * Stop writeback when nr_pages has been consumed
		 */
		if (work->nr_pages <= 0)
			break;

		/*
		 * Background writeout and kupdate-style writeback may
		 * run forever. Stop them if there is other work to do
		 * so that e.g. sync can proceed. They'll be restarted
		 * after the other works are all done.
		 */
		if ((work->for_background || work->for_kupdate) &&
		    !list_empty(&wb->work_list))
			break;

		/*
		 * For background writeout, stop when we are below the
		 * background dirty threshold
		 */
		if (work->for_background && !wb_over_bg_thresh(wb))
			break;

		/*
		 * Kupdate and background works are special and we want to
		 * include all inodes that need writing. Livelock avoidance is
		 * handled by these works yielding to any other work so we are
		 * safe.
		 */
		if (work->for_kupdate) {
			oldest_jif = jiffies -
				msecs_to_jiffies(dirty_expire_interval * 10);
		} else if (work->for_background)
			oldest_jif = jiffies;

		trace_writeback_start(wb, work);
		if (list_empty(&wb->b_io))
			queue_io(wb, work);
		if (work->sb)
			progress = writeback_sb_inodes(work->sb, wb, work);
		else
			progress = __writeback_inodes_wb(wb, work);
		trace_writeback_written(wb, work);

		wb_update_bandwidth(wb, wb_start);

		/*
		 * Did we write something? Try for more
		 *
		 * Dirty inodes are moved to b_io for writeback in batches.
		 * The completion of the current batch does not necessarily
		 * mean the overall work is done. So we keep looping as long
		 * as made some progress on cleaning pages or inodes.
		 */
		if (progress)
			continue;
		/*
		 * No more inodes for IO, bail
		 */
		if (list_empty(&wb->b_more_io))
			break;
		/*
		 * Nothing written. Wait for some inode to
		 * become available for writeback. Otherwise
		 * we'll just busyloop.
		 */
		trace_writeback_wait(wb, work);
		inode = wb_inode(wb->b_more_io.prev);
		spin_lock(&inode->i_lock);
		spin_unlock(&wb->list_lock);
		/* This function drops i_lock... */
		inode_sleep_on_writeback(inode);
		spin_lock(&wb->list_lock);
	}
	spin_unlock(&wb->list_lock);
	blk_finish_plug(&plug);

	return nr_pages - work->nr_pages;
}

/*
 * Return the next wb_writeback_work struct that hasn't been processed yet.
 */
static struct wb_writeback_work *get_next_work_item(struct bdi_writeback *wb)
{
	struct wb_writeback_work *work = NULL;

	spin_lock_bh(&wb->work_lock);
	if (!list_empty(&wb->work_list)) {
		work = list_entry(wb->work_list.next,
				  struct wb_writeback_work, list);
		list_del_init(&work->list);
	}
	spin_unlock_bh(&wb->work_lock);
	return work;
}

static long wb_check_background_flush(struct bdi_writeback *wb)
{
	if (wb_over_bg_thresh(wb)) {

		struct wb_writeback_work work = {
			.nr_pages	= LONG_MAX,
			.sync_mode	= WB_SYNC_NONE,
			.for_background	= 1,
			.range_cyclic	= 1,
			.reason		= WB_REASON_BACKGROUND,
		};

		return wb_writeback(wb, &work);
	}

	return 0;
}

static long wb_check_old_data_flush(struct bdi_writeback *wb)
{
	unsigned long expired;
	long nr_pages;

	/*
	 * When set to zero, disable periodic writeback
	 */
	if (!dirty_writeback_interval)
		return 0;

	expired = wb->last_old_flush +
			msecs_to_jiffies(dirty_writeback_interval * 10);
	if (time_before(jiffies, expired))
		return 0;

	wb->last_old_flush = jiffies;
	nr_pages = get_nr_dirty_pages();

	if (nr_pages) {
		struct wb_writeback_work work = {
			.nr_pages	= nr_pages,
			.sync_mode	= WB_SYNC_NONE,
			.for_kupdate	= 1,
			.range_cyclic	= 1,
			.reason		= WB_REASON_PERIODIC,
		};

		return wb_writeback(wb, &work);
	}

	return 0;
}

static long wb_check_start_all(struct bdi_writeback *wb)
{
	long nr_pages;

	if (!test_bit(WB_start_all, &wb->state))
		return 0;

	nr_pages = get_nr_dirty_pages();
	if (nr_pages) {
		struct wb_writeback_work work = {
			.nr_pages	= wb_split_bdi_pages(wb, nr_pages),
			.sync_mode	= WB_SYNC_NONE,
			.range_cyclic	= 1,
			.reason		= wb->start_all_reason,
		};

		nr_pages = wb_writeback(wb, &work);
	}

	clear_bit(WB_start_all, &wb->state);
	return nr_pages;
}


/*
 * Retrieve work items and do the writeback they describe
 */
static long wb_do_writeback(struct bdi_writeback *wb)
{
	struct wb_writeback_work *work;
	long wrote = 0;

	set_bit(WB_writeback_running, &wb->state);
	while ((work = get_next_work_item(wb)) != NULL) {
		trace_writeback_exec(wb, work);
		wrote += wb_writeback(wb, work);
		finish_writeback_work(wb, work);
	}

	/*
	 * Check for a flush-everything request
	 */
	wrote += wb_check_start_all(wb);

	/*
	 * Check for periodic writeback, kupdated() style
	 */
	wrote += wb_check_old_data_flush(wb);
	wrote += wb_check_background_flush(wb);
	clear_bit(WB_writeback_running, &wb->state);

	return wrote;
}

/*
 * Handle writeback of dirty data for the device backed by this bdi. Also
 * reschedules periodically and does kupdated style flushing.
 */
void wb_workfn(struct work_struct *work)
{
	struct bdi_writeback *wb = container_of(to_delayed_work(work),
						struct bdi_writeback, dwork);
	long pages_written;

	set_worker_desc("flush-%s", dev_name(wb->bdi->dev));
	current->flags |= PF_SWAPWRITE;

	if (likely(!current_is_workqueue_rescuer() ||
		   !test_bit(WB_registered, &wb->state))) {
		/*
		 * The normal path.  Keep writing back @wb until its
		 * work_list is empty.  Note that this path is also taken
		 * if @wb is shutting down even when we're running off the
		 * rescuer as work_list needs to be drained.
		 */
		do {
			pages_written = wb_do_writeback(wb);
			trace_writeback_pages_written(pages_written);
		} while (!list_empty(&wb->work_list));
	} else {
		/*
		 * bdi_wq can't get enough workers and we're running off
		 * the emergency worker.  Don't hog it.  Hopefully, 1024 is
		 * enough for efficient IO.
		 */
		pages_written = writeback_inodes_wb(wb, 1024,
						    WB_REASON_FORKER_THREAD);
		trace_writeback_pages_written(pages_written);
	}
#if defined(OPLUS_FEATURE_IOMONITOR) && defined(CONFIG_IOMONITOR)
	iomonitor_update_fs_stats(FS_DIRTY_PAGES, pages_written);
#endif /*OPLUS_FEATURE_IOMONITOR*/
	if (!list_empty(&wb->work_list))
		wb_wakeup(wb);
	else if (wb_has_dirty_io(wb) && dirty_writeback_interval)
		wb_wakeup_delayed(wb);

	current->flags &= ~PF_SWAPWRITE;
}

/*
 * Start writeback of `nr_pages' pages on this bdi. If `nr_pages' is zero,
 * write back the whole world.
 */
static void __wakeup_flusher_threads_bdi(struct backing_dev_info *bdi,
					 enum wb_reason reason)
{
	struct bdi_writeback *wb;

	if (!bdi_has_dirty_io(bdi))
		return;

	list_for_each_entry_rcu(wb, &bdi->wb_list, bdi_node)
		wb_start_writeback(wb, reason);
}

void wakeup_flusher_threads_bdi(struct backing_dev_info *bdi,
				enum wb_reason reason)
{
	rcu_read_lock();
	__wakeup_flusher_threads_bdi(bdi, reason);
	rcu_read_unlock();
}

/*
 * Wakeup the flusher threads to start writeback of all currently dirty pages
 */
void wakeup_flusher_threads(enum wb_reason reason)
{
	struct backing_dev_info *bdi;

	/*
	 * If we are expecting writeback progress we must submit plugged IO.
	 */
	if (blk_needs_flush_plug(current))
		blk_schedule_flush_plug(current);

	rcu_read_lock();
	list_for_each_entry_rcu(bdi, &bdi_list, bdi_list)
		__wakeup_flusher_threads_bdi(bdi, reason);
	rcu_read_unlock();
}

/*
 * Wake up bdi's periodically to make sure dirtytime inodes gets
 * written back periodically.  We deliberately do *not* check the
 * b_dirtytime list in wb_has_dirty_io(), since this would cause the
 * kernel to be constantly waking up once there are any dirtytime
 * inodes on the system.  So instead we define a separate delayed work
 * function which gets called much more rarely.  (By default, only
 * once every 12 hours.)
 *
 * If there is any other write activity going on in the file system,
 * this function won't be necessary.  But if the only thing that has
 * happened on the file system is a dirtytime inode caused by an atime
 * update, we need this infrastructure below to make sure that inode
 * eventually gets pushed out to disk.
 */
static void wakeup_dirtytime_writeback(struct work_struct *w);
static DECLARE_DELAYED_WORK(dirtytime_work, wakeup_dirtytime_writeback);

static void wakeup_dirtytime_writeback(struct work_struct *w)
{
	struct backing_dev_info *bdi;

	rcu_read_lock();
	list_for_each_entry_rcu(bdi, &bdi_list, bdi_list) {
		struct bdi_writeback *wb;

		list_for_each_entry_rcu(wb, &bdi->wb_list, bdi_node)
			if (!list_empty(&wb->b_dirty_time))
				wb_wakeup(wb);
	}
	rcu_read_unlock();
	schedule_delayed_work(&dirtytime_work, dirtytime_expire_interval * HZ);
}

static int __init start_dirtytime_writeback(void)
{
	schedule_delayed_work(&dirtytime_work, dirtytime_expire_interval * HZ);
	return 0;
}
__initcall(start_dirtytime_writeback);

int dirtytime_interval_handler(struct ctl_table *table, int write,
			       void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (ret == 0 && write)
		mod_delayed_work(system_wq, &dirtytime_work, 0);
	return ret;
}

static noinline void block_dump___mark_inode_dirty(struct inode *inode)
{
	if (inode->i_ino || strcmp(inode->i_sb->s_id, "bdev")) {
		struct dentry *dentry;
		const char *name = "?";

		dentry = d_find_alias(inode);
		if (dentry) {
			spin_lock(&dentry->d_lock);
			name = (const char *) dentry->d_name.name;
		}
		printk(KERN_DEBUG
		       "%s(%d): dirtied inode %lu (%s) on %s\n",
		       current->comm, task_pid_nr(current), inode->i_ino,
		       name, inode->i_sb->s_id);
		if (dentry) {
			spin_unlock(&dentry->d_lock);
			dput(dentry);
		}
	}
}

/**
 * __mark_inode_dirty -	internal function
 *
 * @inode: inode to mark
 * @flags: what kind of dirty (i.e. I_DIRTY_SYNC)
 *
 * Mark an inode as dirty. Callers should use mark_inode_dirty or
 * mark_inode_dirty_sync.
 *
 * Put the inode on the super block's dirty list.
 *
 * CAREFUL! We mark it dirty unconditionally, but move it onto the
 * dirty list only if it is hashed or if it refers to a blockdev.
 * If it was not hashed, it will never be added to the dirty list
 * even if it is later hashed, as it will have been marked dirty already.
 *
 * In short, make sure you hash any inodes _before_ you start marking
 * them dirty.
 *
 * Note that for blockdevs, inode->dirtied_when represents the dirtying time of
 * the block-special inode (/dev/hda1) itself.  And the ->dirtied_when field of
 * the kernel-internal blockdev inode represents the dirtying time of the
 * blockdev's pages.  This is why for I_DIRTY_PAGES we always use
 * page->mapping->host, so the page-dirtying time is recorded in the internal
 * blockdev inode.
 */
void __mark_inode_dirty(struct inode *inode, int flags)
{
	struct super_block *sb = inode->i_sb;
	int dirtytime;

	trace_writeback_mark_inode_dirty(inode, flags);

	/*
	 * Don't do this for I_DIRTY_PAGES - that doesn't actually
	 * dirty the inode itself
	 */
	if (flags & (I_DIRTY_INODE | I_DIRTY_TIME)) {
		trace_writeback_dirty_inode_start(inode, flags);

		if (sb->s_op->dirty_inode)
			sb->s_op->dirty_inode(inode, flags);

		trace_writeback_dirty_inode(inode, flags);
	}
	if (flags & I_DIRTY_INODE)
		flags &= ~I_DIRTY_TIME;
	dirtytime = flags & I_DIRTY_TIME;

	/*
	 * Paired with smp_mb() in __writeback_single_inode() for the
	 * following lockless i_state test.  See there for details.
	 */
	smp_mb();

	if (((inode->i_state & flags) == flags) ||
	    (dirtytime && (inode->i_state & I_DIRTY_INODE)))
		return;

	if (unlikely(block_dump))
		block_dump___mark_inode_dirty(inode);

	spin_lock(&inode->i_lock);
	if (dirtytime && (inode->i_state & I_DIRTY_INODE))
		goto out_unlock_inode;
	if ((inode->i_state & flags) != flags) {
		const int was_dirty = inode->i_state & I_DIRTY;

		inode_attach_wb(inode, NULL);

		if (flags & I_DIRTY_INODE)
			inode->i_state &= ~I_DIRTY_TIME;
		inode->i_state |= flags;

		/*
		 * If the inode is being synced, just update its dirty state.
		 * The unlocker will place the inode on the appropriate
		 * superblock list, based upon its state.
		 */
		if (inode->i_state & I_SYNC)
			goto out_unlock_inode;

		/*
		 * Only add valid (hashed) inodes to the superblock's
		 * dirty list.  Add blockdev inodes as well.
		 */
		if (!S_ISBLK(inode->i_mode)) {
			if (inode_unhashed(inode))
				goto out_unlock_inode;
		}
		if (inode->i_state & I_FREEING)
			goto out_unlock_inode;

		/*
		 * If the inode was already on b_dirty/b_io/b_more_io, don't
		 * reposition it (that would break b_dirty time-ordering).
		 */
		if (!was_dirty) {
			struct bdi_writeback *wb;
			struct list_head *dirty_list;
			bool wakeup_bdi = false;

			wb = locked_inode_to_wb_and_lock_list(inode);

			WARN(bdi_cap_writeback_dirty(wb->bdi) &&
			     !test_bit(WB_registered, &wb->state),
			     "bdi-%s not registered\n", wb->bdi->name);

			inode->dirtied_when = jiffies;
			if (dirtytime)
				inode->dirtied_time_when = jiffies;

			if (inode->i_state & I_DIRTY)
				dirty_list = &wb->b_dirty;
			else
				dirty_list = &wb->b_dirty_time;

			wakeup_bdi = inode_io_list_move_locked(inode, wb,
							       dirty_list);

			spin_unlock(&wb->list_lock);
			trace_writeback_dirty_inode_enqueue(inode);

			/*
			 * If this is the first dirty inode for this bdi,
			 * we have to wake-up the corresponding bdi thread
			 * to make sure background write-back happens
			 * later.
			 */
			if (bdi_cap_writeback_dirty(wb->bdi) && wakeup_bdi)
				wb_wakeup_delayed(wb);
			return;
		}
	}
out_unlock_inode:
	spin_unlock(&inode->i_lock);
}
EXPORT_SYMBOL(__mark_inode_dirty);

/*
 * The @s_sync_lock is used to serialise concurrent sync operations
 * to avoid lock contention problems with concurrent wait_sb_inodes() calls.
 * Concurrent callers will block on the s_sync_lock rather than doing contending
 * walks. The queueing maintains sync(2) required behaviour as all the IO that
 * has been issued up to the time this function is enter is guaranteed to be
 * completed by the time we have gained the lock and waited for all IO that is
 * in progress regardless of the order callers are granted the lock.
 */
static void wait_sb_inodes(struct super_block *sb)
{
	LIST_HEAD(sync_list);

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	mutex_lock(&sb->s_sync_lock);

	/*
	 * Splice the writeback list onto a temporary list to avoid waiting on
	 * inodes that have started writeback after this point.
	 *
	 * Use rcu_read_lock() to keep the inodes around until we have a
	 * reference. s_inode_wblist_lock protects sb->s_inodes_wb as well as
	 * the local list because inodes can be dropped from either by writeback
	 * completion.
	 */
	rcu_read_lock();
	spin_lock_irq(&sb->s_inode_wblist_lock);
	list_splice_init(&sb->s_inodes_wb, &sync_list);

	/*
	 * Data integrity sync. Must wait for all pages under writeback, because
	 * there may have been pages dirtied before our sync call, but which had
	 * writeout started before we write it out.  In which case, the inode
	 * may not be on the dirty list, but we still have to wait for that
	 * writeout.
	 */
	while (!list_empty(&sync_list)) {
		struct inode *inode = list_first_entry(&sync_list, struct inode,
						       i_wb_list);
		struct address_space *mapping = inode->i_mapping;

		/*
		 * Move each inode back to the wb list before we drop the lock
		 * to preserve consistency between i_wb_list and the mapping
		 * writeback tag. Writeback completion is responsible to remove
		 * the inode from either list once the writeback tag is cleared.
		 */
		list_move_tail(&inode->i_wb_list, &sb->s_inodes_wb);

		/*
		 * The mapping can appear untagged while still on-list since we
		 * do not have the mapping lock. Skip it here, wb completion
		 * will remove it.
		 */
		if (!mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK))
			continue;

		spin_unlock_irq(&sb->s_inode_wblist_lock);

		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_FREEING|I_WILL_FREE|I_NEW)) {
			spin_unlock(&inode->i_lock);

			spin_lock_irq(&sb->s_inode_wblist_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		rcu_read_unlock();

		/*
		 * We keep the error status of individual mapping so that
		 * applications can catch the writeback error using fsync(2).
		 * See filemap_fdatawait_keep_errors() for details.
		 */
		filemap_fdatawait_keep_errors(mapping);

		cond_resched();

		iput(inode);

		rcu_read_lock();
		spin_lock_irq(&sb->s_inode_wblist_lock);
	}
	spin_unlock_irq(&sb->s_inode_wblist_lock);
	rcu_read_unlock();
	mutex_unlock(&sb->s_sync_lock);
}

static void __writeback_inodes_sb_nr(struct super_block *sb, unsigned long nr,
				     enum wb_reason reason, bool skip_if_busy)
{
	DEFINE_WB_COMPLETION_ONSTACK(done);
	struct wb_writeback_work work = {
		.sb			= sb,
		.sync_mode		= WB_SYNC_NONE,
		.tagged_writepages	= 1,
		.done			= &done,
		.nr_pages		= nr,
		.reason			= reason,
	};
	struct backing_dev_info *bdi = sb->s_bdi;

	if (!bdi_has_dirty_io(bdi) || bdi == &noop_backing_dev_info)
		return;
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	bdi_split_work_to_wbs(sb->s_bdi, &work, skip_if_busy);
	wb_wait_for_completion(bdi, &done);
}

/**
 * writeback_inodes_sb_nr -	writeback dirty inodes from given super_block
 * @sb: the superblock
 * @nr: the number of pages to write
 * @reason: reason why some writeback work initiated
 *
 * Start writeback on some inodes on this super_block. No guarantees are made
 * on how many (if any) will be written, and this function does not wait
 * for IO completion of submitted IO.
 */
void writeback_inodes_sb_nr(struct super_block *sb,
			    unsigned long nr,
			    enum wb_reason reason)
{
	__writeback_inodes_sb_nr(sb, nr, reason, false);
}
EXPORT_SYMBOL(writeback_inodes_sb_nr);

/**
 * writeback_inodes_sb	-	writeback dirty inodes from given super_block
 * @sb: the superblock
 * @reason: reason why some writeback work was initiated
 *
 * Start writeback on some inodes on this super_block. No guarantees are made
 * on how many (if any) will be written, and this function does not wait
 * for IO completion of submitted IO.
 */
void writeback_inodes_sb(struct super_block *sb, enum wb_reason reason)
{
	return writeback_inodes_sb_nr(sb, get_nr_dirty_pages(), reason);
}
EXPORT_SYMBOL(writeback_inodes_sb);

/**
 * try_to_writeback_inodes_sb - try to start writeback if none underway
 * @sb: the superblock
 * @reason: reason why some writeback work was initiated
 *
 * Invoke __writeback_inodes_sb_nr if no writeback is currently underway.
 */
void try_to_writeback_inodes_sb(struct super_block *sb, enum wb_reason reason)
{
	if (!down_read_trylock(&sb->s_umount))
		return;

	__writeback_inodes_sb_nr(sb, get_nr_dirty_pages(), reason, true);
	up_read(&sb->s_umount);
}
EXPORT_SYMBOL(try_to_writeback_inodes_sb);

/**
 * sync_inodes_sb	-	sync sb inode pages
 * @sb: the superblock
 *
 * This function writes and waits on any dirty inode belonging to this
 * super_block.
 */
void sync_inodes_sb(struct super_block *sb)
{
	DEFINE_WB_COMPLETION_ONSTACK(done);
	struct wb_writeback_work work = {
		.sb		= sb,
		.sync_mode	= WB_SYNC_ALL,
		.nr_pages	= LONG_MAX,
		.range_cyclic	= 0,
		.done		= &done,
		.reason		= WB_REASON_SYNC,
		.for_sync	= 1,
	};
	struct backing_dev_info *bdi = sb->s_bdi;

	/*
	 * Can't skip on !bdi_has_dirty() because we should wait for !dirty
	 * inodes under writeback and I_DIRTY_TIME inodes ignored by
	 * bdi_has_dirty() need to be written out too.
	 */
	if (bdi == &noop_backing_dev_info)
		return;
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/* protect against inode wb switch, see inode_switch_wbs_work_fn() */
	bdi_down_write_wb_switch_rwsem(bdi);
	bdi_split_work_to_wbs(bdi, &work, false);
	wb_wait_for_completion(bdi, &done);
	bdi_up_write_wb_switch_rwsem(bdi);

	wait_sb_inodes(sb);
}
EXPORT_SYMBOL(sync_inodes_sb);

/**
 * write_inode_now	-	write an inode to disk
 * @inode: inode to write to disk
 * @sync: whether the write should be synchronous or not
 *
 * This function commits an inode to disk immediately if it is dirty. This is
 * primarily needed by knfsd.
 *
 * The caller must either have a ref on the inode or must have set I_WILL_FREE.
 */
int write_inode_now(struct inode *inode, int sync)
{
	struct writeback_control wbc = {
		.nr_to_write = LONG_MAX,
		.sync_mode = sync ? WB_SYNC_ALL : WB_SYNC_NONE,
		.range_start = 0,
		.range_end = LLONG_MAX,
	};

	if (!mapping_cap_writeback_dirty(inode->i_mapping))
		wbc.nr_to_write = 0;

	might_sleep();
	return writeback_single_inode(inode, &wbc);
}
EXPORT_SYMBOL(write_inode_now);

/**
 * sync_inode - write an inode and its pages to disk.
 * @inode: the inode to sync
 * @wbc: controls the writeback mode
 *
 * sync_inode() will write an inode and its pages to disk.  It will also
 * correctly update the inode on its superblock's dirty inode lists and will
 * update inode->i_state.
 *
 * The caller must have a ref on the inode.
 */
int sync_inode(struct inode *inode, struct writeback_control *wbc)
{
	return writeback_single_inode(inode, wbc);
}
EXPORT_SYMBOL(sync_inode);

/**
 * sync_inode_metadata - write an inode to disk
 * @inode: the inode to sync
 * @wait: wait for I/O to complete.
 *
 * Write an inode to disk and adjust its dirty state after completion.
 *
 * Note: only writes the actual inode, no associated data or other metadata.
 */
int sync_inode_metadata(struct inode *inode, int wait)
{
	struct writeback_control wbc = {
		.sync_mode = wait ? WB_SYNC_ALL : WB_SYNC_NONE,
		.nr_to_write = 0, /* metadata-only */
	};

	return sync_inode(inode, &wbc);
}
EXPORT_SYMBOL(sync_inode_metadata);
