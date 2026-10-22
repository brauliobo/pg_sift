/*
 * agg/topn.c - ORDER BY <columns> LIMIT n from the column units (agg/topn.h).
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "catalog/pg_opfamily_d.h"
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"

#include "agg/topn.h"
#include "core/cmp.h"
#include "core/compat.h"

/* Superblocks per window of the visit order at most: the zones are looked at again, and the match set is asked for, once per window */
#define TOPN_WINDOW		8
/* Superblocks whose zones tell whether the sort column follows the table */
#define TOPN_PROBES		6

bool		sift_topn_zone_keys = true;

bool
sift_topn_key_ok(const SiftSrcLeaf *lf)
{
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->lossy)
		return false;
	switch (lf->typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return lf->vtype == SCV_I32 || lf->vtype == SCV_I64;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:
			return lf->vtype == SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return lf->vtype == SCV_F64;
		default:
			return false;
	}
}

Node *
sift_topn_spec(int nkey, const int *leaf, const bool *desc, const bool *nulls_first, int nout, const int *attno, int flags, Node *count, Node *offset)
{
	List	   *l = list_make1_int(nkey);

	for (int j = 0; j < nkey; j++)
		l = lappend_int(lappend_int(l, leaf[j]), (desc[j] ? 1 : 0) | (nulls_first[j] ? 2 : 0));
	l = lappend_int(l, nout);
	for (int c = 0; c < nout; c++)
		l = lappend_int(l, attno[c]);
	l = lappend_int(l, flags);
	return (Node *) list_make3(l, count, offset);
}

#define SPEC_KEYS(spec)		((List *) linitial((List *) (spec)))
#define SPEC_COUNT(spec)	((Node *) lsecond((List *) (spec)))
#define SPEC_OFFSET(spec)	((Node *) lthird((List *) (spec)))

/* The rows the scan holds for LIMIT and OFFSET, as EXPLAIN shows them: their sum when they are constants; ", sorted" when the plan sorts every row that passes */
char *
sift_topn_limit_text(Node *spec)
{
	Node	   *count = SPEC_COUNT(spec),
			   *offset = SPEC_OFFSET(spec);
	const char *how = (llast_int(SPEC_KEYS(spec)) & TOPN_SORT) ? ", sorted" : "";
	int64		n;

	if (IsA(count, Const) && (offset == NULL || IsA(offset, Const)) &&
		!pg_add_s64_overflow(DatumGetInt64(((Const *) count)->constvalue), offset ? DatumGetInt64(((Const *) offset)->constvalue) : 0, &n))
		return psprintf(INT64_FORMAT "%s", n, how);
	return psprintf("by parameter%s", how);
}

typedef struct TopKey
{
	int			leaf;
	bool		desc;
	bool		nulls_first;
	SiftSrcVecType vtype;
} TopKey;

/* A key's value as a position in the sort order: NULLs in front of or behind the values, the values by the direction (class -1: no bound) */
typedef struct Rank
{
	int			cls;
	uint64		ord;
} Rank;

typedef struct TopEnt
{
	int64		k[SIFT_TOPN_MAX_KEYS];
	uint64		tie;			/* the TID's key in the result heap, the row in a unit's */
	uint32		row;
	uint8		nul;			/* bit j: key j is NULL */
	ItemPointerData tid;
} TopEnt;

struct SiftTopN
{
	int			limit;			/* rows the heap holds; 0: none wanted (a LIMIT that leaves nothing) */
	bool		all;			/* no bound the heap can hold (NULL, past the cap, more keys): every row that passes is kept, sorted by the keys and the TID at the end */
	int64	   *mem;			/* ... in memory while they fit work_mem: nkey keys, the NULL flags (a bit for each key), the TID's number, for each row */
	int64		nmem;
	int64		capmem;
	int			stride;			/* int64s of a row of mem */
	Tuplesortstate *sort;		/* ... else a tuplesort of them (work_mem, then disk) */
	TupleTableSlot *sin,
			   *sout;			/* the rows going in and out of sort: the keys, then the TID as a number */
	int64		nsort;
	int			pos;			/* the next row of res */
	int			nkey;
	TopKey	   *key;
	int			nout;
	int		   *attno;
	int64		skip;			/* the first rows the Limit above throws away (OFFSET): given as rows of NULLs, not read from the heap */
	int64		emitted;
	TopEnt	   *heap;			/* the best rows so far, the worst at heap[0] */
	int			n;
	TopEnt	   *lh;				/* the best rows of the unit */
	int			ln;
	uint32	   *crow;			/* the rows of the unit that pass, their first sort key and whether it is NULL */
	int64	   *ck;
	uint8	   *cnul;
	int			ccap;
	TopEnt	   *ce;				/* the rows whose first key is not worse than the n-th of the unit's and of the heap */
	uint32	   *cand;			/* the rows of ce */
	uint32	   *rows;
	int64	   *skey;			/* sort: the keys of the rows of a unit, nkey for each row, and whether each is NULL */
	bool	   *snull;
	ItemPointerData *tids;		/* the TIDs of the rows of a unit (sort), or of the heap rows of a unit */
	TopEnt	   *res;			/* after sift_topn_finish: the heap in order */
	/* the visit order */
	uint32		nsb;
	bool		reverse;			/* the superblocks are visited from the last */
	bool		follows;			/* the first sort column follows the superblocks (as far as the probes say): the visit starts at its best end */
	uint32		left;				/* superblocks not yet in a window */
	uint32		size;				/* superblocks of the next window */
	uint32		nprobe;
	struct
	{
		uint32		sb;
		int64		best;
	}			probe[TOPN_PROBES];
	/* the walk (sift_topn_walk_*): the zone and the number of units of every superblock, and the superblocks by their best bound */
	int			wkeys;				/* leading sort keys whose zones bound the walk */
	Rank	   *wbound;			/* wkeys per superblock: the best rank its zone allows for that key */
	uint32	   *wunits;
	uint32	   *word;
	uint32		wpos;
	Snapshot	snap;
	TupleTableSlot *fslot;
	IndexFetchTableData *fetch;	/* the heap row of a TID: the version an index scan finds, along a HOT chain */
	int64		skipped;
	int64		fetched;
	pg_atomic_uint64 *shared;	/* parallel scan: the first-key rank of the n-th row of any participant (sift_topn_share) */
};

/* The value of a LIMIT or OFFSET expression of the plan, evaluated as the Limit node does when it starts */
static int64
eval_bound(Node *e, ExprContext *econtext, PlanState *parent, bool *isnull)
{
	return DatumGetInt64(ExecEvalExprSwitchContext(ExecInitExpr((Expr *) e, parent), econtext, isnull));
}

/*
 * LIMIT + OFFSET, whatever the values the plan was made for: false when no heap can hold them (no limit, past the cap, an overflow).
 * A negative sum is a rows-wanted of zero: the Limit node above raises its error before it asks for a row. *offset: the OFFSET (0 for NULL).
 */
static bool
bound_rows(Node *spec, ExprContext *econtext, PlanState *parent, int *rows, int64 *offset)
{
	bool		isnull,
				offnull = true;
	int64		count = eval_bound(SPEC_COUNT(spec), econtext, parent, &isnull),
				n;

	*offset = SPEC_OFFSET(spec) ? eval_bound(SPEC_OFFSET(spec), econtext, parent, &offnull) : 0;
	if (offnull)
		*offset = 0;
	if (isnull || pg_add_s64_overflow(count, *offset, &n) || n > sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS))
		return false;
	*rows = Max(n, 0);
	return true;
}

/* The rows ordered by the keys (in the direction of each, the NULLs where it says), then by the TID: the order of the heap */
static Tuplesortstate *
sort_begin(SiftTopN *t)
{
	int			n = t->nkey + 1;
	TupleDesc	desc = CreateTemplateTupleDesc(n);
	AttrNumber *att = palloc(n * sizeof(AttrNumber));
	Oid		   *op = palloc(n * sizeof(Oid)),
			   *coll = palloc0(n * sizeof(Oid));
	bool	   *nf = palloc(n * sizeof(bool));

	for (int j = 0; j < n; j++)
	{
		bool		key = j < t->nkey;

		TupleDescInitEntry(desc, j + 1, NULL, INT8OID, -1, 0);
		att[j] = j + 1;
		op[j] = get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, key && t->key[j].desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		nf[j] = key && t->key[j].nulls_first;
	}
	sift_tupdesc_finalize(desc);
	t->sin = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	t->sout = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	return tuplesort_begin_heap(desc, n, att, op, coll, nf, work_mem, NULL, TUPLESORT_RANDOMACCESS);
}

SiftTopN *
sift_topn_open(Node *spec, const SiftSrcLayout *lay, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftTopN   *t = palloc0(sizeof(SiftTopN));
	List	   *l = SPEC_KEYS(spec);
	int			i = 0;
	bool		bounded;
	int64		offset;
	int			flags;

	t->nkey = list_nth_int(l, i++);
	t->key = palloc(t->nkey * sizeof(TopKey));
	for (int j = 0; j < t->nkey; j++)
	{
		int			leaf = list_nth_int(l, i++),
					fl = list_nth_int(l, i++);

		t->key[j] = (TopKey) {leaf, fl & 1, (fl & 2) != 0, lay->leaf[leaf].vtype};
	}
	/* the zones of the next keys tell, for a first key that ties, which superblocks can still hold a row before the n-th: they bound the walk in the order of the keys */
	for (t->wkeys = 1; sift_topn_zone_keys && t->wkeys < t->nkey && (t->key[t->wkeys].vtype == SCV_I32 || t->key[t->wkeys].vtype == SCV_I64); t->wkeys++)
		;
	t->nout = list_nth_int(l, i++);
	t->attno = palloc(Max(t->nout, 1) * sizeof(int));
	for (int c = 0; c < t->nout; c++)
		t->attno[c] = list_nth_int(l, i++);
	bounded = bound_rows(spec, econtext, parent, &t->limit, &offset);
	/* a heap entry holds SIFT_TOPN_MAX_KEYS keys: more keys sort every row, as no bound does */
	flags = list_nth_int(l, i++);
	if ((flags & TOPN_SORT) || (t->nkey > sift_limit(topn_keys, SIFT_TOPN_MAX_KEYS) && t->limit > 0))
		bounded = false;
	if (flags & TOPN_SKIP_OFFSET)
		t->skip = offset;
	t->all = !bounded;
	if (t->all)
	{
		t->stride = t->nkey + (t->nkey + 63) / 64 + 1;
		t->mem = palloc(1024 * t->stride * sizeof(int64));
		t->capmem = 1024;
	}
	else if (t->limit > 0)
	{
		t->heap = palloc(t->limit * sizeof(TopEnt));
		t->lh = palloc(t->limit * sizeof(TopEnt));
		t->rows = palloc(t->limit * sizeof(uint32));
		t->tids = palloc(t->limit * sizeof(ItemPointerData));
	}
	t->snap = snapshot;
	t->fslot = table_slot_create(heap, NULL);
	t->fetch = sift_table_index_fetch_begin(heap);
	MemoryContextSwitchTo(old);
	return t;
}

void
sift_topn_close(SiftTopN *t)
{
	if (t->fslot)
		ExecDropSingleTupleTableSlot(t->fslot);
	if (t->fetch)
		table_index_fetch_end(t->fetch);
	if (t->sort)
	{
		ExecDropSingleTupleTableSlot(t->sin);
		ExecDropSingleTupleTableSlot(t->sout);
		tuplesort_end(t->sort);
	}
}

bool
sift_topn_empty(const SiftTopN *t)
{
	return !t->all && t->limit == 0;
}

int
sift_topn_leaf(const SiftTopN *t, int j)
{
	return t->key[j].leaf;
}

int64
sift_topn_skipped(const SiftTopN *t)
{
	return t->skipped;
}

int64
sift_topn_fetched(const SiftTopN *t)
{
	return t->fetched;
}

/* < 0: a sorts before b by the first nk keys */
static int
key_cmp_n(const SiftTopN *t, const TopEnt *a, const TopEnt *b, int nk)
{
	for (int j = 0; j < nk; j++)
	{
		bool		an = (a->nul >> j) & 1,
					bn = (b->nul >> j) & 1;

		if (an != bn)
			return an == t->key[j].nulls_first ? -1 : 1;
		if (an || a->k[j] == b->k[j])
			continue;
		return (a->k[j] < b->k[j]) != t->key[j].desc ? -1 : 1;
	}
	return 0;
}

static int
key_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	return key_cmp_n(t, a, b, t->nkey);
}

static int
ent_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	int			c = key_cmp(t, a, b);

	return c != 0 ? c : SIFT_CMP3(a->tie, b->tie);
}

static int
ent_cmp_arg(const void *a, const void *b, void *arg)
{
	return ent_cmp(arg, a, b);
}

/* the heap keeps its worst entry at h[0]: a parent sorts after its children */
static void
heap_down(const SiftTopN *t, TopEnt *h, int n, int i)
{
	TopEnt		e = h[i];

	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= n)
			break;
		if (c + 1 < n && ent_cmp(t, &h[c + 1], &h[c]) > 0)
			c++;
		if (ent_cmp(t, &h[c], &e) <= 0)
			break;
		h[i] = h[c];
		i = c;
	}
	h[i] = e;
}

static void
heap_push(const SiftTopN *t, TopEnt *h, int *n, const TopEnt *e)
{
	int			i;

	if (*n == t->limit)
	{
		if (ent_cmp(t, e, &h[0]) >= 0)
			return;
		h[0] = *e;
		heap_down(t, h, *n, 0);
		return;
	}
	i = (*n)++;
	while (i > 0 && ent_cmp(t, e, &h[(i - 1) / 2]) > 0)
	{
		h[i] = h[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	h[i] = *e;
}

/* the key of row i; floats map to integers of the same order (NaN last); a numeric NaN is PG_INT64_MAX in the vector (SiftSrcVec.ovf) */
static inline bool
row_key(const TopKey *k, const SiftSrcVec *v, int i, int64 *out)
{
	if (v->isnull && v->isnull[i])
		return false;
	switch (k->vtype)
	{
		case SCV_I32:
			*out = ((const int32 *) v->data)[i];
			break;
		case SCV_I64:
			*out = ((const int64 *) v->data)[i];
			break;
		default:
			{
				double		d = ((const double *) v->data)[i];
				int64		b;

				if (isnan(d))
					*out = PG_INT64_MAX;
				else
				{
					d = d == 0 ? 0.0 : d;
					memcpy(&b, &d, sizeof(b));
					*out = b < 0 ? b ^ PG_INT64_MAX : b;
				}
			}
	}
	return true;
}

static int
row_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return SIFT_CMP3(x, y);
}

static uint64
tid_key(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static void *
grow(SiftTopN *t, void *p, Size sz)
{
	return p ? repalloc(p, sz) : MemoryContextAlloc(GetMemoryChunkContext(t), sz);
}

static void
grow_unit_buffers(SiftTopN *t, int nrow)
{
	if (t->ccap >= nrow)
		return;
	t->ccap = Max(nrow, 2 * t->ccap);
	t->crow = grow(t, t->crow, t->ccap * sizeof(uint32));
	t->ck = grow(t, t->ck, t->ccap * sizeof(int64));
	t->cnul = grow(t, t->cnul, t->ccap);
	t->cand = grow(t, t->cand, t->ccap * sizeof(uint32));
	t->ce = grow(t, t->ce, t->ccap * sizeof(TopEnt));
	if (t->all)
	{
		t->tids = grow(t, t->tids, t->ccap * sizeof(ItemPointerData));
		t->skey = grow(t, t->skey, (Size) t->ccap * t->nkey * sizeof(int64));
		t->snull = grow(t, t->snull, (Size) t->ccap * t->nkey);
	}
}

/* The rows cand[0..nc) (ascending) with their first key in ce: the other keys, by position */
static void
read_other_keys(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc)
{
	for (int j = 1; j < t->nkey; j++)
	{
		SiftSrcVec	v;

		sift_colsrc_read_rows(src, unit, t->key[j].leaf, t->cand, nc, &v);
		for (int x = 0; x < nc; x++)
			if (!row_key(&t->key[j], &v, x, &t->ce[x].k[j]))
				t->ce[x].nul |= 1 << j;
	}
}

static void share_bar(SiftTopN *t);

/* The best n of those rows by all the keys go to the heap with their TIDs */
static void
finish_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc)
{
	t->ln = 0;
	read_other_keys(t, src, unit, nc);
	for (int x = 0; x < nc; x++)
		heap_push(t, t->lh, &t->ln, &t->ce[x]);
	if (t->ln == 0)
		return;
	for (int i = 0; i < t->ln; i++)
		t->rows[i] = t->lh[i].row;
	qsort(t->rows, t->ln, sizeof(uint32), row_cmp);
	sift_colsrc_tids(src, unit, t->rows, t->ln, t->tids);
	for (int i = 0; i < t->ln; i++)
	{
		TopEnt	   *e = &t->lh[i];
		uint32	   *at = bsearch(&e->row, t->rows, t->ln, sizeof(uint32), row_cmp);

		e->tid = t->tids[at - t->rows];
		e->tie = tid_key(&e->tid);
		heap_push(t, t->heap, &t->n, e);
	}
	share_bar(t);
}

static inline Rank
rank_of(const TopKey *k, bool isnull, int64 v)
{
	Rank		r = {isnull ? (k->nulls_first ? 0 : 2) : 1, 0};

	if (!isnull)
		r.ord = k->desc ? ~((uint64) v ^ UINT64CONST(0x8000000000000000)) : (uint64) v ^ UINT64CONST(0x8000000000000000);
	return r;
}

static inline bool
rank_after(Rank a, Rank b)
{
	return a.cls != b.cls ? a.cls > b.cls : a.ord > b.ord;
}

static inline Rank
ent_rank(const SiftTopN *t, const TopEnt *e)
{
	return rank_of(&t->key[0], e->nul & 1, e->k[0]);
}

/*
 * The rank of the n-th row so far: the heap's worst when it is full, or the bound the participants of a parallel scan share when that sorts
 * before it. A row or a zone strictly after it cannot be in the result, whoever holds the n rows.
 */
static bool
bar_rank(const SiftTopN *t, Rank *bar)
{
	bool		have = t->n == t->limit;

	if (have)
		*bar = ent_rank(t, &t->heap[0]);
	if (t->shared)
	{
		uint64		b = pg_atomic_read_u64(t->shared);
		Rank		r = {1, b};

		if (b != PG_UINT64_MAX && (!have || rank_after(*bar, r)))
		{
			*bar = r;
			have = true;
		}
	}
	return have;
}

/* a full heap whose worst row has a value tightens the bound of the other participants (a NULL worst row is not a rank among the values) */
static void
share_bar(SiftTopN *t)
{
	Rank		w;
	uint64		cur;

	if (t->shared == NULL || t->n < t->limit)
		return;
	w = ent_rank(t, &t->heap[0]);
	cur = pg_atomic_read_u64(t->shared);
	while (w.cls == 1 && w.ord < cur && !pg_atomic_compare_exchange_u64(t->shared, &cur, w.ord))
		;
}

/* Bytes of the rows kept in memory before they go to a tuplesort: hash_mem, as the other tables of the scan (one allocation is MaxAllocSize at most) */
static int64
sort_memory(void)
{
	return Min((int64) get_hash_memory_limit(), (int64) 512 << 20);
}

/* A row of mem: the keys (0 for a NULL), the NULL flags, the TID's number */
static inline int64 *
mem_row(const SiftTopN *t, int64 i)
{
	return t->mem + i * t->stride;
}

static inline bool
mem_null(const SiftTopN *t, const int64 *r, int j)
{
	return (r[t->nkey + j / 64] >> (j % 64)) & 1;
}

static void
sort_put_tuple(SiftTopN *t, const int64 *k, const bool *isnull, uint64 tid)
{
	ExecClearTuple(t->sin);
	for (int j = 0; j < t->nkey; j++)
	{
		t->sin->tts_values[j] = Int64GetDatum(k[j]);
		t->sin->tts_isnull[j] = isnull[j];
	}
	t->sin->tts_values[t->nkey] = Int64GetDatum(tid);
	t->sin->tts_isnull[t->nkey] = false;
	ExecStoreVirtualTuple(t->sin);
	tuplesort_puttupleslot(t->sort, t->sin);
}

/* The rows in memory go to the tuplesort: work_mem is spent */
static void
mem_spill(SiftTopN *t)
{
	bool	   *isnull = palloc(t->nkey * sizeof(bool));
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(t));

	t->sort = sort_begin(t);
	MemoryContextSwitchTo(old);
	for (int64 i = 0; i < t->nmem; i++)
	{
		const int64 *r = mem_row(t, i);

		for (int j = 0; j < t->nkey; j++)
			isnull[j] = mem_null(t, r, j);
		sort_put_tuple(t, r, isnull, r[t->stride - 1]);
	}
	pfree(isnull);
	pfree(t->mem);
	t->mem = NULL;
}

/* One row that passes (no bound for the heap): kept in memory while the rows fit work_mem, else in the tuplesort */
static void
sort_put(SiftTopN *t, const int64 *k, const bool *isnull, uint64 tid)
{
	int64	   *r;

	if (t->mem && (t->nmem + 1) * t->stride * (int64) sizeof(int64) > sort_memory())
		mem_spill(t);
	if (t->mem == NULL)
	{
		sort_put_tuple(t, k, isnull, tid);
		return;
	}
	if (t->nmem == t->capmem)
	{
		t->capmem *= 2;
		t->mem = repalloc(t->mem, t->capmem * t->stride * sizeof(int64));
	}
	r = mem_row(t, t->nmem++);
	memset(r, 0, t->stride * sizeof(int64));
	for (int j = 0; j < t->nkey; j++)
		if (isnull[j])
			r[t->nkey + j / 64] |= (int64) 1 << (j % 64);
		else
			r[j] = k[j];
	r[t->stride - 1] = tid;
}

static int
mem_cmp(const void *a, const void *b, void *arg)
{
	const SiftTopN *t = arg;
	const int64 *x = a,
			   *y = b;

	for (int j = 0; j < t->nkey; j++)
	{
		bool		xn = mem_null(t, x, j),
					yn = mem_null(t, y, j);

		if (xn != yn)
			return xn == t->key[j].nulls_first ? -1 : 1;
		if (xn || x[j] == y[j])
			continue;
		return (x[j] < y[j]) != t->key[j].desc ? -1 : 1;
	}
	return SIFT_CMP3(x[t->stride - 1], y[t->stride - 1]);
}

/* With no bound for the heap: every row that passes goes to the sort, with its keys and its TID */
static void
sort_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	int			nk = t->nkey,
				nc = 0;

	grow_unit_buffers(t, nrow);
	for (int w = 0; w < (nrow + 63) / 64; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			t->cand[nc] = w * 64 + pg_rightmost_one_pos64(b);
			t->snull[(Size) nc * nk] = !row_key(&t->key[0], key0, t->cand[nc], &t->skey[(Size) nc * nk]);
			nc++;
		}
	if (nc == 0)
		return;
	for (int j = 1; j < nk; j++)
	{
		SiftSrcVec	v;

		sift_colsrc_read_rows(src, unit, t->key[j].leaf, t->cand, nc, &v);
		for (int x = 0; x < nc; x++)
			t->snull[(Size) x * nk + j] = !row_key(&t->key[j], &v, x, &t->skey[(Size) x * nk + j]);
	}
	sift_colsrc_tids(src, unit, t->cand, nc, t->tids);
	for (int x = 0; x < nc; x++)
		sort_put(t, &t->skey[(Size) x * nk], &t->snull[(Size) x * nk], tid_key(&t->tids[x]));
	t->nsort += nc;
}

/*
 * The rows of the unit are cut in two steps, so that the leaves of the other sort keys are read for a handful of rows and not for all of
 * them. First the first key of every row that passes: a row strictly worse on it than the n-th of the heap, or than the n-th of the unit
 * so far, cannot be in the result whatever its other keys are. Then the others, for the rows that remain (the n best and those tied with
 * the n-th).
 */
void
sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	const TopKey *k0 = &t->key[0];
	Rank		bar = {0, 0},
				mine;
	bool		barred = false;
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;

	if (t->all)
	{
		sort_unit(t, src, unit, key0, rmask, nrow);
		return;
	}
	grow_unit_buffers(t, nrow);
	t->ln = 0;
	barred = bar_rank(t, &bar);
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			int64		v = 0;
			bool		isnull = !row_key(k0, key0, i, &v);
			TopEnt		e;

			if (barred && rank_after(rank_of(k0, isnull, v), bar))
				continue;
			memset(&e, 0, sizeof(e));
			e.k[0] = v;
			e.nul = isnull;
			e.row = e.tie = i;
			t->crow[m] = i;
			t->ck[m] = v;
			t->cnul[m++] = isnull;
			heap_push(t, t->lh, &t->ln, &e);	/* the other keys are 0 in all of them: the order is the first key's, then the row */
			if (t->ln == t->limit)
			{
				mine = ent_rank(t, &t->lh[0]);
				if (!barred || rank_after(bar, mine))
				{
					bar = mine;
					barred = true;
				}
			}
		}
	for (int q = 0; q < m; q++)
	{
		TopEnt	   *e = &t->ce[nc];

		if (barred && rank_after(rank_of(k0, t->cnul[q], t->ck[q]), bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->k[0] = t->ck[q];
		e->nul = t->cnul[q];
		e->row = t->cand[nc] = t->crow[q];
		e->tie = e->row;
		nc++;
	}
	if (nc > 0)
		finish_unit(t, src, unit, nc);
}

bool
sift_topn_wants_zones(const SiftTopN *t)
{
	return !t->all && (t->key[0].vtype == SCV_I32 || t->key[0].vtype == SCV_I64);
}

/*
 * Can a row of a unit with this zone of the first sort column sort before the n-th best so far (or tie with it)? Nulls sort as the query
 * says, the rest between min and max. Rows tied on the first key may win on the others, so a bound equal to the n-th's is kept. The zone
 * counts the dead rows too: it only ever says "no" for a unit that has nothing better.
 */
bool
sift_topn_zone_viable(const SiftTopN *t, const SiftSrcZone *z)
{
	const TopKey *k = &t->key[0];
	Rank		bar,
				best;

	if (t->all || z->inexact || !bar_rank(t, &bar))
		return true;
	if (z->nnull > 0 && k->nulls_first)
		best = (Rank) {0, 0};
	else if (z->nnull == z->nvals)
		best = (Rank) {k->nulls_first ? 0 : 2, 0};
	else
		best = rank_of(k, false, k->desc ? z->max : z->min);
	return !rank_after(best, bar);
}

/* The zone of the unit of superblock sb, probed before the scan; it may be passed in any order */
void
sift_topn_probe(SiftTopN *t, uint32 sb, const SiftSrcZone *z)
{
	if (t->nprobe < TOPN_PROBES && !z->inexact && z->nnull < z->nvals)
	{
		t->probe[t->nprobe].sb = sb;
		t->probe[t->nprobe++].best = t->key[0].desc ? z->max : z->min;
	}
}

/* The superblocks to probe: spread over the table */
uint32
sift_topn_probe_sb(const SiftTopN *t, int i, uint32 nsb)
{
	return (uint32) (((uint64) i * (nsb - 1)) / (TOPN_PROBES - 1));
}

int
sift_topn_nprobes(void)
{
	return TOPN_PROBES;
}

void
sift_topn_start(SiftTopN *t, uint32 nsb)
{
	int			ups = 0,
				downs = 0;

	t->nsb = nsb;
	t->reverse = t->follows = false;
	for (uint32 i = 1; i < t->nprobe; i++)
	{
		ups += t->probe[i].best > t->probe[i - 1].best;
		downs += t->probe[i].best < t->probe[i - 1].best;
	}
	/* a column that grows (or shrinks) with the superblocks: probes in ascending sb order all go the same way */
	if (t->nprobe >= 4 && (ups == (int) t->nprobe - 1 || downs == (int) t->nprobe - 1))
	{
		t->follows = true;
		/* best values at the high end of the table: desc and growing, or asc and shrinking */
		t->reverse = (ups > 0) == t->key[0].desc;
	}
	t->left = nsb;
	/* a column that follows the table starts with one superblock, the one the best values are in, and grows to TOPN_WINDOW */
	t->size = t->follows ? 1 : TOPN_WINDOW;
}

bool
sift_topn_reverse(const SiftTopN *t)
{
	return t->reverse;
}

/* The next window of superblocks in the visit order, [*lo, *hi); *best: its superblock nearest to the best values */
bool
sift_topn_next_window(SiftTopN *t, uint32 *lo, uint32 *hi, uint32 *best)
{
	uint32		n = Min(t->size, t->left);

	if (n == 0)
		return false;
	*lo = t->reverse ? t->left - n : t->nsb - t->left;
	*hi = *lo + n;
	*best = t->reverse ? *hi - 1 : *lo;
	t->left -= n;
	t->size = Min(t->size * 2, TOPN_WINDOW);
	return true;
}

void
sift_topn_skip(SiftTopN *t)
{
	t->skipped++;
}

/*
 * The walk: for a first sort column that does not follow the table (the probes disagree) the superblocks are visited by the best value of
 * their zone, not in table order. Every superblock's zone is read first (sift_topn_walk_add, headers only), the superblocks are sorted by the
 * best row they can hold (sift_topn_walk_sort), and sift_topn_walk_next hands them out until the zone of the next cannot beat the n-th best:
 * the zones after it are no better, so the walk ends there. The result does not depend on the order: a superblock is left out only when no
 * row in it can sort before the n-th (a bound equal to it is kept: ties go on to the other keys and the TID).
 */
bool
sift_topn_follows(const SiftTopN *t)
{
	return t->follows;
}

void
sift_topn_walk_begin(SiftTopN *t, uint32 nsb)
{
	MemoryContext cxt = GetMemoryChunkContext(t);

	t->nsb = nsb;
	t->wbound = MemoryContextAllocZero(cxt, (Size) nsb * t->wkeys * sizeof(Rank));
	t->wunits = MemoryContextAllocZero(cxt, nsb * sizeof(uint32));
	t->word = MemoryContextAlloc(cxt, nsb * sizeof(uint32));
	for (uint32 i = 0; i < nsb; i++)
		t->word[i] = i;
	t->wpos = 0;
}

int
sift_topn_walk_keys(const SiftTopN *t)
{
	return t->wkeys;
}

/* The best rank key j can have in a unit with this zone: before every rank when the zone is not exact, a NULL first under NULLS FIRST, else the bound of the values (NULLs last when there are no values) */
static Rank
zone_best(const TopKey *k, const SiftSrcZone *z)
{
	if (z->inexact)
		return (Rank) {-1, 0};
	if (z->nnull == z->nvals || (z->nnull > 0 && k->nulls_first))
		return rank_of(k, true, 0);
	return rank_of(k, false, k->desc ? z->max : z->min);
}

/* z: the zones of the first sift_topn_walk_keys keys of superblock sb */
void
sift_topn_walk_add(SiftTopN *t, uint32 sb, uint32 units, const SiftSrcZone *z)
{
	for (int j = 0; j < t->wkeys; j++)
		t->wbound[(Size) sb * t->wkeys + j] = zone_best(&t->key[j], &z[j]);
	t->wunits[sb] = units;
}

static int
rank_cmp(Rank a, Rank b)
{
	return rank_after(a, b) ? 1 : rank_after(b, a) ? -1 : 0;
}

/* The superblocks by the best row they can hold, key by key: a unit whose bound on the keys so far ties with another is ordered by the next key */
static int
walk_cmp(const void *pa, const void *pb, void *arg)
{
	const SiftTopN *t = arg;
	uint32		a = *(const uint32 *) pa,
				b = *(const uint32 *) pb;

	for (int j = 0; j < t->wkeys; j++)
	{
		int			c = rank_cmp(t->wbound[(Size) a * t->wkeys + j], t->wbound[(Size) b * t->wkeys + j]);

		if (c != 0)
			return c;
	}
	return SIFT_CMP3(a, b);
}

/*
 * Can a row of a superblock with these bounds on the leading keys sort before the n-th best so far, or tie with it? A bound on each key is a
 * lower bound of its rows' keys, so a row's keys are not before the bounds in key order: the superblock is out when its bound is behind the
 * n-th on a key that the bounds before it tie on, and in when it is ahead of it (or a bound is none).
 */
static bool
bounds_viable(const SiftTopN *t, const Rank *bound)
{
	const TopEnt *w;

	if (t->all || t->n < t->limit)
		return true;
	w = &t->heap[0];
	for (int j = 0; j < t->wkeys; j++)
	{
		int			c = rank_cmp(bound[j], rank_of(&t->key[j], (w->nul >> j) & 1, w->k[j]));

		if (c != 0)
			return c < 0;
	}
	return true;
}

static bool
walk_viable(const SiftTopN *t, uint32 sb)
{
	return bounds_viable(t, &t->wbound[(Size) sb * t->wkeys]);
}

/*
 * The walk of a parallel scan, where a participant has no table of the bounds (the leader's order is all it gets): the superblock it takes
 * next, by the zones z of its leading keys. The first key is tested against the bound of every participant, all of them against the n-th row
 * of this participant's heap, which is no better than the n-th of the result.
 */
bool
sift_topn_walk_zone_viable(const SiftTopN *t, const SiftSrcZone *z)
{
	Rank		bound[SIFT_TOPN_MAX_KEYS];

	if (!sift_topn_zone_viable(t, &z[0]))
		return false;
	for (int j = 0; j < t->wkeys; j++)
		bound[j] = zone_best(&t->key[j], &z[j]);
	return bounds_viable(t, bound);
}

void
sift_topn_walk_sort(SiftTopN *t)
{
	qsort_arg(t->word, t->nsb, sizeof(uint32), walk_cmp, t);
}

bool
sift_topn_walk_next(SiftTopN *t, uint32 *sb)
{
	if (t->wpos >= t->nsb)
		return false;
	*sb = t->word[t->wpos];
	if (!walk_viable(t, *sb))
	{
		for (; t->wpos < t->nsb; t->wpos++)
			t->skipped += t->wunits[t->word[t->wpos]];
		return false;
	}
	t->wpos++;
	return true;
}

void
sift_topn_finish(SiftTopN *t)
{
	t->pos = 0;
	if (t->sort)
		tuplesort_performsort(t->sort);
	else if (t->all)
		qsort_arg(t->mem, t->nmem, t->stride * sizeof(int64), mem_cmp, t);
	else
	{
		t->res = t->heap;
		qsort_arg(t->res, t->n, sizeof(TopEnt), ent_cmp_arg, t);
	}
}

/* The next row of the result: the TID of the sort, or of the heap in order */
static bool
next_tid(SiftTopN *t, ItemPointerData *tid)
{
	bool		isnull;
	int64		tie;

	if (t->mem)
	{
		if (t->pos >= t->nmem)
			return false;
		tie = mem_row(t, t->pos++)[t->stride - 1];
		ItemPointerSet(tid, tie >> 16, tie & 0xFFFF);
		return true;
	}
	if (!t->all)
	{
		if (t->pos >= t->n)
			return false;
		*tid = t->res[t->pos++].tid;
		return true;
	}
	if (!tuplesort_gettupleslot(t->sort, true, false, t->sout, NULL))
		return false;
	tie = DatumGetInt64(slot_getattr(t->sout, t->nkey + 1, &isnull));
	ItemPointerSet(tid, tie >> 16, tie & 0xFFFF);
	return true;
}

bool
sift_topn_next(SiftTopN *t, TupleTableSlot *slot)
{
	ItemPointerData tid;
	bool		call_again = false,
				all_dead;

	if (!next_tid(t, &tid))
		return false;
	if (t->emitted++ < t->skip)
	{
		ExecClearTuple(slot);
		memset(slot->tts_isnull, true, t->nout * sizeof(bool));
		ExecStoreVirtualTuple(slot);
		return true;
	}
	/* the TID is the root of a HOT chain when the row was updated without a new index entry: the fetch follows the chain to the visible version */
	if (!table_index_fetch_tuple(t->fetch, &tid, t->snap, t->fslot, &call_again, &all_dead))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: top-N row (%u,%u) is not in the heap", ItemPointerGetBlockNumber(&tid), ItemPointerGetOffsetNumber(&tid)),
				 errhint("REINDEX the index.")));
	t->fetched++;
	ExecClearTuple(slot);
	for (int c = 0; c < t->nout; c++)
		slot->tts_values[c] = slot_getattr(t->fslot, t->attno[c], &slot->tts_isnull[c]);
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	return true;
}

/* ---- parallel scan: the participants keep a heap each, the leader merges them ---- */

#define TOPN_MAGIC	0x544F5031

bool
sift_topn_parallel_ok(const SiftTopN *t)
{
	return !t->all && t->limit > 0;
}

void
sift_topn_share(SiftTopN *t, pg_atomic_uint64 *bound)
{
	t->shared = bound;
}

void
sift_topn_bound_init(pg_atomic_uint64 *bound)
{
	pg_atomic_init_u64(bound, PG_UINT64_MAX);
}

/* A worker takes the order of the leader's probes */
void
sift_topn_visit(SiftTopN *t, uint32 nsb, bool reverse)
{
	t->nsb = nsb;
	t->reverse = reverse;
}

const uint32 *
sift_topn_walk_order(const SiftTopN *t)
{
	return t->word;
}

void
sift_topn_add_skipped(SiftTopN *t, int64 n)
{
	t->skipped += n;
}

/* The heap of a worker, for the leader: the entries are the keys and the TID of the rows, not the rows */
void
sift_topn_export(const SiftTopN *t, StringInfo b)
{
	uint32		h[2] = {TOPN_MAGIC, t->n};

	appendBinaryStringInfo(b, (const char *) h, sizeof(h));
	appendBinaryStringInfo(b, (const char *) t->heap, t->n * sizeof(TopEnt));
}

void
sift_topn_import(SiftTopN *t, const char *data, Size len)
{
	uint32		h[2];

	SIFT_INVARIANT(len >= sizeof(h), "sift: bad message of a parallel top-N");
	memcpy(h, data, sizeof(h));
	SIFT_INVARIANT(h[0] == TOPN_MAGIC && len == sizeof(h) + h[1] * sizeof(TopEnt), "sift: bad message of a parallel top-N");
	for (uint32 i = 0; i < h[1]; i++)
	{
		TopEnt		e;

		memcpy(&e, data + sizeof(h) + i * sizeof(TopEnt), sizeof(e));
		heap_push(t, t->heap, &t->n, &e);
	}
}

void
sift_topn_rewind(SiftTopN *t)
{
	t->pos = 0;
	t->emitted = 0;
	if (t->sort)
		tuplesort_rescan(t->sort);
}

int64
sift_topn_sorted(const SiftTopN *t)
{
	return t->all ? t->nsort : -1;
}
