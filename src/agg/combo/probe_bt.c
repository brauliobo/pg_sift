/*
 * agg/combo/probe_bt.c - combos: the btree side of the direct probe of a correlated child
 * The compare of a key with an index tuple, the descent from the root or from the pages of the key before, the walk over the entries of the key in
 * index order and the fetch of their heap tuples, given to a sink (dp_batch; the interface is described in combo/probe.h, the combo's side is combo/probe.c).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/tableam.h"
#include "access/tupmacs.h"
#include "access/visibilitymap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/uuid.h"

#include "agg/lookup/int.h"
#include "agg/combo/probe.h"

static inline int
dp_sort_cmp(DKey *x, DKey *y)
{
	for (int i = 0; i < DP_ABBREV / 8; i++)
		if (x->ab[i] != y->ab[i])
			return x->ab[i] < y->ab[i] ? -1 : 1;
	return 0;
}

typedef DKey *DKeyP;

#define ST_SORT dp_sort_keys
#define ST_ELEMENT_TYPE DKeyP
#define ST_COMPARE(a, b) dp_sort_cmp(*(a), *(b))
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

/* the prefix of the key that sorts like the index does, for the columns whose order is the bytes (the others add nothing: the order only decides how many pages the descents share) */
void
dp_abbrev(DProbe *dp, DKey *k)
{
	unsigned char buf[DP_ABBREV] = {0};
	int			at = 0;

	for (int i = 0; i < dp->nk && at < DP_ABBREV; i++)
	{
		const DCol *c = &dp->col[i];
		const DArg *a = &k->a[i];

		if (c->kind == DC_INT)
		{
			uint64		v = (uint64) a->i ^ (UINT64CONST(1) << (c->len * 8 - 1));

			for (int b = c->len - 1; b >= 0 && at < DP_ABBREV; b--)
				buf[at++] = (unsigned char) (c->desc ? ~(v >> (8 * b)) : (v >> (8 * b)));
		}
		else if (c->kind == DC_UUID)
		{
			for (int b = 0; b < UUID_LEN && at < DP_ABBREV; b++)
				buf[at++] = c->desc ? ~((const unsigned char *) a->p)[b] : ((const unsigned char *) a->p)[b];
		}
		else if (c->kind == DC_TEXT || c->kind == DC_TEXTC)
		{
			Size		m = Min(a->n, (Size) (DP_ABBREV - at));

			memcpy(buf + at, a->p, m);
			at += m;
			if (at < DP_ABBREV)
				buf[at++] = 0;
		}
	}
	for (int w = 0; w < DP_ABBREV / 8; w++)
	{
		uint64		x;

		memcpy(&x, buf + 8 * w, 8);
		k->ab[w] = pg_bswap64(x);
	}
}

static inline int
dp_sign(int64 v)
{
	return (v > 0) - (v < 0);
}

/* the bytes of a varlena of an index tuple (a compressed one is expanded in the batch context) */
static pg_attribute_always_inline const char *
dp_bytes(DProbe *dp, const char *p, Size *n)
{
	if (unlikely(VARATT_IS_COMPRESSED(p) || VARATT_IS_EXTERNAL(p)))
	{
		MemoryContext old = MemoryContextSwitchTo(dp->bcxt);
		struct varlena *v = pg_detoast_datum_packed((struct varlena *) p);

		MemoryContextSwitchTo(old);
		*n = VARSIZE_ANY_EXHDR(v);
		return VARDATA_ANY(v);
	}
	*n = VARSIZE_ANY_EXHDR(p);
	return VARDATA_ANY(p);
}

/* the sign of key - value of an index column: what _bt_compare reports for one attribute (the sign flipped back for a DESC column) */
static pg_attribute_always_inline int
dp_col_cmp(DProbe *dp, const DCol *c, const DArg *a, const char *p)
{
	int			r;

	switch (c->kind)
	{
		case DC_INT:
			{
				int64		v;

				if (c->len == 2)
					v = *(const int16 *) p;
				else if (c->len == 4)
					v = *(const int32 *) p;
				else
					v = *(const int64 *) p;
				r = (a->i > v) - (a->i < v);
				break;
			}
		case DC_TEXT:
			{
				Size		n;
				const char *b;

				if (unlikely(VARATT_IS_COMPRESSED(p) || VARATT_IS_EXTERNAL(p)))
				{
					b = dp_bytes(dp, p, &n);
					r = memcmp(a->p, b, Min(a->n, n));
					r = r != 0 ? dp_sign(r) : (a->n > n) - (a->n < n);
					break;
				}
				b = VARDATA_ANY(p);
				n = VARSIZE_ANY_EXHDR(p);
				r = memcmp(a->p, b, Min(a->n, n));
				r = r != 0 ? dp_sign(r) : (a->n > n) - (a->n < n);
				break;
			}
		case DC_UUID:
			r = dp_sign(memcmp(a->p, p, UUID_LEN));
			break;
		case DC_TEXTC:
		case DC_ARRAY:
			{
				Size		n;
				const char *b = dp_bytes(dp, p, &n);

				if (n == a->n && memcmp(a->p, b, n) == 0)
				{
					r = 0;
					break;
				}
			}
			sift_fallthrough;
		default:
			{
				Datum		item = c->byval ? fetch_att(p, true, c->len) : PointerGetDatum(p);

				DP_COUNT(generic_cmps, 1);
				r = DatumGetInt32(FunctionCall2Coll(c->cmp, c->coll, item, a->d));
				return c->desc ? dp_sign(r) : -dp_sign(r);
			}
	}
	return c->desc ? -r : r;
}

/*
 * The sign of key - item as _bt_compare reports it for a forward search with the key's columns and no heap TID: the attributes the item has, then a
 * key with more attributes than a truncated pivot is greater. An item equal in every attribute is equal; the walk to the right that follows every
 * descent makes a descent that ends one page too far left harmless, never one that ends too far right.
 */
static inline int
dp_cmp_tuple(DProbe *dp, const DKey *k, IndexTuple itup, int ntupatts, int *eq)
{
	int			ncmp = Min(ntupatts, dp->nk);
	int			from = Min(*eq, ncmp);

	DP_COUNT(tuple_cmps, 1);

	if (!IndexTupleHasNulls(itup))
	{
		const char *data = (const char *) itup + IndexInfoFindDataOffset(itup->t_info);
		long		off = 0;

		for (int i = 0; i < ncmp; i++)
		{
			const DCol *c = &dp->col[i];
			const char *p;

			/* a fixed width or a varlena column (the short header of a varlena has no padding in front of it) */
			if (c->len > 0 || (c->len == -1 && data[off] == 0))
				off = TYPEALIGN(c->alignb, off);
			else if (unlikely(c->len < -1))
				off = att_align_pointer(off, c->align, c->len, data + off);
			p = data + off;
			if (i >= from)
			{
				int			r = dp_col_cmp(dp, c, &k->a[i], p);

				DP_COUNT(col_cmps, 1);

				if (r != 0)
				{
					*eq = i;
					return r;
				}
			}
			off += c->len > 0 ? c->len : c->len == -1 ? VARSIZE_ANY(p) : strlen(p) + 1;
		}
	}
	else
	{
		TupleDesc	td = RelationGetDescr(dp->irel);

		for (int i = from; i < ncmp; i++)
		{
			const DCol *c = &dp->col[i];
			bool		isnull;
			Datum		item = index_getattr(itup, i + 1, td, &isnull);
			int			r;

			DP_COUNT(col_cmps, 1);
			if (isnull)
				r = c->nullsfirst ? 1 : -1;		/* the key is not NULL */
			else if (c->byval || c->len != -1)
			{
				/* a fixed width value, the key's columns are not NULL: compare through the order function */
				r = DatumGetInt32(FunctionCall2Coll(c->cmp, c->coll, item, k->a[i].d));
				r = c->desc ? dp_sign(r) : -dp_sign(r);
			}
			else
			{
				r = dp_col_cmp(dp, c, &k->a[i], DatumGetPointer(item));
			}
			if (r != 0)
			{
				*eq = i;
				return r;
			}
		}
	}
	*eq = ncmp;
	if (dp->nk > ntupatts)
		return 1;
	/*
	 * Every key attribute is equal and a pivot's heap TID was truncated away (minus infinity): the entries equal to the key come after it, none
	 * before (what is left of it differs from it in a key attribute, so is less). With fewer key columns than the index has a smaller entry could
	 * still match the columns, so the search goes left of the pivot there and walks right.
	 */
	if (dp->nk == dp->nkeyatts && BTreeTupleIsPivot(itup) && BTreeTupleGetHeapTID(itup) == NULL)
		return 1;
	return 0;
}

static inline int
dp_cmp_item(DProbe *dp, const DKey *k, Page page, BTPageOpaque op, OffsetNumber off, int *eq)
{
	IndexTuple	itup;

	if (!P_ISLEAF(op) && off == P_FIRSTDATAKEY(op))
		return 1;				/* minus infinity */
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
	return dp_cmp_tuple(dp, k, itup, BTreeTupleGetNAtts(itup, dp->irel), eq);
}

/* the sign of key - high key of a page, and the columns they agree on (`eq` in: the columns the key's range is known to agree on) */
static inline int
dp_cmp_hikey(DProbe *dp, const DKey *k, Page page, int *eq)
{
	IndexTuple	hk = (IndexTuple) PageGetItem(page, PageGetItemId(page, P_HIKEY));

	return dp_cmp_tuple(dp, k, hk, BTreeTupleGetNAtts(hk, dp->irel), eq);
}

static bool
dp_page_ok(Page page)
{
	return !PageIsNew(page) && PageGetSpecialSize(page) == MAXALIGN(sizeof(BTPageOpaqueData));
}

/* the root of the index (the fast root): false if the metapage is not the current format */
static bool
dp_root(DProbe *dp, int *level)
{
	/* the index AM's own start of a scan: the relation cache's copy of the metapage when it has one (a root split invalidates it, and the root page is checked), else the metapage */
	Buffer		buf = _bt_getroot(dp->irel, dp->hrel, BT_READ);
	Page		page;
	bool		ok;

	if (!BufferIsValid(buf))
	{
		dp->root = P_NONE;			/* an empty index */
		*level = 0;
		return true;
	}
	page = BufferGetPage(buf);
	ok = dp_page_ok(page);
	if (ok)
	{
		dp->root = BufferGetBlockNumber(buf);
		*level = (int) BTPageGetOpaque(page)->btpo_level;
		ok = *level < DP_MAX_LEVELS;
	}
	UnlockReleaseBuffer(buf);
	return ok;
}

static void
dp_note_page(DProbe *dp, int lv, Buffer buf, Page page, BTPageOpaque op)
{
	DLevel	   *l = &dp->lv[lv];

	l->blk = BufferGetBlockNumber(buf);
	l->has_hik = false;
	if (!P_RIGHTMOST(op))
	{
		IndexTuple	hk = (IndexTuple) PageGetItem(page, PageGetItemId(page, P_HIKEY));
		Size		sz = IndexTupleSize(hk);

		if (sz <= BLCKSZ / 2)
		{
			memcpy(l->hik, hk, sz);
			l->has_hik = true;
		}
		else
			l->blk = InvalidBlockNumber;	/* never the start of a later descent */
	}
}

/*
 * The pages above the leaves are pinned for the batch: the descents of sorted keys read the same few of them again and again, and a pin of an internal page blocks
 * no VACUUM (it asks a cleanup lock of leaves only). dp_read gives the page of level lv share-locked, the pin of the level kept in dp->pin; dp_unlock
 * ends the lock and the pin of a leaf, and the lock only of a kept one; dp_unpin ends the pins of the batch.
 */
static Buffer
dp_read(DProbe *dp, int lv, BlockNumber blk)
{
	Buffer		buf;

	if (lv > 0 && BufferIsValid(dp->pin[lv]) && BufferGetBlockNumber(dp->pin[lv]) == blk)
		buf = dp->pin[lv];
	else
	{
		DP_COUNT(page_reads, 1);
		buf = ReadBuffer(dp->irel, blk);
		if (lv > 0)
		{
			if (BufferIsValid(dp->pin[lv]))
				ReleaseBuffer(dp->pin[lv]);
			dp->pin[lv] = buf;
		}
	}
	LockBuffer(buf, BT_READ);
	return buf;
}

static void
dp_unlock(DProbe *dp, int lv, Buffer buf)
{
	if (lv > 0 && buf == dp->pin[lv])
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	else
		UnlockReleaseBuffer(buf);
}

static void
dp_unpin(DProbe *dp)
{
	for (int lv = 1; lv < DP_MAX_LEVELS; lv++)
		if (BufferIsValid(dp->pin[lv]))
		{
			ReleaseBuffer(dp->pin[lv]);
			dp->pin[lv] = InvalidBuffer;
		}
}

/*
 * The leaf that holds the first entry that is not less than the key, share-locked, and the offset of that entry; InvalidBuffer when the pages are not
 * what a descent expects (the key is left to the child). A descent that starts from the pages of the key before ("reuse") is checked at the leaf: it
 * must hold an entry less than the key, or be the leftmost page, so that no page to its left can hold an entry of the key; else the caller descends again from the root.
 */
static Buffer
dp_descend(DProbe *dp, const DKey *k, bool reuse, OffsetNumber *offp, bool *retry)
{
	int			lv = 0;
	BlockNumber blk;
	Buffer		buf;
	int			eqc = 0;		/* the columns on which the key agrees with both bounds of the page's range: every item of the page agrees with it on them */
	int			hik_c = 0,		/* the compare of the key with the high key the first page was left with, when that is the one the page still has */
				hik_eq = 0;
	bool		hik_known = false;

	*retry = false;
	dp->first_c = DP_NOCMP;
	if (reuse)
	{
		for (lv = 0; lv < dp->nlv - 1; lv++)
		{
			const DLevel *l = &dp->lv[lv];

			if (l->blk == InvalidBlockNumber)
				continue;
			if (!l->has_hik)
				break;
			hik_eq = 0;
			hik_c = dp_cmp_tuple(dp, k, (IndexTuple) l->hik, BTreeTupleGetNAtts((IndexTuple) l->hik, dp->irel), &hik_eq);
			if (hik_c <= 0)
			{
				hik_known = true;
				break;
			}
		}
		blk = lv == dp->nlv - 1 ? dp->root : dp->lv[lv].blk;
		/*
		 * The pages above the start are not read by this key. Every so often they are pinned and released, so that the clock of the buffer pool still sees
		 * them used by the probes that skip them: a pool of a few thousand buffers would otherwise age the root out between two batches.
		 */
		if ((++dp->skipped & 31) == 0)
			for (int up = lv + 1; up < dp->nlv; up++)
				ReleaseBuffer(ReadBuffer(dp->irel, up == dp->nlv - 1 ? dp->root : dp->lv[up].blk));
	}
	else
	{
		lv = dp->nlv - 1;
		blk = dp->root;
	}
	for (;;)
	{
		Page		page;
		BTPageOpaque op;
		OffsetNumber low,
					high;
		int			lo_eq,
					hi_eq,
					hi_c = DP_NOCMP;

		buf = dp_read(dp, lv, blk);
		page = BufferGetPage(buf);
		if (!dp_page_ok(page))
			goto bad;
		op = BTPageGetOpaque(page);
		hi_eq = eqc;
		for (;;)
		{
			BlockNumber next = op->btpo_next;

			if (!P_IGNORE(op) && P_RIGHTMOST(op))
				break;
			if (!P_IGNORE(op))
			{
				int			c;

				if (hik_known && blk == dp->lv[lv].blk &&
					IndexTupleSize((IndexTuple) PageGetItem(page, PageGetItemId(page, P_HIKEY))) == IndexTupleSize((IndexTuple) dp->lv[lv].hik) &&
					memcmp(PageGetItem(page, PageGetItemId(page, P_HIKEY)), dp->lv[lv].hik, IndexTupleSize((IndexTuple) dp->lv[lv].hik)) == 0)
				{
					c = hik_c;
					hi_eq = hik_eq;
				}
				else
				{
					hi_eq = eqc;
					c = dp_cmp_hikey(dp, k, page, &hi_eq);
				}
				if (c <= 0)
					break;
			}
			hik_known = false;
			if (P_RIGHTMOST(op) || next == P_NONE)
				goto bad;
			dp_unlock(dp, lv, buf);
			buf = dp_read(dp, lv, next);
			page = BufferGetPage(buf);
			if (!dp_page_ok(page))
				goto bad;
			op = BTPageGetOpaque(page);
			eqc = 0;			/* beyond the range the parent gave */
			hi_eq = 0;
		}
		hik_known = false;
		if ((int) op->btpo_level != lv || P_ISLEAF(op) != (lv == 0))
			goto bad;
		dp_note_page(dp, lv, buf, page, op);
		low = P_FIRSTDATAKEY(op);
		high = PageGetMaxOffsetNumber(page) + 1;
		lo_eq = eqc;
		while (low < high)
		{
			OffsetNumber mid = low + ((high - low) >> 1);
			int			e = Min(lo_eq, hi_eq);
			int			c = dp_cmp_item(dp, k, page, op, mid, &e);

			if (c > 0)
			{
				low = mid + 1;
				lo_eq = e;
			}
			else
			{
				high = mid;
				hi_eq = e;
				hi_c = c;
			}
		}
		if (lv == 0)
		{
			if (reuse && low == P_FIRSTDATAKEY(op) && !P_LEFTMOST(op))
			{
				*retry = true;
				goto bad;
			}
			*offp = low;
			dp->first_c = hi_c;
			return buf;
		}
		if (low <= P_FIRSTDATAKEY(op) || low > PageGetMaxOffsetNumber(page) + 1)
			goto bad;
		{
			IndexTuple	it = (IndexTuple) PageGetItem(page, PageGetItemId(page, OffsetNumberPrev(low)));

			blk = BTreeTupleGetDownLink(it);
		}
		eqc = Min(lo_eq, hi_eq);
		dp_unlock(dp, lv, buf);
		combo_probe_hook(0);
		lv--;
		if (blk == P_NONE || blk == BTREE_METAPAGE)
		{
			buf = InvalidBuffer;
			goto bad_nobuf;
		}
	}
bad:
	dp_unlock(dp, lv, buf);
bad_nobuf:
	return InvalidBuffer;
}

/* the child's filter and projections on the heap tuple in dp->scan, the row into the combo's map */
/* ioss: a copy of the matching entry's index tuple (the key part of a posting tuple), kept until the page's tuples are done */
static IndexTuple
dp_keep(DProbe *dp, IndexTuple itup)
{
	Size		sz = BTreeTupleIsPosting(itup) ? BTreeTupleGetPostingOffset(itup) : IndexTupleSize(itup);
	IndexTuple	cp;

	if (dp->alen + MAXALIGN(sz) > dp->acap)
	{
		dp->acap = Max(Max(dp->acap * 2, 4096), (Size) MAXALIGN(sz));
		dp->abuf = MemoryContextAlloc(dp->bcxt, dp->acap);
		dp->alen = 0;
	}
	cp = (IndexTuple) (dp->abuf + dp->alen);
	memcpy(cp, itup, sz);
	if (BTreeTupleIsPosting(itup))
		cp->t_info = (cp->t_info & ~INDEX_SIZE_MASK) | sz;
	dp->alen += MAXALIGN(sz);
	return cp;
}

static void
dp_load(DProbe *dp, IndexTuple itup)
{
	TupleTableSlot *slot = dp->scan;

	ExecClearTuple(slot);
	index_deform_tuple(itup, RelationGetDescr(dp->irel), slot->tts_values, slot->tts_isnull);
	ExecStoreVirtualTuple(slot);
}

/* the blocks of the relation this backend has read from the OS so far (-1: the statistics are off) */
int64
dp_reads(Relation rel)
{
	if (rel->pgstat_info == NULL)
		return -1;
	return rel->pgstat_info->counts.blocks_fetched - rel->pgstat_info->counts.blocks_hit;
}

/*
 * The entries of a key are on a page of the index, the heap tuples they point to at random places of the table. When the probes of the statement read their
 * heap pages from the OS (a handful did), the pages of the entries just found are asked for together, so that the device works on them while the first is
 * read: the blocks asked are those the fetches below read, at most the entries of the keys probed. A table that is in memory is never asked.
 */
#define DP_PREFETCH_READS	8

static void
dp_prefetch(DProbe *dp, int nt)
{
	BlockNumber last = InvalidBlockNumber;

	if (effective_io_concurrency == 0 || (!sift_child_warm_test && dp_reads(dp->hrel) - dp->reads0 < DP_PREFETCH_READS))
		return;
	for (int t = 0; t < nt; t++)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&dp->tids[t]);

		if (blk == last || (dp->ioss && VM_ALL_VISIBLE(dp->hrel, blk, &dp->vmbuf)))
			continue;
		last = blk;
		(void) PrefetchBuffer(dp->hrel, MAIN_FORKNUM, blk);
		dp->asked++;
#ifdef SIFT_TESTING
		sift_test_warm_calls++;
#endif
	}
}

static void
dp_walk(DProbe *dp, DKey *k, Buffer buf, OffsetNumber off, DSink *sink)
{
	bool		end = false;
	OffsetNumber first_off = off;
	int			first_c = dp->first_c;	/* the descent's compare of the entry it ended at */

	k->state = DKS_ROWS;
	sink->begin(sink, k);
	k->n = 0;
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		BTPageOpaque op = BTPageGetOpaque(page);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		BlockNumber next = op->btpo_next;
		bool		rightmost = P_RIGHTMOST(op);
		int			nt = 0;
		Buffer		held = InvalidBuffer;

		dp->alen = 0;
		for (; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			IndexTuple	itup;
			int			c,
						np;

			if (dp->ignore_dead && ItemIdIsDead(iid))
				continue;
			itup = (IndexTuple) PageGetItem(page, iid);
			if (off == first_off && first_c != DP_NOCMP)
			{
				c = first_c;
				first_c = DP_NOCMP;
			}
			else
			{
				int			eq = 0;

				c = dp_cmp_tuple(dp, k, itup, BTreeTupleGetNAtts(itup, dp->irel), &eq);
			}
			if (c < 0)
			{
				end = true;
				break;
			}
			if (c > 0)
				continue;
			np = BTreeTupleIsPosting(itup) ? (int) BTreeTupleGetNPosting(itup) : 1;
			if (nt + np > dp->captids)
			{
				dp->captids = Max(dp->captids * 2, nt + np + 64);
				dp->tids = dp->tids ? repalloc(dp->tids, dp->captids * sizeof(ItemPointerData)) :
					MemoryContextAlloc(dp->bcxt, dp->captids * sizeof(ItemPointerData));
				if (dp->ioss)
					dp->tup = dp->tup ? repalloc(dp->tup, dp->captids * sizeof(IndexTuple)) : MemoryContextAlloc(dp->bcxt, dp->captids * sizeof(IndexTuple));
			}
			if (dp->ioss)
			{
				IndexTuple	kept = dp_keep(dp, itup);

				for (int p = 0; p < np; p++)
					dp->tup[nt + p] = kept;
			}
			if (BTreeTupleIsPosting(itup))
			{
				for (int p = 0; p < np; p++)
					dp->tids[nt++] = *BTreeTupleGetPostingN(itup, p);
			}
			else
				dp->tids[nt++] = itup->t_tid;
		}
		/* the entries to the right are not less than the high key: a key below it has no entry there, and that page is not read (as the index AM's scan does) */
		if (!end && !rightmost)
		{
			int			eq = 0;

			if (dp_cmp_hikey(dp, k, page, &eq) < 0)
				end = true;
		}
		/*
		 * An index-only scan keeps the pin of the leaf until the visibility map has been asked for its entries: a VACUUM that removed the entry of a dead tuple and marked
		 * the page all-visible between the read and the check would otherwise let the dead tuple through (it needs a cleanup lock of the leaf).
		 */
		if (dp->ioss)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			held = buf;
		}
		else
			UnlockReleaseBuffer(buf);
		buf = InvalidBuffer;
		combo_probe_hook(1);
		dp_prefetch(dp, nt);
		for (int t = 0; t < nt; t++)
		{
			ItemPointerData tid = dp->tids[t];
			bool		call_again = false;
			bool		all_dead;

			if (dp->ioss)
			{
				dp_load(dp, dp->tup[t]);
				if (sink->keep && !sink->keep(sink))
					continue;
				if (!VM_ALL_VISIBLE(dp->hrel, ItemPointerGetBlockNumber(&tid), &dp->vmbuf))
				{
					if (!table_index_fetch_tuple(dp->fetch, &tid, dp->snap, dp->tslot, &call_again, &all_dead))
						continue;
					SIFT_INVARIANT(!call_again, "an index-only probe found a heap fetch that needs another call");
				}
				if (sink->row(sink))
				{
					k->n++;
					if (dp->limit == 1)
					{
						ReleaseBuffer(held);
						return;
					}
				}
				continue;
			}
			do
			{
				if (table_index_fetch_tuple(dp->fetch, &tid, dp->snap, dp->scan, &call_again, &all_dead) && sink->row(sink))
				{
					k->n++;
					if (dp->limit == 1)
						return;
				}
			} while (call_again);
		}
		if (BufferIsValid(held))
			ReleaseBuffer(held);
		if (end || rightmost || next == P_NONE)
			return;
		combo_probe_hook(2);
		for (;;)
		{
			DP_COUNT(page_reads, 1);
			buf = ReadBuffer(dp->irel, next);
			LockBuffer(buf, BT_READ);
			page = BufferGetPage(buf);
			if (!dp_page_ok(page))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
				break;
			}
			op = BTPageGetOpaque(page);
			if (!P_IGNORE(op))
				break;
			next = op->btpo_next;
			UnlockReleaseBuffer(buf);
			buf = InvalidBuffer;
			if (next == P_NONE)
				break;
		}
		if (buf == InvalidBuffer)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("index \"%s\" ends in a page that cannot be read", RelationGetRelationName(dp->irel))));
		if (!P_ISLEAF(op))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("index \"%s\" has a non-leaf page to the right of a leaf page", RelationGetRelationName(dp->irel))));
		off = P_FIRSTDATAKEY(op);
		first_c = DP_NOCMP;
	}
}

static void
dp_probe(DProbe *dp, DKey *k, DSink *sink)
{
	Buffer		buf = InvalidBuffer;
	OffsetNumber off = InvalidOffsetNumber;
	bool		retry = false;

	pgstat_count_index_scan(dp->irel);
	if (dp->path_ok)
		buf = dp_descend(dp, k, true, &off, &retry);
	if (buf == InvalidBuffer)
	{
		dp->path_ok = true;
		buf = dp_descend(dp, k, false, &off, &retry);
	}
	if (buf == InvalidBuffer)
	{
		dp->path_ok = false;
		k->state = DKS_FALLBACK;
		return;
	}
	dp_walk(dp, k, buf, off, sink);
}

/*
 * Probes the n keys of ord (each DKS_TODO, its arguments and dp_abbrev set) in index order and gives the visible heap tuples of each to the sink. The keys end DKS_ROWS
 * (n matches counted by the sink) or, where the descent cannot serve them, DKS_FALLBACK. Returns the number of keys probed.
 */
int
dp_batch(DProbe *dp, DKey **ord, int n, DSink *sink)
{
	int			level;

	if (n > 1)
		dp_sort_keys(ord, n);
	if (!dp_root(dp, &level))
	{
		for (int p = 0; p < n; p++)
			ord[p]->state = DKS_FALLBACK;
		return 0;
	}
	if (dp->root == P_NONE)
	{
		for (int p = 0; p < n; p++)
			ord[p]->state = DKS_ROWS;
		return 0;
	}
	dp->nlv = level + 1;
	dp->path_ok = false;
	for (int p = 0; p < n; p++)
	{
		dp_probe(dp, ord[p], sink);
		CHECK_FOR_INTERRUPTS();
	}
	DP_COUNT(probes, n);
	dp_unpin(dp);
	if (BufferIsValid(dp->vmbuf))
	{
		ReleaseBuffer(dp->vmbuf);
		dp->vmbuf = InvalidBuffer;
	}
	return n;
}
