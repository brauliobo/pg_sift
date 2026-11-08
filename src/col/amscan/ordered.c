/*
 * col/amscan/ordered.c - an index-only scan in the order of the first key column
 * The planner offers it (ordpath.c) for a query that reads that column alone and wants it sorted (ORDER BY, a merge join): the entries of the global order of the
 * column over the segments and the write buffers, merged (agg/gord.h), are the rows in order, where the units give them in the order of the heap and a Sort follows.
 * The plan says it wants the order: the node of its path is marked (COL_ORDERED_MARK), and sift_col_ioss_want hands that to the scan.
 *
 * A pair (COL_ORDERED_PAIR_MARK) is the order of the first two columns, g and k: the second order of k grouped by g (a block of every row, exact keys) lists the rows of a group by k, so
 * the groups, ascending (descending: from the last, each group backwards), are read one after the other, each as the order of k with g pinned (agg/gord.h). A group costs its open, so a
 * table of many small groups is priced for it (cost.c); a table whose second order is not there, or compact, has its rows read from the units and sorted here.
 */
#include "postgres.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/rel.h"
#include "col/am.h"
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
	GOrdPair   *pairs;			/* the rows with a key, ascending by (key, address) */
	uint32		npairs;
	uint64	   *nulls;			/* the addresses of the rows without one */
	uint32		nnulls;
	int			run;			/* the runs of the sequence given: the rows with a key and those without (NULL rows first for desc) */
	bool		nullrun;
	uint32		left;			/* the rows of the run not yet given, the next at idx (step 1 or -1) */
	int64		idx;
	int			step;
	IndexTuple	ktup;			/* the index tuple of a row with a key, whose bytes at koff are replaced */
	IndexTuple	ntup;			/* ... of a row without */
	Size		koff;
	Oid			type;
	bool		ahead;			/* the entry after a dead one was read to see whether another source repeats it: it is given next */
	int64		a_key;
	bool		a_null;
	ItemPointerData a_tid;
	int			a_st;
	struct OrdMark *mark;		/* the rows given since the last mark, which a restore gives again (a merge join marks at the start of a group of equal keys: the group is all it keeps) */
	uint32		nmark;
	uint32		cmark;
	uint32		replay;			/* the next of them to give again; nmark: the scan goes on */
	bool		marked;
	bool		pair;			/* the order of (g, k): the type above is k's, g is attribute 1 */
	Oid			gtype;
	Size		goff;			/* g in the tuple of a row with a key, and in the one without */
	Size		goffn;
	int			gleaf;
	int			kleaf;
	int64		gcur;			/* the group being read */
	int64	   *groups;			/* the values of g in the order of the scan; NULL: the rows were sorted here (rows) */
	uint32		ngroups;
	uint32		gi;
	uint32		grows;			/* the rows to expect from a group */
	MemoryContext gcxt[2];		/* the merge of the group being read and of the one before: the next one starts from the chunks the last decoded, then the older context is reset */
	int			gsel;
	struct OrdRow *rows;
	uint32		nrows;
	bool		bad;
} ColOrd;

typedef struct OrdRow
{
	int64		g;
	int64		k;
	uint64		tid;
	bool		knull;
} OrdRow;

typedef struct OrdMark
{
	int64		g;
	int64		key;
	bool		isnull;
	ItemPointerData tid;
} OrdMark;

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

/*
 * The tuples of the scan: attribute 1 with a value, and none; every other attribute NULL (the plan reads the first alone). A pair has g in the first and k in the second, which is
 * NULL in the tuple of a row without a key. The place of the values is checked against index_form_tuple.
 */
static void
ord_tuples(IndexScanDesc scan, ColOrd *o)
{
	TupleDesc	desc = scan->xs_itupdesc;
	Form_pg_attribute att = TupleDescAttr(desc, o->pair ? 1 : 0);
	Datum		vals[INDEX_MAX_KEYS] = {0};
	bool		nulls[INDEX_MAX_KEYS];
	int64		probe = INT64CONST(0x0102030405060708),
				gprobe = INT64CONST(0x1112131415161718);
	IndexTuple	chk;
	Datum		d;

	memset(nulls, true, sizeof(nulls));
	o->type = att->atttypid;
	d = o->type == INT2OID ? Int16GetDatum((int16) probe) : o->type == INT4OID || o->type == DATEOID ? Int32GetDatum((int32) probe) : Int64GetDatum(probe);
	if (o->pair)
	{
		Form_pg_attribute gatt = TupleDescAttr(desc, 0);

		o->gtype = gatt->atttypid;
		nulls[0] = false;
		vals[0] = o->gtype == INT2OID ? Int16GetDatum((int16) gprobe) : o->gtype == INT4OID || o->gtype == DATEOID ? Int32GetDatum((int32) gprobe) : Int64GetDatum(gprobe);
		o->ntup = index_form_tuple(desc, vals, nulls);
		o->goffn = IndexInfoFindDataOffset(o->ntup->t_info);
		nulls[1] = false;
		vals[1] = d;
		o->ktup = index_form_tuple(desc, vals, nulls);
		o->goff = IndexInfoFindDataOffset(o->ktup->t_info);
		o->koff = att_align_nominal(o->goff + gatt->attlen, att->attalign);
		chk = palloc(IndexTupleSize(o->ktup));
		memcpy(chk, o->ktup, IndexTupleSize(o->ktup));
		memset((char *) chk + o->goff, 0, o->koff - o->goff + att->attlen);
		key_put(o->gtype, (char *) chk + o->goff, gprobe);
		key_put(o->type, (char *) chk + o->koff, probe);
		SIFT_INVARIANT(memcmp(chk, o->ktup, IndexTupleSize(o->ktup)) == 0, "the index tuple of an ordered pair scan has its values elsewhere");
		pfree(chk);
		return;
	}
	o->ntup = index_form_tuple(desc, vals, nulls);
	nulls[0] = false;
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

static void ord_pair_open(IndexScanDesc scan, ColScanOpaque *so);

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
	o->pair = so->ordpair;
	old = MemoryContextSwitchTo(o->cxt);
	ord_tuples(scan, o);
	o->src = sift_colsrc_open(so->heap, scan->indexRelation, scan->xs_snapshot, o->cxt);
	if (so->first == NULL)
	{
		int			n = sift_col_rel(scan->indexRelation)->natts + 1;

		so->first = palloc(n * sizeof(int));
		memcpy(so->first, sift_col_rel(scan->indexRelation)->first, n * sizeof(int));
	}
	so->ord = o;
	if (o->pair)
	{
		o->gleaf = so->first[1];
		o->kleaf = so->first[2];
		ord_pair_open(scan, so);
		MemoryContextSwitchTo(old);
		return;
	}
	/* a descending scan has the NULL rows first, as the b-tree's default does */
	o->g = sift_gord_open(o->src, so->first[1], ORD_ROWS, o->desc, o->desc, 0, NULL, o->cxt);
	/* the order of a segment that keeps its ends only, or its keys rounded, is not read whole: the units give the rows faster than the heap the keys */
	if (o->g != NULL && (sift_gord_ends(o->g) || sift_gord_compact(o->g)))
		o->g = NULL;
	MemoryContextSwitchTo(old);
}

/* The key of the row at tid for the snapshot and whether it has one, read from the heap; false if no version is visible (a pair: or the group of that version is not the one being read) */
static bool
ord_heap_key(IndexScanDesc scan, ColScanOpaque *so, ItemPointer tid, int64 *key, bool *isnull)
{
	bool		call_again = false,
				all_dead = false;
	Datum		v;
	ColOrd	   *o = so->ord;
	AttrNumber *atts = scan->indexRelation->rd_index->indkey.values;

	if (so->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(so->cxt);

		so->fetch = sift_table_index_fetch_begin(so->heap);
		so->slot = table_slot_create(so->heap, NULL);
		MemoryContextSwitchTo(old);
	}
	if (!table_index_fetch_tuple(so->fetch, tid, scan->xs_snapshot, so->slot, &call_again, &all_dead))
		return false;
	if (o->pair)
	{
		bool		gnull;

		v = slot_getattr(so->slot, atts[0], &gnull);
		if (gnull || key_of(o->gtype, v) != o->gcur)
			return false;
	}
	v = slot_getattr(so->slot, atts[o->pair ? 1 : 0], isnull);
	*key = *isnull ? 0 : key_of(o->type, v);
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
	{
		uint32		n;
		const ItemPointerData *tids;
		const int64 *vals;
		const uint64 *isnull;

		while (sift_col_unordered_batch(scan, &n, &tids, &vals, &isnull, NULL, NULL))
		{
			if (o->npairs + n > cap)
				o->pairs = repalloc_huge(o->pairs, (cap = Max(cap * 2, o->npairs + n)) * sizeof(GOrdPair));
			if (o->nnulls + n > ncap)
				o->nulls = repalloc_huge(o->nulls, (ncap = Max(ncap * 2, o->nnulls + n)) * sizeof(uint64));
			for (uint32 k = 0; k < n; k++)
			{
				uint64		tid = ((uint64) ItemPointerGetBlockNumber(&tids[k]) << 16) | ItemPointerGetOffsetNumber(&tids[k]);

				if (isnull != NULL && (isnull[k >> 6] >> (k & 63) & 1))
					o->nulls[o->nnulls++] = tid;
				else
					o->pairs[o->npairs++] = (GOrdPair) {vals[k], tid};
			}
		}
	}
	gord_pairs_sort(o->pairs, o->npairs);
	MemoryContextSwitchTo(old);
}

static int
ord_row_cmp(const void *a, const void *b)
{
	const OrdRow *x = a,
			   *y = b;

	if (x->g != y->g)
		return x->g < y->g ? -1 : 1;
	if (x->knull != y->knull)
		return x->knull ? 1 : -1;
	if (x->k != y->k)
		return x->k < y->k ? -1 : 1;
	return x->tid < y->tid ? -1 : x->tid > y->tid;
}

/* The rows are read from the units, in the order of the heap, and sorted by (g, k) here: the pair has no order to read */
static void
ord_pair_sort(IndexScanDesc scan, ColOrd *o)
{
	uint32		cap = 1024,
				n;
	const ItemPointerData *tids;
	const int64 *vals,
			   *vals2;
	const uint64 *isnull,
			   *isnull2;

	o->rows = palloc(cap * sizeof(OrdRow));
	while (sift_col_unordered_batch(scan, &n, &tids, &vals, &isnull, &vals2, &isnull2))
	{
		if (o->nrows + n > cap)
			o->rows = repalloc_huge(o->rows, (cap = Max(cap * 2, o->nrows + n)) * sizeof(OrdRow));
		for (uint32 r = 0; r < n; r++)
			o->rows[o->nrows++] = (OrdRow) {vals[r], vals2[r], ((uint64) ItemPointerGetBlockNumber(&tids[r]) << 16) | ItemPointerGetOffsetNumber(&tids[r]),
											isnull2 != NULL && (isnull2[r >> 6] >> (r & 63) & 1)};
	}
	qsort(o->rows, o->nrows, sizeof(OrdRow), ord_row_cmp);
	o->left = o->nrows;
	o->step = o->desc ? -1 : 1;
	o->idx = o->desc ? (int64) o->nrows - 1 : 0;
}

/*
 * The scan of the next group: the order of k with g pinned to its value (the groups ascending, descending for a backward scan). False after the last. A group whose order is not whole
 * (the ends of it, or compact chunks) sets bad: which the segments decide, so it is found at the first group.
 */
static bool
ord_next_group(ColOrd *o)
{
	SiftGBound	b = {0};
	SiftGOrd   *prev = o->g;

	o->g = NULL;
	o->ahead = false;
	if (o->desc ? o->gi == 0 : o->gi >= o->ngroups)
	{
		MemoryContextReset(o->gcxt[o->gsel]);
		return false;
	}
	o->gcur = o->groups[o->desc ? --o->gi : o->gi++];
	b.npin = 1;
	b.pinleaf[0] = o->gleaf;
	b.pinval[0] = o->gcur;
	o->gsel ^= 1;
	o->g = sift_gord_open_from(prev, o->src, o->kleaf, ORD_ROWS, o->desc, o->desc, 0, &b, o->gcxt[o->gsel]);
	MemoryContextReset(o->gcxt[o->gsel ^ 1]);
	if (o->g == NULL || sift_gord_group(o->g) != o->gleaf || sift_gord_ends(o->g) || sift_gord_compact(o->g))
	{
		o->bad = true;
		o->g = NULL;
		return false;
	}
	return true;
}

static void
ord_pair_open(IndexScanDesc scan, ColScanOpaque *so)
{
	ColOrd	   *o = so->ord;
	int64		only = 0;
	const int64 *onlyp = NULL;

	o->gcxt[0] = AllocSetContextCreate(o->cxt, "sift_col ordered groups", ALLOCSET_DEFAULT_SIZES);
	o->gcxt[1] = AllocSetContextCreate(o->cxt, "sift_col ordered groups", ALLOCSET_DEFAULT_SIZES);
	/* g = value (a scan key of the planner's, ordpath.c): the one group, none if the value is NULL */
	if (scan->numberOfKeys > 0)
	{
		const ScanKeyData *sk = &scan->keyData[0];

		SIFT_INVARIANT(scan->numberOfKeys == 1 && sk->sk_attno == 1 && sk->sk_strategy == BTEqualStrategyNumber && !(sk->sk_flags & ~SK_ISNULL), "an ordered pair scan with a condition that is not g = value");
		if (!(sk->sk_flags & SK_ISNULL))
		{
			only = key_of(o->gtype, sk->sk_argument);
			onlyp = &only;
		}
	}
	if (scan->numberOfKeys > 0 && onlyp == NULL)
		o->groups = palloc(sizeof(int64));
	else if (sift_colsrc_gorder_group_values(o->src, o->gleaf, o->kleaf, onlyp, &o->groups, &o->ngroups))
	{
		o->gi = o->desc ? o->ngroups : 0;
		ord_next_group(o);
		if (!o->bad)
			return;
		pfree(o->groups);
		o->groups = NULL;
		o->bad = false;
	}
	ord_pair_sort(scan, o);
}

static void
ord_fill(IndexScanDesc scan, ColOrd *o, int64 g, int64 key, bool isnull, ItemPointer tid)
{
	IndexTuple	t = isnull ? o->ntup : o->ktup;

	if (!isnull)
		key_put(o->type, (char *) t + o->koff, key);
	if (o->pair)
		key_put(o->gtype, (char *) t + (isnull ? o->goffn : o->goff), g);
	t->t_tid = *tid;
	scan->xs_heaptid = *tid;
	scan->xs_itup = t;
	scan->xs_recheck = false;
}

/* The row given after a mark is kept for a restore */
static pg_noinline void
ord_remember(ColOrd *o, int64 key, bool isnull, ItemPointer tid)
{
	OrdMark    *m;

	if (o->nmark >= o->cmark)
	{
		MemoryContext old = MemoryContextSwitchTo(o->cxt);

		o->cmark = Max(64, o->cmark * 2);
		SIFT_REGROW_HUGE_IN(CurrentMemoryContext, o->mark, o->cmark * sizeof(OrdMark));
		MemoryContextSwitchTo(old);
	}
	m = &o->mark[o->nmark++];
	m->g = o->gcur;
	m->key = key;
	m->isnull = isnull;
	m->tid = *tid;
	o->replay = o->nmark;
}

static pg_attribute_always_inline void
ord_give(IndexScanDesc scan, ColOrd *o, int64 key, bool isnull, ItemPointer tid)
{
	if (unlikely(o->marked))
		ord_remember(o, key, isnull, tid);
	ord_fill(scan, o, o->gcur, key, isnull, tid);
	sift_col_ioss_sample(scan, scan->opaque, tid);
}

/* The rows after the mark are kept from now on; those a restore has given again and the scan has passed are not needed any more */
void
sift_col_ordered_mark(ColScanOpaque *so)
{
	ColOrd	   *o = so->ord;

	Assert(o != NULL);
	if (o->replay > 0)
		memmove(o->mark, o->mark + o->replay, (o->nmark - o->replay) * sizeof(OrdMark));
	o->nmark -= o->replay;
	o->replay = 0;
	o->marked = true;
}

void
sift_col_ordered_restore(ColScanOpaque *so)
{
	so->ord->replay = 0;
}

bool
sift_col_ordered_next(IndexScanDesc scan, ScanDirection dir)
{
	ColScanOpaque *so = scan->opaque;
	ColOrd	   *o = so->ord;

	if (unlikely(o != NULL && o->marked) && o->replay < o->nmark)
	{
		OrdMark    *m = &o->mark[o->replay++];

		ord_fill(scan, o, m->g, m->key, m->isnull, &m->tid);
		return true;
	}
	if (o == NULL)
	{
		/* the planner offers the path for a plan that reads the first column alone, with no index condition (ordpath.c) */
		SIFT_INVARIANT((scan->numberOfKeys == 0 || (so->ordpair && scan->numberOfKeys == 1)) && sift_col_ordered_type(TupleDescAttr(scan->xs_itupdesc, 0)->atttypid), "an ordered scan with an index condition or a column it cannot order");
		ord_open(scan, so, dir);
		o = so->ord;
		if (o->g == NULL && !o->pair)
			ord_sort(scan, so);
	}
	if (o->pair && o->groups == NULL)
	{
		const OrdRow *r;
		ItemPointerData tid;

		if (o->left == 0)
			return false;
		o->left--;
		r = &o->rows[o->idx];
		o->idx += o->step;
		o->gcur = r->g;
		ItemPointerSet(&tid, (BlockNumber) (r->tid >> 16), (OffsetNumber) (r->tid & 0xFFFF));
		ord_give(scan, o, r->k, r->knull, &tid);
		return true;
	}
	if (!o->pair && o->g == NULL)
	{
		/* the sequence: the rows with a key then those without, each run reversed for desc, which has the rows without a key first */
		ItemPointerData tid;

		while (o->left == 0)
		{
			int			r = o->run++;

			if (r >= 2)
				return false;
			o->nullrun = (r == 0) == o->desc;
			o->left = o->nullrun ? o->nnulls : o->npairs;
			o->step = o->desc ? -1 : 1;
			o->idx = o->desc ? (int64) o->left - 1 : 0;
		}
		o->left--;
		if (o->nullrun)
		{
			uint64		t = o->nulls[o->idx];

			o->idx += o->step;
			ItemPointerSet(&tid, (BlockNumber) (t >> 16), (OffsetNumber) (t & 0xFFFF));
			ord_give(scan, o, 0, true, &tid);
		}
		else
		{
			const GOrdPair *p = &o->pairs[o->idx];

			o->idx += o->step;
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
			while (o->g == NULL || !sift_gord_next(o->g, &key, &isnull, &tid))
			{
				if (!o->pair || !ord_next_group(o))
				{
					SIFT_INVARIANT(!o->bad, "a group of an ordered pair scan has no whole order, though the first had");
					return false;
				}
			}
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
	{
		SiftSrcCounters cc;

		sift_colsrc_counters(so->ord->src, &cc);
		so->dirty_rows += cc.dirty_rows;
		sift_colsrc_close(so->ord->src);
	}
	MemoryContextDelete(so->ord->cxt);
	so->ord = NULL;
}
