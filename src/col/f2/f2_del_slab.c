/*
 * col/f2/f2_del_slab.c - see f2_del_slab.h.
 */
#include "core/host.h"

#include "col/f2/f2_del_slab.h"

static bool
has_tid(const uint64 *v, uint32 n, uint64 tid)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (v[mid] < tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && v[lo] == tid;
}

int
sift_del_slab_hits(const uint64 *sorted_tids, uint32 nrefs, const SiftPendDelRec *recs, int n, bool *hit)
{
	int			nhit = 0;

	for (int i = 0; i < n; i++)
	{
		hit[i] = (recs[i].tid & SIFT_PD_TID_SUCC) == 0 && has_tid(sorted_tids, nrefs, recs[i].tid & SIFT_PD_TID_MASK);
		nhit += hit[i];
	}
	return nhit;
}

bool
sift_l0_grace_due(uint32 rows, uint32 pages, bool inserters_settled)
{
	return rows >= SIFT_L0_HEAPREF_FLUSH_ROWS / 2 || pages >= SIFT_L0_GRACE_PAGES || inserters_settled;
}

#ifndef SIFT_S49_STANDALONE

#include "col/f2/f2.h"
#include "col/f2/l0_href_fetch.h"
#include "core/page_io.h"
#include "core/xid_cache.h"

static int
tid_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* The deletes of a hit whose deleter committed before every snapshot: the version they name is dead to all, and is the set the flush leaves out */
static SiftPendDelIndex *
settled_deletes(const SiftPendDelRec *recs, int n, const bool *hit, bool *settled, int *nsettled)
{
	SiftXidHorizonCache hc;
	uint64	   *tids = palloc(Max(n, 1) * sizeof(uint64));
	SiftPendDelIndex *px;

	sift_xid_horizon_init(&hc, InvalidFullTransactionId);
	*nsettled = 0;
	for (int i = 0; i < n; i++)
	{
		settled[i] = hit[i] && TransactionIdIsNormal(recs[i].xmax) && sift_xid_horizon_status(&hc, recs[i].xmax) == SIFT_H_COMMITTED_OLD;
		if (settled[i])
			tids[(*nsettled)++] = recs[i].tid & SIFT_PD_TID_MASK;
	}
	px = *nsettled > 0 ? sift_pdel_idx_build(tids, *nsettled) : NULL;
	pfree(tids);
	return px;
}

/* Some delete names a row of the inline slab: only then is the slab sorted into a part, so that the delete finds the row */
static bool
slab_holds_deleted(Relation rel, uint64 part_id, BlockNumber head, const SiftPartSpec *spec, const SiftPendDelRec *recs, int n)
{
	SiftRowBuf *rb = sift_rowbuf_new(spec);
	uint64	   *tids;
	bool	   *hit = palloc(Max(n, 1) * sizeof(bool));
	int			nhit;

	sift_l0_slab_read(rel, part_id, head, rb, spec, NULL, NULL, NULL);
	tids = memcpy(palloc(Max(rb->n, 1) * sizeof(uint64)), rb->tid, rb->n * sizeof(uint64));
	qsort(tids, rb->n, sizeof(uint64), tid_cmp);
	nhit = sift_del_slab_hits(tids, (uint32) rb->n, recs, n, hit);
	pfree(tids);
	pfree(hit);
	sift_rowbuf_free(rb);
	return nhit > 0;
}

int
sift_del_slab_flush(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec, BlockNumber tail, const SiftPendDelRec *recs, int n, bool *done)
{
	SiftHrefIo	h;
	const char *why;
	BlockNumber bad = InvalidBlockNumber,
			   *pages;
	uint32		nrefs,
				npages;
	SiftL0HeapRef *refs;
	uint64	   *tids;
	bool	   *hit,
			   *settled;
	int			nsettled;
	SiftPendDelIndex *px;
	SiftRowBuf *rb;

	if (!sift_l0_slab_is_href(rel, part_id, tail))
		return slab_holds_deleted(rel, part_id, tail, spec, recs, n) && sift_l0_slab_flush(rel, part_id, key_lo, spec) ? 0 : -1;
	sift_href_io_init(&h, rel, (uint32) part_id);
	refs = sift_l0_href_chain_read(&h.io, tail, &nrefs, &pages, &npages, &why, &bad);
	if (refs == NULL)
		SIFT_CORRUPT(rel, "Block %u: the slab of partition %u is damaged: %s.", bad, (uint32) part_id, why);
	tids = palloc(Max(nrefs, 1) * sizeof(uint64));
	for (uint32 i = 0; i < nrefs; i++)
		tids[i] = sift_f2_tid(&refs[i].ctid);
	qsort(tids, nrefs, sizeof(uint64), tid_cmp);
	hit = palloc0(Max(n, 1) * sizeof(bool));
	settled = palloc0(Max(n, 1) * sizeof(bool));
	if (sift_del_slab_hits(tids, nrefs, recs, n, hit) == 0)
	{
		pfree(refs);
		pfree(tids);
		return -1;
	}
	px = settled_deletes(recs, n, hit, settled, &nsettled);
	rb = sift_rowbuf_new(spec);
	sift_href_rows(rel, NULL, px, refs, nrefs, rb);
	sift_l0_slab_publish(rel, part_id, key_lo, spec, rb, pages, (int) npages);
	for (int i = 0; i < n; i++)
		done[i] |= settled[i];
	sift_rowbuf_free(rb);
	sift_pdel_idx_free(px);
	pfree(refs);
	pfree(tids);
	return nsettled;
}

bool
sift_l0_href_flush_due(Relation rel, BlockNumber tail, FullTransactionId horizon)
{
	Buffer		buf = sift_blk_read(rel, tail, BUFFER_LOCK_SHARE);
	const SiftL0SlabPageData *hd = (const SiftL0SlabPageData *) SiftPageGetData(BufferGetPage(buf));
	bool		due = (hd->flags & SIFT_L0_SLAB_HEAPREF) == 0 ||
		sift_l0_grace_due(hd->chain_rows, hd->chain_pages, FullTransactionIdPrecedes(hd->max_xid, horizon));

	UnlockReleaseBuffer(buf);
	return due;
}

#endif
