/*
 * core/f2/fsm/check.c - what the allocator of format major 2 says of its own metadata: whether a block is one of the metadata slabs and the check of every list against the counts of the root (core/f2/fsm/fsm.c).
 */
#include "core/host.h"

#include "col/f2/meta.h"
#include "core/f2/fsm/int.h"
#include "core/popcount.h"

bool
sift_fsm_is_meta_block(Relation rel, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	SiftFsmSlabDir d;
	bool		is;

	sift_fsm_check_root(rel, buf);
	d = sift_fsm_dir_read(rel, BufferGetPage(buf), NULL);
	is = sift_fsm_dir_find(&d, blk) >= 0;
	sift_fsm_dir_free(&d);
	UnlockReleaseBuffer(buf);
	return is;
}

/* The slabs cut at block from: the pages in use of those that start at or above it, the free pages and the number of those below */
typedef struct SlabSplit
{
	uint32		used;
	uint32		room;
	uint32		below;
} SlabSplit;

static SlabSplit
split_slabs(Relation rel, BlockNumber from)
{
	Buffer		buf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	SiftFsmSlabDir d;
	SlabSplit	s = {0};

	sift_fsm_check_root(rel, buf);
	d = sift_fsm_dir_read(rel, BufferGetPage(buf), NULL);
	for (uint32 k = 0; k < d.n; k++)
		if (d.slab[k]->start >= from)
			s.used += sift_fsm_slab_used(d.slab[k]);
		else
		{
			s.room += d.slab[k]->nblocks - sift_fsm_slab_used(d.slab[k]);
			s.below++;
		}
	sift_fsm_dir_free(&d);
	UnlockReleaseBuffer(buf);
	return s;
}

/* The first slab, which holds the root of the partition directory, is never the one */
BlockNumber
sift_fsm_slab_start_ending_at(Relation rel, BlockNumber end)
{
	Buffer		buf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	SiftFsmSlabDir d;
	BlockNumber start = InvalidBlockNumber;

	sift_fsm_check_root(rel, buf);
	d = sift_fsm_dir_read(rel, BufferGetPage(buf), NULL);
	for (uint32 k = 1; k < d.n; k++)
		if (d.slab[k]->start + d.slab[k]->nblocks == end)
			start = d.slab[k]->start;
	sift_fsm_dir_free(&d);
	UnlockReleaseBuffer(buf);
	return start;
}

uint32
sift_fsm_meta_used_from(Relation rel, BlockNumber from)
{
	return split_slabs(rel, from).used;
}

uint32
sift_fsm_meta_short_below(Relation rel, BlockNumber from, bool for_move)
{
	SlabSplit	s = split_slabs(rel, from);
	uint32		want = s.used + (for_move ? (s.used > 0 ? SIFT_FSM_REHOME_SLACK : 0) + sift_fsm_meta_reserve(s.below) : 0);

	return want > s.room ? want - s.room : 0;
}

SiftFsmStats
sift_fsm_check(Relation rel)
{
	SiftFsmStats st = {0};
	Buffer		buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_SHARE);
	const SiftColMetaPageData *meta = SIFT_COL_META(BufferGetPage(buf0));
	Buffer		rootbuf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	Page		root = BufferGetPage(rootbuf);
	SiftFsmMetaPageData *m = FSMM(root);
	ExtVec		all = {0}, pages = {0}, queue = {0};
	SiftFsmSlabDir dir;
	uint64		freebits = 0,
				tail_end;

	sift_fsm_check_root(rel, rootbuf);
	tail_end = meta->high_water;
	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
	{
		ExtVec		ready = {0};

		sift_fsm_walk_list(rel, m->class_head[k], k, &ready, &pages);
		if ((uint32) ready.n != m->class_free_extents[k])
			SIFT_CORRUPT(rel, "Class %d lists %d extents, the root counts %u.", k, ready.n, m->class_free_extents[k]);
		for (int i = 0; i < ready.n; i++)
		{
			if (ready.v[i].nblocks != SIFT_FSM_CLASS_BLOCKS(k) || ready.v[i].start < meta->meta_slab_end)
				SIFT_CORRUPT(rel, "A free extent of class %d is %u blocks at block %u.", k, ready.v[i].nblocks, ready.v[i].start);
			st.class_blocks[k] += ready.v[i].nblocks;
			sift_fsm_extvec_add(&all, ready.v[i].start, ready.v[i].nblocks, InvalidFullTransactionId);
		}
		st.class_extents[k] = ready.n;
	}
	sift_fsm_walk_list(rel, m->quarantine_head, SIFT_FSM_CLASS_QUEUE, &queue, &pages);
	if ((uint32) queue.n != m->quarantine_extents)
		SIFT_CORRUPT(rel, "The queue lists %d extents, the root counts %u.", queue.n, m->quarantine_extents);
	for (int i = 0; i < queue.n; i++)
	{
		st.quarantine_blocks += queue.v[i].nblocks;
		sift_fsm_extvec_add(&all, queue.v[i].start, queue.v[i].nblocks, InvalidFullTransactionId);
	}
	if (st.quarantine_blocks != m->quarantine_blocks)
		SIFT_CORRUPT(rel, "The queue holds %llu blocks, the root counts %llu.", (unsigned long long) st.quarantine_blocks, (unsigned long long) m->quarantine_blocks);
	dir = sift_fsm_dir_read(rel, root, NULL);
	if (((meta->common.ro_compat_flags & SIFT_ROCOMPAT_FSM_CHAIN) != 0) != (dir.npages > 1))
		SIFT_CORRUPT(rel, "The root names %d pages of metadata slabs, the metapage says %s.", dir.npages - 1, (meta->common.ro_compat_flags & SIFT_ROCOMPAT_FSM_CHAIN) ? "some" : "none");
	for (uint32 k = 0; k < dir.n; k++)
	{
		for (int w = 0; w < SIFT_FSM_META_SLAB / 64; w++)
			freebits += pg_popcount64(dir.slab[k]->free[w]);
		st.meta_total += dir.slab[k]->nblocks;
	}
	if (freebits != m->meta_free_count)
		SIFT_CORRUPT(rel, "The slabs have %llu free blocks, the root counts %u.", (unsigned long long) freebits, m->meta_free_count);
	qsort(all.v, all.n, sizeof(SiftFsmExtent), sift_fsm_ext_cmp);
	for (int i = 0; i < all.n; i++)
		if ((uint64) all.v[i].start + all.v[i].nblocks > meta->high_water ||
			(i > 0 && (uint64) all.v[i - 1].start + all.v[i - 1].nblocks > all.v[i].start))
			SIFT_CORRUPT(rel, "Free or queued extents overlap or pass the high-water mark at block %u.", all.v[i].start);
	for (int i = all.n - 1; i >= 0 && (uint64) all.v[i].start + all.v[i].nblocks == tail_end; i--)
	{
		st.tail_blocks += all.v[i].nblocks;
		tail_end = all.v[i].start;
	}
	st.meta_free = freebits;
	{
		ExtVec		idle = {0};

		sift_fsm_slabs_idle(SLABS(root), m->meta_free_count, &idle);
		for (int i = 0; i < idle.n; i++)
			st.meta_idle += idle.v[i].nblocks;
	}
	st.quarantine_extents = queue.n;
	st.pages = pages.n + dir.npages - 1;
	sift_fsm_dir_free(&dir);
	UnlockReleaseBuffer(rootbuf);
	UnlockReleaseBuffer(buf0);
	return st;
}
