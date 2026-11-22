/*
 * core/f2/fsm/reclaim.c - the reclaim of the allocator of format major 2 (core/f2/fsm.h): the matured extents of the quarantine join the free lists, and the lists are written anew.
 */
#include "core/host.h"

#include "core/f2/fsm.h"
#include "core/f2/f2_pace.h"
#include "core/f2/fsm/fsm_int.h"
#include "core/horizon.h"
#include "core/page_io.h"
#include "core/wal_conflict.h"

/* lag: the standby-safe horizon an extent's id must precede too; invalid when the quarantine holds over a quarter of the index, which is then released at the raw horizon */
static FullTransactionId
quarantine_lag(Relation rel, const ExtVec *queue)
{
	uint64		held = 0;
	for (int i = 0; i < queue->n; i++)
		held += queue->v[i].nblocks;
	return held > RelationGetNumberOfBlocks(rel) / 4 ? InvalidFullTransactionId : sift_standby_safe_horizon(rel, false);
}

static bool
matured(const FsmCtx *c, FullTransactionId safexid, FullTransactionId lag)
{
	if (c->exclusive || !FullTransactionIdIsValid(safexid))
		return true;
	if (FullTransactionIdIsValid(lag) && !FullTransactionIdPrecedes(safexid, lag))
		return false;
	return FullTransactionIdIsValid(c->horizon) ? FullTransactionIdPrecedes(safexid, c->horizon) : sift_horizon_removable(c->rel, safexid);
}

/* The extents [start, start + n) cut into the largest classes that fit, each as the entry of its class */
static void
cut_into_classes(ExtVec *byclass, BlockNumber start, uint64 n)
{
	while (n > 0)
	{
		int			c = sift_fsm_class_floor(n);

		sift_fsm_extvec_add(&byclass[c], start, SIFT_FSM_CLASS_BLOCKS(c), InvalidFullTransactionId);
		start += SIFT_FSM_CLASS_BLOCKS(c);
		n -= SIFT_FSM_CLASS_BLOCKS(c);
	}
}

/* Sorts the extents and joins the adjacent ones; an extent that is used up is dropped */
static void
coalesce(Relation rel, ExtVec *v)
{
	int			n = 0;

	for (int i = 0; i < v->n; i++)
		if (v->v[i].nblocks > 0)
			v->v[n++] = v->v[i];
	v->n = n;
	qsort(v->v, v->n, sizeof(SiftFsmExtent), sift_fsm_ext_cmp);
	n = 0;
	for (int i = 0; i < v->n;)
	{
		SiftFsmExtent run = v->v[i];
		uint64		end = (uint64) run.start + run.nblocks;

		for (i++; i < v->n && v->v[i].start <= end; i++)
		{
			if (v->v[i].start < end)
				SIFT_CORRUPT(rel, "Free extents overlap at block %u.", v->v[i].start);
			end += v->v[i].nblocks;
		}
		run.nblocks = (uint32) (end - run.start);
		v->v[n++] = run;
	}
	v->n = n;
}

static int
pages_for(int n)
{
	return (n + SIFT_FSM_PAGE_CAP - 1) / SIFT_FSM_PAGE_CAP;
}

/* Whether the free list of a class holds what the pass would write for it: the same extents, whatever the order of its pages. Such a list keeps its pages, and the record does not name its class */
static bool
same_extents(ExtVec *listed, const ExtVec *written)
{
	if (listed->n != written->n)
		return false;
	qsort(listed->v, listed->n, sizeof(SiftFsmExtent), sift_fsm_ext_cmp);
	for (int i = 0; i < listed->n; i++)
		if (listed->v[i].start != written->v[i].start || listed->v[i].nblocks != written->v[i].nblocks)
			return false;
	return true;
}

/*
 * Rebuilds the lists: the matured extents of the queue join the free ones, adjacent runs are coalesced and cut into classes again, the rest of the queue is kept. The new pages
 * come from blocks the bitmaps hold free now and are written before the one record that switches the lists over (the old pages go back to the bitmaps in it), so a crash at any
 * point leaves the old lists. When the free metadata blocks do not hold the pages of all the lists, only the queue is rewritten first: its matured metadata pages go back to the bitmaps, which
 * gives the full pass that follows the room, and no slab is claimed for lists that the pages just given back would hold.
 */
static uint64
reclaim_pass(FsmCtx *c, BlockNumber *new_hw)
{
	Relation	rel = c->rel;
	bool		claimed = false;
	bool		meta_only = false;
	uint64		freed_before = 0;

	for (;;)
	{
		BlockNumber hw = new_hw != NULL ? sift_fsm_high_water(c) : 0;
		ExtVec		ready = {0}, queue = {0}, pages = {0}, data = {0}, pending = {0}, metafree = {0}, idle = {0};
		ExtVec		byclass[SIFT_FSM_NCLASSES] = {{0}},
					listed[SIFT_FSM_NCLASSES] = {{0}},
					listed_pages[SIFT_FSM_NCLASSES] = {{0}};
		BlockNumber old_head[SIFT_FSM_NCLASSES];
		bool		keep[SIFT_FSM_NCLASSES] = {false};
		Buffer		rootbuf = sift_fsm_lock_root(rel);
		Page		root = BufferGetPage(rootbuf);
		SiftFsmMetaPageData *m = FSMM(root);
		int			newpages = 0;
		uint32		meta_free = m->meta_free_count;
		uint64		freed = freed_before;
		BlockNumber cut = InvalidBlockNumber;
		FullTransactionId max_matured = InvalidFullTransactionId,
					lag;

		sift_fsm_walk_list(rel, m->quarantine_head, SIFT_FSM_CLASS_QUEUE, &queue, &pages);
		lag = quarantine_lag(rel, &queue);
		for (int i = 0; i < queue.n; i++)
		{
			const SiftFsmExtent *q = &queue.v[i];
			bool		meta = sift_fsm_slab_of(SLABS(root), q->start) != NULL;

			if (!matured(c, q->safexid, lag))
				sift_fsm_extvec_add(&pending, q->start, q->nblocks, q->safexid);
			else if (meta_only && !meta)
				sift_fsm_extvec_add(&pending, q->start, q->nblocks, InvalidFullTransactionId);
			else
			{
				if (FullTransactionIdIsValid(q->safexid) && (!FullTransactionIdIsValid(max_matured) || FullTransactionIdFollows(q->safexid, max_matured)))
					max_matured = q->safexid;
				if (meta)
					sift_fsm_extvec_add(&metafree, q->start, q->nblocks, InvalidFullTransactionId);
				else
				{
					sift_fsm_extvec_add(&data, q->start, q->nblocks, InvalidFullTransactionId);
					freed += q->nblocks;
				}
			}
		}
		if (new_hw == NULL && (meta_only ? metafree.n == 0 : queue.n == pending.n))
		{
			UnlockReleaseBuffer(rootbuf);
			return freed_before;
		}
		for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		{
			old_head[k] = m->class_head[k];
			if (!meta_only)
				sift_fsm_walk_list(rel, old_head[k], k, &listed[k], &listed_pages[k]);
			for (int i = 0; i < listed[k].n; i++)
				sift_fsm_extvec_add(&ready, listed[k].v[i].start, listed[k].v[i].nblocks, InvalidFullTransactionId);
		}
		for (int i = 0; i < ready.n; i++)
			sift_fsm_extvec_add(&data, ready.v[i].start, ready.v[i].nblocks, InvalidFullTransactionId);
		coalesce(rel, &data);
		if (new_hw != NULL && !claimed)
		{
			sift_fsm_slabs_idle(SLABS(root), meta_free, &idle);
			for (int i = 0; i < idle.n; i++)
			{
				sift_fsm_extvec_add(&data, idle.v[i].start, idle.v[i].nblocks, InvalidFullTransactionId);
				meta_free -= idle.v[i].nblocks;
			}
			coalesce(rel, &data);
		}
		for (int i = 0; i < data.n; i++)
		{
			BlockNumber start = data.v[i].start;

			if (new_hw != NULL && (uint64) start + data.v[i].nblocks == hw && data.v[i].nblocks >= SIFT_TAIL_SHRINK_MIN_BLOCKS)
				cut = start;
			else
				cut_into_classes(byclass, start, data.v[i].nblocks);
		}
		for (int k = 0; k < SIFT_FSM_NCLASSES && !meta_only; k++)
		{
			keep[k] = same_extents(&listed[k], &byclass[k]);
			if (!keep[k])
			{
				newpages += pages_for(byclass[k].n);
				for (int i = 0; i < listed_pages[k].n; i++)
					sift_fsm_extvec_add(&pages, listed_pages[k].v[i].start, 1, InvalidFullTransactionId);
			}
		}
		newpages += pages_for(pending.n);
		if (newpages > (int) meta_free)
		{
			UnlockReleaseBuffer(rootbuf);
			if (!meta_only && new_hw == NULL && metafree.n > 0)
				meta_only = true;
			else
			{
				sift_fsm_claim_slab(c);
				claimed = true;
			}
			continue;
		}

		{
			Buffer		wbuf;
			Page		wroot;
			SiftXlog	x;
			BlockNumber heads[SIFT_FSM_NCLASSES];
			BlockNumber qhead = InvalidBlockNumber, qtail = InvalidBlockNumber;
			BlockNumber *taken = palloc(Max(newpages, 1) * sizeof(BlockNumber));
			int			ntaken = 0;
			PGAlignedBlock scratch;

			(void) sift_wal_snapshot_conflict(rel, InvalidBlockNumber, max_matured);
			memcpy(scratch.data, root, BLCKSZ);
			sift_fsm_slabs_drop(scratch.data, &idle);
			for (int i = 0; i < newpages; i++)
				taken[i] = sift_fsm_slab_take(scratch.data);
			UnlockReleaseBuffer(rootbuf);

			for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
			{
				BlockNumber next = InvalidBlockNumber;

				if (meta_only || keep[k])
					continue;
				for (int p = pages_for(byclass[k].n) - 1; p >= 0; p--)
				{
					int			from = p * SIFT_FSM_PAGE_CAP,
								cnt = Min(byclass[k].n - from, (int) SIFT_FSM_PAGE_CAP);
					BlockNumber blk = taken[ntaken++];
					Page		pg;

					x = sift_xlog_start(rel);
					pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);
					sift_fsm_init_list_page(pg, k, 0, next);
					for (int j = 0; j < cnt; j++)
						FSMP(pg)->ext[j] = byclass[k].v[from + cnt - 1 - j];
					FSMP(pg)->nentries = cnt;
					sift_fsm_list_seal(pg);
					sift_xlog_done(&x);
					next = blk;
				}
				heads[k] = next;
			}
			{
				BlockNumber next = InvalidBlockNumber;

				for (int p = pages_for(pending.n) - 1; p >= 0; p--)
				{
					int			from = p * SIFT_FSM_PAGE_CAP,
								cnt = Min(pending.n - from, (int) SIFT_FSM_PAGE_CAP);
					BlockNumber blk = taken[ntaken++];
					Page		pg;

					x = sift_xlog_start(rel);
					pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);
					sift_fsm_init_list_page(pg, SIFT_FSM_CLASS_QUEUE, SIFT_FSM_PAGE_DEFERRED, next);
					memcpy(FSMP(pg)->ext, &pending.v[from], cnt * sizeof(SiftFsmExtent));
					FSMP(pg)->nentries = cnt;
					sift_fsm_list_seal(pg);
					sift_xlog_done(&x);
					if (qtail == InvalidBlockNumber)
						qtail = blk;
					next = blk;
				}
				qhead = next;
			}

			x = sift_xlog_start(rel);
			wbuf = sift_fsm_lock_root(rel);
			wroot = sift_xlog_register(&x, wbuf, 0);
			sift_fsm_slabs_drop(wroot, &idle);
			for (int i = 0; i < newpages; i++)
				if (sift_fsm_slab_take(wroot) != taken[i])
					SIFT_CORRUPT(rel, "The allocator changed under its own lock.");
			for (int i = 0; i < pages.n; i++)
				sift_fsm_slab_put(rel, wroot, pages.v[i].start);
			for (int i = 0; i < metafree.n; i++)
				for (uint32 b = 0; b < metafree.v[i].nblocks; b++)
					sift_fsm_slab_put(rel, wroot, metafree.v[i].start + b);
			m = FSMM(wroot);
			for (int k = 0; k < SIFT_FSM_NCLASSES && !meta_only; k++)
				if (!keep[k])
				{
					m->class_head[k] = heads[k];
					m->class_free_extents[k] = byclass[k].n;
				}
			m->quarantine_head = qhead;
			m->quarantine_tail = qtail;
			m->quarantine_extents = pending.n;
			m->quarantine_blocks = 0;
			for (int i = 0; i < pending.n; i++)
				m->quarantine_blocks += pending.v[i].nblocks;
			sift_fsm_root_seal(wroot);
			if (new_hw != NULL && cut != InvalidBlockNumber)
			{
				sift_fsm_set_high_water(c, &x, cut);
				sift_xlog_finish(&x);
				UnlockReleaseBuffer(wbuf);
			}
			else
				sift_xlog_done(&x);
		}
		for (int i = 0; i < metafree.n; i++)
			freed += metafree.v[i].nblocks;
		if (meta_only)
		{
			freed_before = freed;
			meta_only = false;
			continue;
		}
		if (new_hw != NULL)
			*new_hw = cut != InvalidBlockNumber ? cut : hw;
		return freed;
	}
}

uint64
sift_fsm_reclaim_locked(FsmCtx *c, BlockNumber *new_hw)
{
	bool		outer = !c->in_reclaim;
	uint64		freed;

	c->in_reclaim = true;
	freed = reclaim_pass(c, new_hw);
	if (outer && new_hw == NULL)
		(void) sift_fsm_recycle_empty_slabs(c);
	c->in_reclaim = !outer;
	return freed;
}

