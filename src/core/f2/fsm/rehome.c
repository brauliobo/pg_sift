/*
 * core/f2/fsm/rehome.c - the way down of the metadata that holds the end of the file (core/f2/fsm/fsm.h): a slab claimed at the end of the file while the blocks below were still held by what a
 * compaction was replacing keeps whatever page was written to it last, and the pages of the slabs below, which a commit fills first, are free again by the time the compaction is over. The
 * pages in use of the slabs at the end are taken down into the free pages below them (the manifest and the part headers by a commit, the lists of the allocator here), after the slabs below
 * are given room for them out of the free extents under them; the slabs that are left with no page in use go with the next cut of the file (sift_fsm_slabs_idle).
 */
#include "core/host.h"

#include "core/f2/fsm/int.h"
#include "core/page_io.h"

bool
sift_fsm_slab_room_below(Relation rel, BlockNumber from)
{
	uint32		lack = sift_fsm_meta_short_below(rel, from, true);
	SiftExtent	ext[SIFT_FSM_NCLASSES];
	int			n = 0;

	for (uint32 want = lack > 0 ? Min(pg_nextpower2_32(lack), SIFT_FSM_META_SLAB) : 0; want > 0 && n == 0; want /= 2)
		n = sift_fsm_alloc_below(rel, want, from, ext, lengthof(ext));
	if (n > 0)
	{
		FsmCtx		c = sift_fsm_ctx_open(rel);

		sift_fsm_adopt_slabs(&c, ext, n);
		sift_fsm_ctx_close(&c);
	}
	return sift_fsm_meta_short_below(rel, from, false) == 0;
}

static BlockNumber
head_of(Relation rel, int cls)
{
	Buffer		rootbuf = sift_fsm_lock_root(rel);
	SiftFsmMetaPageData *m = FSMM(BufferGetPage(rootbuf));
	BlockNumber head = cls == SIFT_FSM_CLASS_QUEUE ? m->quarantine_head : m->class_head[cls];

	UnlockReleaseBuffer(rootbuf);
	return head;
}

static BlockNumber
next_of(Relation rel, int cls, BlockNumber blk)
{
	Buffer		buf = sift_fsm_lock_list_page(rel, blk, cls);
	BlockNumber next = FSMP(BufferGetPage(buf))->next_blk;

	UnlockReleaseBuffer(buf);
	return next;
}

/* The page at blk, which follows prev in the list of cls, copied to the lowest free page of the slabs and the link to it changed in the same record, when that page is lower than blk: where the page is after the call */
static BlockNumber
rehome_page(FsmCtx *c, int cls, BlockNumber prev, BlockNumber blk)
{
	SiftXlog	x = sift_xlog_start(c->rel);
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	Page		root = sift_xlog_register(&x, rootbuf, 0);
	SiftFsmMetaPageData *m = FSMM(root);
	BlockNumber to = sift_fsm_slab_take(root);
	Buffer		old;
	Page		page;

	if (to == InvalidBlockNumber || to >= blk)
	{
		sift_xlog_abort(&x);
		UnlockReleaseBuffer(rootbuf);
		return blk;
	}
	old = sift_fsm_lock_list_page(c->rel, blk, cls);
	page = sift_xlog_register(&x, sift_blk_new(c->rel, to), GENERIC_XLOG_FULL_IMAGE);
	memcpy(page, BufferGetPage(old), BLCKSZ);
	UnlockReleaseBuffer(old);
	sift_fsm_list_seal(page);
	if (prev != InvalidBlockNumber)
	{
		Page		before = sift_xlog_register(&x, sift_fsm_lock_list_page(c->rel, prev, cls), 0);

		FSMP(before)->next_blk = to;
		sift_fsm_list_seal(before);
	}
	else if (cls == SIFT_FSM_CLASS_QUEUE)
		m->quarantine_head = to;
	else
		m->class_head[cls] = to;
	if (cls == SIFT_FSM_CLASS_QUEUE && m->quarantine_tail == blk)
		m->quarantine_tail = to;
	sift_fsm_slab_put(c->rel, root, blk);
	sift_fsm_root_seal(root);
	sift_xlog_done(&x);
	return to;
}

uint32
sift_fsm_rehome_lists(Relation rel, BlockNumber from)
{
	FsmCtx		c = sift_fsm_ctx_open(rel);
	uint32		moved = 0;

	for (int cls = 0; cls <= SIFT_FSM_CLASS_QUEUE; cls++)
	{
		BlockNumber prev = InvalidBlockNumber,
					blk = head_of(rel, cls);

		while (blk != InvalidBlockNumber)
		{
			BlockNumber next = next_of(rel, cls, blk),
						at = blk >= from ? rehome_page(&c, cls, prev, blk) : blk;

			moved += at != blk;
			prev = at;
			blk = next;
		}
	}
	sift_fsm_ctx_close(&c);
	return moved;
}
