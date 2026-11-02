/*
 * col/amscan/ordered.c - an index-only scan in the order of the first key column
 * The planner offers it (ordpath.c) for a query that reads that column alone and wants it sorted (ORDER BY, a merge join): the entries of the global order of the
 * column over the segments and the write buffers, merged (agg/gord.h), are the rows in order, where the units give them in the order of the heap and a Sort follows.
 * The plan says it wants the order: the node of its path is marked (COL_ORDERED_MARK), and sift_col_ioss_want hands that to the scan.
 */
#include "postgres.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/rel.h"
#include "col/am.h"
#include "agg/colsrc.h"
#include "agg/gord.h"
#include "col/amscan/amscan_int.h"
#include "col/scan/gmem.h"

/* the rows of 2^28 that the scan will ask the blocks of the order for: more than a block of the ends only answers */
#define ORD_ROWS	(1 << 28)

typedef struct ColOrd
{
	MemoryContext cxt;
	SiftSrcSource *src;
	SiftGOrd   *g;				/* the merge of the order; NULL: the order is not whole, the rows were sorted here (pairs) */
	bool		desc;
	bool		sorted;			/* the pairs are read and sorted */
	GOrdPair   *pairs;			/* the rows with a key, ascending by (key, address) */
	uint32		npairs;
	uint64	   *nulls;			/* the addresses of the rows without one */
	uint32		nnulls;
	uint32		at;				/* the place in the sequence of the scan: NULL rows first for desc, else last */
	IndexTuple	ktup;			/* the index tuple of a row with a key, whose bytes at koff are replaced */
	IndexTuple	ntup;			/* ... of a row without */
	Size		koff;
	Oid			type;
	bool		ahead;			/* the entry after a dead one was read to see whether another source repeats it: it is given next */
	int64		a_key;
	bool		a_null;
	ItemPointerData a_tid;
	int			a_st;
} ColOrd;

static int64
key_of(Oid type, Datum d)
{
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

static void
key_put(Oid type, char *dst, int64 key)
{
	switch (type)
	{
		case INT2OID:
			{
				int16		x = (int16) key;

				memcpy(dst, &x, sizeof(x));
				break;
			}
		case INT4OID:
		case DATEOID:
			{
				int32		x = (int32) key;

				memcpy(dst, &x, sizeof(x));
				break;
			}
		default:
			memcpy(dst, &key, sizeof(key));
	}
}

bool
sift_col_ordered_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID || type == DATEOID || type == TIMESTAMPOID || type == TIMESTAMPTZOID;
}

/* The tuples of the scan: attribute 1 with a value, and none; every other attribute NULL (the plan reads the first alone). The place of the value is checked against index_form_tuple. */
static void
ord_tuples(IndexScanDesc scan, ColOrd *o)
{
	TupleDesc	desc = scan->xs_itupdesc;
	Form_pg_attribute att = TupleDescAttr(desc, 0);
	Datum		vals[INDEX_MAX_KEYS] = {0};
	bool		nulls[INDEX_MAX_KEYS];
	int64		probe = INT64CONST(0x0102030405060708);
	IndexTuple	chk;
	Datum		d;

	memset(nulls, true, sizeof(nulls));
	o->type = att->atttypid;
	o->ntup = index_form_tuple(desc, vals, nulls);
	nulls[0] = false;
	d = o->type == INT2OID ? Int16GetDatum((int16) probe) : o->type == INT4OID || o->type == DATEOID ? Int32GetDatum((int32) probe) : Int64GetDatum(probe);
	vals[0] = d;
	o->ktup = index_form_tuple(desc, vals, nulls);
	o->koff = IndexInfoFindDataOffset(o->ktup->t_info);
	chk = palloc(IndexTupleSize(o->ktup));
	memcpy(chk, o->ktup, IndexTupleSize(o->ktup));
	memset((char *) chk + o->koff, 0, att->attlen);
	key_put(o->type, (char *) chk + o->koff, probe);
	SIFT_INVARIANT(memcmp(chk, o->ktup, IndexTupleSize(o->ktup)) == 0, "the index tuple of an ordered scan has its first value elsewhere");
	pfree(chk);
}

static void
ord_open(IndexScanDesc scan, ColScanOpaque *so, ScanDirection dir)
{
	ColOrd	   *o;
	MemoryContext old;

	if (so->heap == NULL)
		so->heap = table_open(scan->indexRelation->rd_index->indrelid, AccessShareLock);
	o = MemoryContextAllocZero(so->cxt, sizeof(ColOrd));
	o->cxt = AllocSetContextCreate(so->cxt, "sift_col ordered scan", ALLOCSET_DEFAULT_SIZES);
	o->desc = ScanDirectionIsBackward(dir);
	old = MemoryContextSwitchTo(o->cxt);
	ord_tuples(scan, o);
	o->src = sift_colsrc_open(so->heap, scan->indexRelation, scan->xs_snapshot, o->cxt);
	if (so->first == NULL)
	{
		int			n = sift_col_rel(scan->indexRelation)->natts + 1;

		so->first = palloc(n * sizeof(int));
		memcpy(so->first, sift_col_rel(scan->indexRelation)->first, n * sizeof(int));
	}
	/* a descending scan has the NULL rows first, as the b-tree's default does */
	o->g = sift_gord_open(o->src, so->first[1], ORD_ROWS, o->desc, o->desc, 0, NULL, o->cxt);
	/* the order of a segment that keeps its ends only, or its keys rounded, is not read whole: the units give the rows faster than the heap the keys */
	if (o->g != NULL && (sift_gord_ends(o->g) || sift_gord_compact(o->g)))
		o->g = NULL;
	MemoryContextSwitchTo(old);
	so->ord = o;
}

/* The key of the row at tid for the snapshot and whether it has one, read from the heap; false if no version is visible */
static bool
ord_heap_key(IndexScanDesc scan, ColScanOpaque *so, ItemPointer tid, int64 *key, bool *isnull)
{
	bool		call_again = false,
				all_dead = false;
	Datum		v;

	if (so->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(so->cxt);

		so->fetch = sift_table_index_fetch_begin(so->heap);
		so->slot = table_slot_create(so->heap, NULL);
		MemoryContextSwitchTo(old);
	}
	if (!table_index_fetch_tuple(so->fetch, tid, scan->xs_snapshot, so->slot, &call_again, &all_dead))
		return false;
	v = slot_getattr(so->slot, scan->indexRelation->rd_index->indkey.values[0], isnull);
	*key = *isnull ? 0 : key_of(so->ord->type, v);
	return true;
}

/* The order is not whole in some segment: the rows are read from the units, in the order of the heap, and sorted here */
static void
ord_sort(IndexScanDesc scan, ColScanOpaque *so)
{
	ColOrd	   *o = so->ord;
	uint32		cap = 1024,
				ncap = 1024;
	MemoryContext old = MemoryContextSwitchTo(o->cxt);

	o->pairs = palloc(cap * sizeof(GOrdPair));
	o->nulls = palloc(ncap * sizeof(uint64));
	while (sift_col_unordered_next(scan))
	{
		bool		isnull;
		Datum		d = index_getattr(scan->xs_itup, 1, scan->xs_itupdesc, &isnull);
		uint64		tid = ((uint64) ItemPointerGetBlockNumber(&scan->xs_heaptid) << 16) | ItemPointerGetOffsetNumber(&scan->xs_heaptid);

		if (isnull)
		{
			if (o->nnulls == ncap)
				o->nulls = repalloc(o->nulls, (ncap *= 2) * sizeof(uint64));
			o->nulls[o->nnulls++] = tid;
		}
		else
		{
			if (o->npairs == cap)
				o->pairs = repalloc(o->pairs, (cap *= 2) * sizeof(GOrdPair));
			o->pairs[o->npairs++] = (GOrdPair) {key_of(o->type, d), tid};
		}
	}
	gord_pairs_sort(o->pairs, o->npairs);
	o->sorted = true;
	MemoryContextSwitchTo(old);
}

static void
ord_give(IndexScanDesc scan, ColOrd *o, int64 key, bool isnull, ItemPointer tid)
{
	IndexTuple	t = isnull ? o->ntup : o->ktup;

	if (!isnull)
		key_put(o->type, (char *) t + o->koff, key);
	t->t_tid = *tid;
	scan->xs_heaptid = *tid;
	sift_col_ioss_sample(scan, scan->opaque, tid);
	scan->xs_itup = t;
	scan->xs_recheck = false;
}

bool
sift_col_ordered_next(IndexScanDesc scan, ScanDirection dir)
{
	ColScanOpaque *so = scan->opaque;
	ColOrd	   *o = so->ord;

	if (o == NULL)
	{
		/* the planner offers the path for a plan that reads the first column alone, with no index condition (ordpath.c) */
		SIFT_INVARIANT(scan->numberOfKeys == 0 && sift_col_ordered_type(TupleDescAttr(scan->xs_itupdesc, 0)->atttypid), "an ordered scan with an index condition or a column it cannot order");
		ord_open(scan, so, dir);
		o = so->ord;
		if (o->g == NULL)
			ord_sort(scan, so);
	}
	if (o->g == NULL)
	{
		/* the sequence: the rows with a key then those without, reversed for desc */
		uint32		total = o->npairs + o->nnulls,
					i;
		ItemPointerData tid;
		bool		nulls_run;

		if (o->at >= total)
			return false;
		i = o->at++;
		nulls_run = o->desc ? i < o->nnulls : i >= o->npairs;
		if (nulls_run)
		{
			uint64		t = o->desc ? o->nulls[o->nnulls - 1 - i] : o->nulls[i - o->npairs];

			ItemPointerSet(&tid, (BlockNumber) (t >> 16), (OffsetNumber) (t & 0xFFFF));
			ord_give(scan, o, 0, true, &tid);
		}
		else
		{
			const GOrdPair *p = o->desc ? &o->pairs[o->npairs - 1 - (i - o->nnulls)] : &o->pairs[i];

			ItemPointerSet(&tid, (BlockNumber) (p->tid >> 16), (OffsetNumber) (p->tid & 0xFFFF));
			ord_give(scan, o, p->key, false, &tid);
		}
		return true;
	}
	for (;;)
	{
		int64		key;
		bool		isnull;
		ItemPointerData tid;
		int			st;

		if (o->ahead)
		{
			key = o->a_key;
			isnull = o->a_null;
			tid = o->a_tid;
			st = o->a_st;
			o->ahead = false;
		}
		else
		{
			if (!sift_gord_next(o->g, &key, &isnull, &tid))
				return false;
			st = sift_gord_state(o->g, &tid);
		}
		/*
		 * A row its segment lost (dead) is an entry of no row, unless another source holds a row at that address with that key, which the merge dropped as the repeat of this
		 * entry: the next read tells. A row the unit cannot vouch for (unknown) has the heap say what it holds now. A row on a page the heap has to say, the executor checks.
		 */
		if (st == SIFT_GORD_DEAD)
		{
			o->ahead = sift_gord_next(o->g, &o->a_key, &o->a_null, &o->a_tid);
			if (o->ahead)
				o->a_st = sift_gord_state(o->g, &o->a_tid);
			if (!sift_gord_repeated(o->g))
				continue;
		}
		else if (st == SIFT_GORD_UNKNOWN)
		{
			int64		hkey;
			bool		hnull;

			if (!ord_heap_key(scan, so, &tid, &hkey, &hnull) || hnull != isnull || (!isnull && hkey != key))
				continue;
		}
		ord_give(scan, o, key, isnull, &tid);
		return true;
	}
}

void
sift_col_ordered_end(ColScanOpaque *so)
{
	if (so->ord == NULL)
		return;
	if (so->ord->src != NULL)
		sift_colsrc_close(so->ord->src);
	MemoryContextDelete(so->ord->cxt);
	so->ord = NULL;
}
