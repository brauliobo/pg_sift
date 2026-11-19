/*
 * col/scan/memload.c - the write buffers a column scan reads: loaded whole, or, for a scan that asks for some keys, the pages that may hold them
 */
#include "postgres.h"
#include "col/scan_int.h"

/*
 * The batches of the write buffers for a scan that asks for the rows with a key in *keys (NULL: every row). A buffer that has the range of the keys in the head of its pages
 * (SIFT_COL_FORMAT_WBUFKEY) is read page by page for the rows that may have one when that costs less than reading it whole (sift_cwbuf_batches_keys): the batches are then
 * partial, without tokens (what a cache of the scan keys on is a kept set), and mem_partial says so.
 */
void
scan_load_wbuf_keys(SiftColSource *src, const SiftCWbufKeys *keys)
{
	bool		own = src->nwtok >= 0,
				partial = false;

	if (own)
	{
		src->wtok = MemoryContextAllocZero(src->mcxt, Max(src->dir->nsegs, 1) * sizeof(SiftCWbufToken));
		src->nwtok = 0;
	}
	for (int i = 0; i < src->dir->nsegs; i++)
	{
		int			n;
		bool		seg_partial;
		SiftColBatch **bs;
		TransactionId stamp = InvalidTransactionId;

		if (!sift_seg_is_wbuf(&src->dir->segs[i]))
			continue;
		/*
		 * The version the scan read at open, not the current one, and that is enough. VACUUM bumps it after it flags records and before the heap frees their
		 * addresses; batches kept at another version have their pages checked, and the flags the version shows are found. A flag it does not show (set after
		 * the open, or before a bump the open preceded) leaves the batches as they were: its record belongs to a row dead to every snapshot, since the snapshot
		 * of this scan, taken before the open, holds the heap back from removing anything it can see. An address freed after the open belongs to a row inserted
		 * after that snapshot, which the heap check does not return.
		 */
		bs = sift_cwbuf_batches_keys(src->index, &src->dir->segs[i], src->schema, src->dir->version, keys, src->mcxt, &n, own ? &src->wtok[src->nwtok++] : NULL, &stamp, &seg_partial);
		partial |= seg_partial;
		for (int j = 0; j < n; j++)
			scan_add_mem(src, bs[j], true, stamp, (src->dir->segs[i].flags & SIFT_SEG_ROOTS) != 0);
	}
	if (partial)
	{
		src->wtok = NULL;
		src->mem_keys = *keys;
	}
	src->mem_partial = partial;
	src->mem_loaded = true;
}

void
scan_load_wbuf(SiftColSource *src)
{
	scan_load_wbuf_keys(src, NULL);
}

/*
 * The keys every row a begin selects has: the predicates are ANDed, so the rows that satisfy them have a key in the range of the conjunction of those that bound the key leaf (an
 * integer leaf of the index's key_cut, which the pages of a buffer hold the range of). NULL when none of them does. The units a zone test prunes are pruned by the same predicates.
 */
static const SiftCWbufKeys *
scan_wbuf_keys(SiftColSource *src, const SiftColPred *preds, int npreds, SiftCWbufKeys *keys)
{
	int			leaf;
	bool		found = false;

	if ((src->flags & COLO_TABLE) || (leaf = sift_cwbuf_key_leaf(src->index)) < 0)
		return NULL;
	*keys = (SiftCWbufKeys) {leaf, PG_INT64_MIN, PG_INT64_MAX};
	for (int i = 0; i < npreds; i++)
	{
		const SiftColPred *p = &preds[i];

		if (p->col != leaf || p->nvals < 1)
			continue;
		switch (p->op)
		{
			case SIFT_COLOP_EQ:
				keys->lo = Max(keys->lo, p->vals[0].v.i);
				keys->hi = Min(keys->hi, p->vals[0].v.i);
				break;
			case SIFT_COLOP_LT:
				keys->hi = Min(keys->hi, p->vals[0].v.i == PG_INT64_MIN ? PG_INT64_MIN : p->vals[0].v.i - 1);
				keys->lo = p->vals[0].v.i == PG_INT64_MIN ? PG_INT64_MAX : keys->lo;
				break;
			case SIFT_COLOP_LE:
				keys->hi = Min(keys->hi, p->vals[0].v.i);
				break;
			case SIFT_COLOP_GT:
				keys->lo = Max(keys->lo, p->vals[0].v.i == PG_INT64_MAX ? PG_INT64_MAX : p->vals[0].v.i + 1);
				keys->hi = p->vals[0].v.i == PG_INT64_MAX ? PG_INT64_MIN : keys->hi;
				break;
			case SIFT_COLOP_GE:
				keys->lo = Max(keys->lo, p->vals[0].v.i);
				break;
			case SIFT_COLOP_IN:
				{
					int64		lo = p->vals[0].v.i,
								hi = lo;

					for (int k = 1; k < p->nvals; k++)
					{
						lo = Min(lo, p->vals[k].v.i);
						hi = Max(hi, p->vals[k].v.i);
					}
					keys->lo = Max(keys->lo, lo);
					keys->hi = Min(keys->hi, hi);
					break;
				}
			default:
				continue;
		}
		found = true;
	}
	return found ? keys : NULL;
}

/*
 * The batches of the write buffers a begin that asks for keys reads: loaded for the keys of the first begin, and again for the whole buffers when a later begin of the source asks for
 * keys they were not loaded for (a rescan with other parameters, a pass without conditions on the key).
 */
static void
scan_mem_for(SiftColSource *src, const SiftCWbufKeys *keys)
{
	if (src->flags & SIFT_COLO_NOWBUF)
		return;
	if (src->mem_partial && !(keys != NULL && keys->leaf == src->mem_keys.leaf && keys->lo >= src->mem_keys.lo && keys->hi <= src->mem_keys.hi))
	{
		src->nmem = 0;
		src->nwtok = 0;
		src->mem_loaded = src->mem_partial = false;
		keys = NULL;
	}
	if (!src->mem_loaded)
		scan_load_wbuf_keys(src, keys);
}

/* The batches of the write buffers a begin of a scan with these predicates reads: those that may hold a row of the keys they ask for (scan_mem_for) */
void
scan_load_mem(SiftColSource *src, const SiftColPred *preds, int npreds)
{
	SiftCWbufKeys keys;

	scan_mem_for(src, scan_wbuf_keys(src, preds, npreds, &keys));
}
