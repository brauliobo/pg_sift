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
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "col/am.h"
#include "agg/gord.h"
#include "col/amscan/amscan_int.h"
#include "col/scan/gmem.h"

/* the rows of 2^28 that the scan will ask the blocks of the order for: more than a block of the ends only answers */
#define ORD_ROWS	(1 << 28)

/* '>' of int8: the descending sort of the rows sorted here (the operator of Int8LessOperator's pair) */
#define ORD_INT8_GT	413

typedef struct ColOrd
{
	MemoryContext cxt;
	SiftSrcSource *src;
	SiftGOrd   *g;				/* the merge of the order; NULL: the order is not whole, the rows were sorted here (pairs) */
	bool		desc;
	GOrdPair   *pairs;			/* the rows with a key, ascending by (key, address) */
	uint32		npairs;
	uint32		pcap;
	uint64	   *nulls;			/* the addresses of the rows without one */
	uint32		nnulls;
	uint32		ncap;
	Tuplesortstate *sort;		/* the rows sorted here once they take more than work_mem: (g, key, address) in the order of the scan (the arrays are then empty) */
	TupleTableSlot *sput;
	TupleTableSlot *sget;
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
	BufFile    *mfile;			/* ... in a temporary file from the entry that takes more than work_mem: the entries are then mbase.. of it, and mark is not used */
	uint64		mbase;
	uint32		replay;			/* the next of them to give again; nmark: the scan goes on */
	bool		marked;
	bool		pair;			/* the order of (g, k): the type above is k's, g is attribute 1 */
	Oid			gtype;
	Size		goff;			/* g in the tuple of a row with a key, and in the one without */
	Size		goffn;
	int			gleaf;
	int			kleaf;
	int64		gcur;			/* the group being read */
	void	   *gcursor;		/* the values of g in the order of the scan; bygroup without one: no group */
	bool		bygroup;		/* the groups are read one after the other; else the rows were sorted here (rows) */
	uint32		grows;			/* the rows to expect from a group */
	MemoryContext gcxt[2];		/* the merge of the group being read and of the one before: the next one starts from the chunks the last decoded, then the older context is reset */
	int			gsel;
	struct OrdRow *rows;
	uint32		nrows;
	uint32		rcap;
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

/* work_mem, and no more than 4 GB: the counts of the arrays are 32 bits */
static Size
ord_budget(void)
{
	return Min((Size) work_mem * 1024, (Size) 4 << 30);
}

static Size
ord_held(const ColOrd *o)
{
	return (Size) o->pcap * sizeof(GOrdPair) + (Size) o->ncap * sizeof(uint64) + (Size) o->rcap * sizeof(OrdRow);
}

/* a tuplesort of (g, key, address) ordered as the scan gives them: g, the keys (NULL last, first for desc) and the addresses, ascending (descending for desc); a plain scan has no g to order by */
static void
ord_sort_begin(ColOrd *o)
{
	TupleDesc	desc = CreateTemplateTupleDesc(3);
	AttrNumber	att[3] = {1, 2, 3};
	Oid			op = o->desc ? ORD_INT8_GT : Int8LessOperator,
				ops[3] = {op, op, op},
				coll[3] = {InvalidOid, InvalidOid, InvalidOid};
	bool		nulls_first[3] = {o->desc, o->desc, o->desc};
	int			skip = o->pair ? 0 : 1;
	MemoryContext old = MemoryContextSwitchTo(o->cxt);

	for (int i = 1; i <= 3; i++)
		TupleDescInitEntry(desc, i, "c", INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	o->sput = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	o->sget = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	o->sort = tuplesort_begin_heap(desc, 3 - skip, att + skip, ops + skip, coll + skip, nulls_first + skip, work_mem, NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(old);
}

static void
ord_sort_put(ColOrd *o, int64 g, int64 k, bool knull, uint64 tid)
{
	TupleTableSlot *slot = o->sput;

	ExecClearTuple(slot);
	slot->tts_values[0] = Int64GetDatum(g);
	slot->tts_values[1] = Int64GetDatum(k);
	slot->tts_values[2] = Int64GetDatum((int64) tid);
	slot->tts_isnull[0] = false;
	slot->tts_isnull[1] = knull;
	slot->tts_isnull[2] = false;
	ExecStoreVirtualTuple(slot);
	tuplesort_puttupleslot(o->sort, slot);
}

/* The rows held in the arrays take work_mem: they move to a tuplesort, which spills to disk, and the rows that follow go to it */
static void
ord_spill(ColOrd *o)
{
	ord_sort_begin(o);
	for (uint32 r = 0; r < o->npairs; r++)
		ord_sort_put(o, 0, o->pairs[r].key, false, o->pairs[r].tid);
	for (uint32 r = 0; r < o->nnulls; r++)
		ord_sort_put(o, 0, 0, true, o->nulls[r]);
	for (uint32 r = 0; r < o->nrows; r++)
		ord_sort_put(o, o->rows[r].g, o->rows[r].k, o->rows[r].knull, o->rows[r].tid);
	if (o->pairs)
		pfree(o->pairs);
	if (o->nulls)
		pfree(o->nulls);
	if (o->rows)
		pfree(o->rows);
	o->pairs = NULL;
	o->nulls = NULL;
	o->rows = NULL;
	o->npairs = o->nnulls = o->nrows = o->pcap = o->ncap = o->rcap = 0;
}

/* Room for one more entry of an array of esize bytes: doubled, or the rows go to the tuplesort when that would pass work_mem (false) */
static bool
ord_room(ColOrd *o, void **arr, uint32 *cap, Size esize)
{
	uint32		ncap = Max(*cap * 2, 1024);

	if (ord_held(o) + (Size) (ncap - *cap) * esize > ord_budget())
	{
		ord_spill(o);
		return false;
	}
	*arr = *arr ? repalloc_huge(*arr, (Size) ncap * esize) : MemoryContextAllocHuge(o->cxt, (Size) ncap * esize);
	*cap = ncap;
	return true;
}

/* One row read from the units: in the arrays, or in the tuplesort once they have taken work_mem */
static void
ord_add(ColOrd *o, int64 g, int64 k, bool knull, uint64 tid)
{
	if (o->sort == NULL)
	{
		if (o->pair && (o->nrows < o->rcap || ord_room(o, (void **) &o->rows, &o->rcap, sizeof(OrdRow))))
		{
			o->rows[o->nrows++] = (OrdRow) {g, k, tid, knull};
			return;
		}
		if (!o->pair && knull && (o->nnulls < o->ncap || ord_room(o, (void **) &o->nulls, &o->ncap, sizeof(uint64))))
		{
			o->nulls[o->nnulls++] = tid;
			return;
		}
		if (!o->pair && !knull && (o->npairs < o->pcap || ord_room(o, (void **) &o->pairs, &o->pcap, sizeof(GOrdPair))))
		{
			o->pairs[o->npairs++] = (GOrdPair) {k, tid};
			return;
		}
	}
	ord_sort_put(o, g, k, knull, tid);
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

/*
 * The order is not whole in some segment (a pair: the second order is not there to read group after group): the rows are read from the units, in the order of the heap, and sorted here,
 * by (key, address), with the rows without a key after them (first for desc); a pair by (g, key, address). Up to work_mem in arrays, then in a tuplesort that spills.
 */
static void
ord_sort(IndexScanDesc scan, ColOrd *o)
{
	const ItemPointerData *tids;
	const int64 *vals,
			   *vals2;
	const uint64 *isnull,
			   *isnull2;
	uint32		n;

	while (sift_col_unordered_batch(scan, &n, &tids, &vals, &isnull, o->pair ? &vals2 : NULL, o->pair ? &isnull2 : NULL))
		for (uint32 r = 0; r < n; r++)
		{
			uint64		tid = ((uint64) ItemPointerGetBlockNumber(&tids[r]) << 16) | ItemPointerGetOffsetNumber(&tids[r]);

			if (o->pair)
				ord_add(o, vals[r], vals2[r], isnull2 != NULL && (isnull2[r >> 6] >> (r & 63) & 1), tid);
			else
				ord_add(o, 0, vals[r], isnull != NULL && (isnull[r >> 6] >> (r & 63) & 1), tid);
		}
	if (o->sort)
		tuplesort_performsort(o->sort);
	else if (o->pair)
	{
		qsort(o->rows, o->nrows, sizeof(OrdRow), ord_row_cmp);
		o->left = o->nrows;
		o->idx = o->desc ? (int64) o->nrows - 1 : 0;
	}
	else
		gord_pairs_sort(o->pairs, o->npairs);
	o->step = o->desc ? -1 : 1;
}

/* The next row of the tuplesort; false after the last */
static bool
ord_sorted_next(ColOrd *o, int64 *g, int64 *k, bool *knull, ItemPointer tid)
{
	TupleTableSlot *slot = o->sget;
	uint64		t;

	if (!tuplesort_gettupleslot(o->sort, true, false, slot, NULL))
		return false;
	slot_getallattrs(slot);
	*g = DatumGetInt64(slot->tts_values[0]);
	*knull = slot->tts_isnull[1];
	*k = *knull ? 0 : DatumGetInt64(slot->tts_values[1]);
	t = (uint64) DatumGetInt64(slot->tts_values[2]);
	ItemPointerSet(tid, (BlockNumber) (t >> 16), (OffsetNumber) (t & 0xFFFF));
	return true;
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
	if (o->gcursor == NULL || !sift_colsrc_gorder_groups_next(o->src, o->gcursor, &o->gcur))
	{
		MemoryContextReset(o->gcxt[o->gsel]);
		return false;
	}
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
	{
		o->bygroup = true;
		return;
	}
	o->gcursor = sift_colsrc_gorder_groups_open(o->src, o->gleaf, o->kleaf, onlyp, o->desc, o->cxt);
	if (o->gcursor != NULL)
	{
		o->bygroup = true;
		ord_next_group(o);
		if (!o->bad)
			return;
		o->gcursor = NULL;
		o->bygroup = false;
		o->bad = false;
	}
	ord_sort(scan, o);
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

/*
 * The row given after a mark is kept for a restore: in an array until the rows kept take work_mem, then in a temporary file (a run of equal keys is as long as the data has it, and a b-tree keeps a position
 * for it). The entries are then in the file from mbase, each OrdMark long.
 */
static pg_noinline void
ord_remember(ColOrd *o, int64 key, bool isnull, ItemPointer tid)
{
	OrdMark		m = {o->gcur, key, isnull, *tid};

	if (o->mfile == NULL && o->nmark >= o->cmark)
	{
		MemoryContext old = MemoryContextSwitchTo(o->cxt);
		uint32		ncap = Max(64, o->cmark * 2);

		if ((Size) ncap * sizeof(OrdMark) > ord_budget())
		{
			o->mfile = BufFileCreateTemp(false);
			o->mbase = 0;
			for (uint32 i = 0; i < o->nmark; i++)
				BufFileWrite(o->mfile, &o->mark[i], sizeof(OrdMark));
			pfree(o->mark);
			o->mark = NULL;
			o->cmark = 0;
		}
		else
		{
			o->cmark = ncap;
			SIFT_REGROW_HUGE_IN(CurrentMemoryContext, o->mark, o->cmark * sizeof(OrdMark));
		}
		MemoryContextSwitchTo(old);
	}
	if (o->mfile)
	{
		BufFileSeek(o->mfile, 0, (off_t) ((o->mbase + o->nmark) * sizeof(OrdMark)), SEEK_SET);
		BufFileWrite(o->mfile, &m, sizeof(m));
		o->nmark++;
	}
	else
		o->mark[o->nmark++] = m;
	o->replay = o->nmark;
}

/* The entry i of the rows kept since the mark */
static OrdMark
ord_kept(ColOrd *o, uint32 i)
{
	OrdMark		m;

	if (o->mfile == NULL)
		return o->mark[i];
	BufFileSeek(o->mfile, 0, (off_t) ((o->mbase + i) * sizeof(OrdMark)), SEEK_SET);
	BufFileReadExact(o->mfile, &m, sizeof(m));
	return m;
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
	if (o->mfile)
		o->mbase = o->nmark == o->replay ? 0 : o->mbase + o->replay;
	else if (o->replay > 0)
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
		OrdMark		m = ord_kept(o, o->replay++);

		ord_fill(scan, o, m.g, m.key, m.isnull, &m.tid);
		return true;
	}
	if (o == NULL)
	{
		/* the planner offers the path for a plan that reads the first column alone, with no index condition (ordpath.c) */
		SIFT_INVARIANT((scan->numberOfKeys == 0 || (so->ordpair && scan->numberOfKeys == 1)) && sift_col_ordered_type(TupleDescAttr(scan->xs_itupdesc, 0)->atttypid), "an ordered scan with an index condition or a column it cannot order");
		ord_open(scan, so, dir);
		o = so->ord;
		if (o->g == NULL && !o->pair)
			ord_sort(scan, o);
	}
	if (o->sort)
	{
		int64		g,
					key;
		bool		isnull;
		ItemPointerData tid;

		if (!ord_sorted_next(o, &g, &key, &isnull, &tid))
			return false;
		o->gcur = g;
		ord_give(scan, o, key, isnull, &tid);
		return true;
	}
	if (o->pair && !o->bygroup)
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
	if (so->ord->sort)
	{
		tuplesort_end(so->ord->sort);
		ExecDropSingleTupleTableSlot(so->ord->sput);
		ExecDropSingleTupleTableSlot(so->ord->sget);
	}
	if (so->ord->mfile)
		BufFileClose(so->ord->mfile);
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
