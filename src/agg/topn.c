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
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "utils/varlena.h"

#include "agg/part.h"
#include "agg/topn.h"
#include "core/cmp.h"
#include "core/compat.h"

/* Superblocks per window of the visit order at most: the zones are looked at again, and the match set is asked for, once per window */
#define TOPN_WINDOW		8
/* Superblocks whose zones tell whether the sort column follows the table */
#define TOPN_PROBES		6

bool		sift_topn_zone_keys = true;
bool		sift_topn_runs = true;

bool
sift_topn_key_ok(const SiftSrcLeaf *lf)
{
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->lossy)
		return false;
	switch (lf->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return lf->vtype == SCV_I32 || lf->vtype == SCV_I64;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:
		case CASHOID:
		case TIMEOID:
			return lf->vtype == SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return lf->vtype == SCV_F64;
		case TEXTOID:
		case VARCHAROID:
		case UUIDOID:
		case BYTEAOID:
		case INETOID:
			return lf->vtype == SCV_CODE;
		default:
			return false;
	}
}

Node *
sift_topn_spec(int nkey, const int *leaf, const bool *desc, const bool *nulls_first, const Oid *sortop, const Oid *cmp, const Oid *collid, const SiftTopXform *xf, int nout, const int *attno, int flags, Node *count, Node *offset)
{
	List	   *l = list_make1_int(nkey);

	for (int j = 0; j < nkey; j++)
	{
		l = lappend_int(lappend_int(l, leaf[j]), (desc[j] ? 1 : 0) | (nulls_first[j] ? 2 : 0));
		l = lappend_int(lappend_int(lappend_int(l, (int) sortop[j]), (int) cmp[j]), (int) collid[j]);
		l = lappend_int(lappend_int(lappend_int(lappend_int(l, xf[j].kind), (int) (uint32) xf[j].c), (int) (uint32) ((uint64) xf[j].c >> 32)), (int) xf[j].coll);
	}
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

/* The bytes of a dictionary entry, copied out of the unit: what a heap entry holds for a key of byte strings (kept at int64 in TopEnt.k) */
typedef struct KeyBlob
{
	uint32		len;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} KeyBlob;

typedef struct TopKey
{
	int			leaf;
	bool		desc;
	bool		nulls_first;
	SiftSrcVecType vtype;
	/* a key of byte strings (text, uuid, bytea, inet; vtype SCV_CODE): an entry holds a KeyBlob of the dictionary entry, compared as PostgreSQL's sort does */
	bool		blob;
	uint8		bkind;			/* BK_*: how two entries compare */
	Oid			typid;			/* the type of the column of the sort when the rows go to a tuplesort */
	Oid			sortop;
	Oid			collid;
	FmgrInfo   *cmp;			/* BK_FMGR: the btree comparison function of the type */
	int			xk;				/* TXK_*: what the key does to its leaf's value (agg/topn.h) */
	int64		xc;				/* TXK_COALESCE: the constant */
	Oid			xcoll;			/* TXK_LOWER, TXK_UPPER: the collation of the call */
	FmgrInfo   *xfn;
	KeyBlob   **xmemo;			/* TXK_LOWER, TXK_UPPER: the result for each code of the dictionary (xid, xbytes, xn) once it is known (NULL: not yet) */
	const void *xid;
	const char *xbytes;
	uint32		xn;
} TopKey;

#define BK_TEXT		0			/* the bytes of a text: varstr_cmp under the collation of the query (byte order under C, strcoll then byte order else) */
#define BK_UUID		1			/* 16 bytes: memcmp, as uuid_cmp */
#define BK_BYTEA	2			/* the varlena image: memcmp of the contents, then the length, as byteacmp */
#define BK_FMGR		3			/* any other datum image (inet): the type's own comparison function, on an aligned copy */

#define KEY_BLOB(v)		((const KeyBlob *) (uintptr_t) (v))

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
	uint16		part;			/* the partition of the row's heap (a partitioned table), else 0 */
} TopEnt;

/* The rows of a partition are fetched through its own slot and index fetch, its columns by the attribute numbers of the partition */
typedef struct TopFetch
{
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	int		   *attno;
} TopFetch;

struct SiftTopN
{
	int			limit;			/* rows the heap holds; 0: none wanted (a LIMIT that leaves nothing) */
	bool		totals;			/* the aggregates over every row that passes are computed in this scan (TOPN_TOTALS): all units are read */
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
	int			nblob;			/* keys of byte strings */
	MemoryContext ucxt;			/* the copies of the unit's entries that the rows of one unit hold (reset at the next unit) */
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
	uint32	   *rwin;			/* a window of a sorted run (sift_topn_unit) */
	int			budget;			/* rows a unit gives in the first pass of a walk through sorted runs (0: all it can), sift_topn_run_budget */
	bool		revisit;		/* the second pass: only the units the first left unfinished */
	bool		yielded;		/* trunc[rpos] was given out and is not yet done */
	struct RunTrunc *trunc;
	int			ntrunc,
				captrunc,
				rpos;
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
	SiftSrcSource *src;
	Relation	rel;			/* the table the rows are fetched from: partitioned, or the heap */
	int			nfetch;			/* 1, or the partitions of a partitioned table: the fetches are made when a row of the partition is the first */
	TopFetch   *pf;				/* the heap row of a TID: the version an index scan finds, along a HOT chain */
	int64		skipped;
	int64		nrun;			/* units read through their sorted run */
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
		bool		blob = key && t->key[j].blob;

		TupleDescInitEntry(desc, j + 1, NULL, blob ? t->key[j].typid : INT8OID, -1, 0);
		att[j] = j + 1;
		/* a key of byte strings sorts by the operator and collation of the query: the comparator of the heap plan itself */
		op[j] = blob ? t->key[j].sortop : get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, key && t->key[j].desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		coll[j] = blob ? t->key[j].collid : InvalidOid;
		nf[j] = key && t->key[j].nulls_first;
	}
	sift_tupdesc_finalize(desc);
	t->sin = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	t->sout = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	return tuplesort_begin_heap(desc, n, att, op, coll, nf, work_mem, NULL, TUPLESORT_RANDOMACCESS);
}

/* The slot and the index fetch of a heap: of the partition part of a partitioned table, with the attribute numbers it has for the columns of the scan tuple (by the names of the table's) */
static void
fetch_begin(SiftTopN *t, int part)
{
	TopFetch   *f = &t->pf[part];
	Relation	heap = t->rel;
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(t));

	f->attno = t->attno;
	if (heap->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		heap = sift_aggpart_heap(t->src, part);
		f->attno = palloc(Max(t->nout, 1) * sizeof(int));
		for (int c = 0; c < t->nout; c++)
			f->attno[c] = get_attnum(RelationGetRelid(heap), get_attname(RelationGetRelid(t->rel), t->attno[c], false));
	}
	f->slot = table_slot_create(heap, NULL);
	f->fetch = sift_table_index_fetch_begin(heap);
	MemoryContextSwitchTo(old);
}

SiftTopN *
sift_topn_open(Node *spec, const SiftSrcLayout *lay, SiftSrcSource *src, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt)
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
		Oid			sortop = (Oid) list_nth_int(l, i++),
					cmp = (Oid) list_nth_int(l, i++),
					collid = (Oid) list_nth_int(l, i++);
		TopKey	   *k = &t->key[j];
		int			xk = list_nth_int(l, i++);
		uint32		xlo = (uint32) list_nth_int(l, i++),
					xhi = (uint32) list_nth_int(l, i++);
		Oid			xcoll = (Oid) list_nth_int(l, i++);

		*k = (TopKey) {.leaf = leaf, .desc = fl & 1, .nulls_first = (fl & 2) != 0, .vtype = lay->leaf[leaf].vtype,
			.xk = xk, .xc = (int64) (((uint64) xhi << 32) | xlo), .xcoll = xcoll};
		if (xk == TXK_LOWER || xk == TXK_UPPER)
		{
			k->xfn = palloc0(sizeof(FmgrInfo));
			fmgr_info(xk == TXK_LOWER ? F_LOWER_TEXT : F_UPPER_TEXT, k->xfn);
		}
		if (k->vtype == SCV_CODE)
		{
			k->blob = true;
			k->bkind = cmp == F_BTTEXTCMP ? BK_TEXT : cmp == F_UUID_CMP ? BK_UUID : cmp == F_BYTEACMP ? BK_BYTEA : BK_FMGR;
			k->typid = k->bkind == BK_TEXT ? TEXTOID : lay->leaf[leaf].typid;
			k->sortop = sortop;
			k->collid = collid;
			if (k->bkind == BK_FMGR)
			{
				k->cmp = palloc0(sizeof(FmgrInfo));
				fmgr_info(cmp, k->cmp);
			}
			t->nblob++;
		}
	}
	if (t->nblob > 0)
		t->ucxt = AllocSetContextCreate(cxt, "sift top-N unit", ALLOCSET_DEFAULT_SIZES);
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
	t->totals = (flags & TOPN_TOTALS) != 0;
	if (t->all && t->nblob > 0)
		t->sort = sort_begin(t);		/* byte strings do not fit the array of integers: the tuplesort holds them, in memory up to work_mem */
	else if (t->all)
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
	t->src = src;
	t->rel = heap;
	t->nfetch = heap->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ? sift_aggpart_count(src) : 1;
	t->pf = palloc0(t->nfetch * sizeof(TopFetch));
	if (heap->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		fetch_begin(t, 0);
	MemoryContextSwitchTo(old);
	return t;
}

void
sift_topn_close(SiftTopN *t)
{
	for (int i = 0; i < t->nfetch; i++)
	{
		if (t->pf[i].slot)
			ExecDropSingleTupleTableSlot(t->pf[i].slot);
		if (t->pf[i].fetch)
			table_index_fetch_end(t->pf[i].fetch);
	}
	if (t->sort)
	{
		ExecDropSingleTupleTableSlot(t->sin);
		ExecDropSingleTupleTableSlot(t->sout);
		tuplesort_end(t->sort);
	}
	if (t->ucxt)
		MemoryContextDelete(t->ucxt);
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

/* < 0, 0, > 0: the entry a sorts before, with or after b, ascending, as the type's btree comparison of the query's operator class says */
static int
bytes_cmp(const TopKey *k, const char *a, uint32 la, const char *b, uint32 lb)
{
	switch (k->bkind)
	{
		case BK_TEXT:
			return varstr_cmp(a, la, b, lb, k->collid);
		case BK_UUID:
			return memcmp(a, b, 16);
		case BK_BYTEA:
			{
				int			c = memcmp(a + VARHDRSZ, b + VARHDRSZ, Min(la, lb) - VARHDRSZ);

				return c != 0 ? c : SIFT_CMP3(la, lb);
			}
		default:
			{
				union
				{
					char		c[64];
					int64		align;
				}			x,
							y;
				char	   *pa = la <= sizeof(x.c) ? x.c : palloc(la),
						   *pb = lb <= sizeof(y.c) ? y.c : palloc(lb);
				int			c;

				memcpy(pa, a, la);
				memcpy(pb, b, lb);
				c = DatumGetInt32(FunctionCall2Coll(k->cmp, k->collid, PointerGetDatum(pa), PointerGetDatum(pb)));
				if (pa != x.c)
					pfree(pa);
				if (pb != y.c)
					pfree(pb);
				return c;
			}
	}
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
		if (t->key[j].blob)
		{
			const KeyBlob *x = KEY_BLOB(a->k[j]),
					   *y = KEY_BLOB(b->k[j]);
			int			c = bytes_cmp(&t->key[j], x->data, x->len, y->data, y->len);

			if (c == 0)
				continue;
			return (c < 0) != t->key[j].desc ? -1 : 1;
		}
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
	{
		*out = k->xc;
		return k->xk == TXK_COALESCE;
	}
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

static KeyBlob *blob_new(MemoryContext cxt, const char *p, uint32 len);

/* the string through lower() or upper() as the call of the query does it: a copy in the arena of the unit */
static KeyBlob *
xform_blob(SiftTopN *t, const TopKey *k, const char *p, uint32 len)
{
	MemoryContext old = MemoryContextSwitchTo(t->ucxt);
	struct varlena *in = palloc(VARHDRSZ + len),
			   *out;
	KeyBlob    *b;

	SET_VARSIZE(in, VARHDRSZ + len);
	memcpy(VARDATA(in), p, len);
	out = (struct varlena *) DatumGetPointer(FunctionCall1Coll(k->xfn, k->xcoll, PointerGetDatum(in)));
	b = blob_new(t->ucxt, VARDATA_ANY(out), VARSIZE_ANY_EXHDR(out));
	MemoryContextSwitchTo(old);
	return b;
}

/*
 * The entry `code` of a dictionary as the bytes of key j: a datum image (bytea, inet) is stored padded, the length the key means is its varlena
 * header's; a lower() or upper() key is the call's result, evaluated once for an entry of a unit
 */
static inline void
key_entry(SiftTopN *t, int j, const SiftSrcDict *d, uint32 code, const char **p, uint32 *len)
{
	TopKey	   *k = &t->key[j];

	*p = d->bytes + d->off[code];
	*len = d->off[code + 1] - d->off[code];
	if (k->bkind == BK_BYTEA || k->bkind == BK_FMGR)
	{
		varattrib_4b h;

		memcpy(&h, *p, sizeof(uint32));
		*len = VARSIZE_4B(&h);
	}
	if (k->xfn)
	{
		KeyBlob    *b;

		if (k->xid != d->id || k->xbytes != d->bytes || k->xn != d->n)
		{
			k->xmemo = MemoryContextAllocZero(t->ucxt, Max(d->n, 1) * sizeof(KeyBlob *));
			k->xid = d->id;
			k->xbytes = d->bytes;
			k->xn = d->n;
		}
		if ((b = k->xmemo[code]) == NULL)
			b = k->xmemo[code] = xform_blob(t, k, *p, *len);
		*p = b->data;
		*len = b->len;
	}
}

static KeyBlob *
blob_new(MemoryContext cxt, const char *p, uint32 len)
{
	KeyBlob    *b = MemoryContextAlloc(cxt, offsetof(KeyBlob, data) + len);

	b->len = len;
	memcpy(b->data, p, len);
	return b;
}

/* the key j of row i of v, of any key type: a byte string is a copy in the arena of the unit */
static bool
row_val(SiftTopN *t, int j, const SiftSrcVec *v, int i, int64 *out)
{
	const char *p;
	uint32		len;

	if (!t->key[j].blob)
		return row_key(&t->key[j], v, i, out);
	if (v->isnull && v->isnull[i])
		return false;
	key_entry(t, j, v->dict, ((const uint32 *) v->data)[i], &p, &len);
	*out = (int64) (uintptr_t) blob_new(t->ucxt, p, len);
	return true;
}

/* The heap of the result keeps its own copies of the byte strings of the entries it takes, and frees them when it drops the row */
static void
res_own(SiftTopN *t, TopEnt *e)
{
	MemoryContext cxt = GetMemoryChunkContext(t);

	for (int j = 0; j < t->nkey; j++)
		if (t->key[j].blob && !((e->nul >> j) & 1))
		{
			const KeyBlob *b = KEY_BLOB(e->k[j]);

			e->k[j] = (int64) (uintptr_t) blob_new(cxt, b->data, b->len);
		}
}

static void
res_free(const SiftTopN *t, const TopEnt *e)
{
	for (int j = 0; j < t->nkey; j++)
		if (t->key[j].blob && !((e->nul >> j) & 1))
			pfree((void *) KEY_BLOB(e->k[j]));
}

static void
res_push(SiftTopN *t, const TopEnt *e)
{
	TopEnt		c = *e,
				old;

	if (t->nblob == 0)
	{
		heap_push(t, t->heap, &t->n, e);
		return;
	}
	if (t->n == t->limit)
	{
		if (ent_cmp(t, e, &t->heap[0]) >= 0)
			return;
		old = t->heap[0];
		res_own(t, &c);
		heap_push(t, t->heap, &t->n, &c);
		res_free(t, &old);
		return;
	}
	res_own(t, &c);
	heap_push(t, t->heap, &t->n, &c);
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
			if (!row_val(t, j, &v, x, &t->ce[x].k[j]))
				t->ce[x].nul |= 1 << j;
	}
}

static void share_bar(SiftTopN *t);

/* The partition of the unit the source is at: the TIDs it gives are those of that partition's heap */
static inline uint16
unit_part(const SiftTopN *t, SiftSrcSource *src)
{
	return t->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ? sift_aggpart_current(src) : 0;
}

/* The key a row has in the order of ties: its TID, those of the partitions one after the other */
static inline uint64
tie_key(const ItemPointerData *tid, uint16 part)
{
	return tid_key(tid) | (uint64) part << 48;
}

static inline void
tie_tid(int64 tie, ItemPointerData *tid, uint16 *part)
{
	ItemPointerSet(tid, (tie >> 16) & PG_UINT32_MAX, tie & 0xFFFF);
	*part = tie >> 48;
}

/* The best n of those rows by all the keys go to the heap with their TIDs */
static void
finish_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc)
{
	uint16		part;

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
	part = unit_part(t, src);
	for (int i = 0; i < t->ln; i++)
	{
		TopEnt	   *e = &t->lh[i];
		uint32	   *at = bsearch(&e->row, t->rows, t->ln, sizeof(uint32), row_cmp);

		e->tid = t->tids[at - t->rows];
		e->part = part;
		e->tie = tie_key(&e->tid, part);
		res_push(t, e);
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

	if (t->shared == NULL || t->n < t->limit || t->key[0].blob)
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
		const TopKey *key = &t->key[j];

		if (!key->blob || isnull[j])
			t->sin->tts_values[j] = Int64GetDatum(k[j]);
		else if (key->bkind == BK_TEXT)
		{
			struct varlena *v = MemoryContextAlloc(t->ucxt, VARHDRSZ + KEY_BLOB(k[j])->len);

			SET_VARSIZE(v, VARHDRSZ + KEY_BLOB(k[j])->len);
			memcpy(VARDATA(v), KEY_BLOB(k[j])->data, KEY_BLOB(k[j])->len);
			t->sin->tts_values[j] = PointerGetDatum(v);
		}
		else
			t->sin->tts_values[j] = PointerGetDatum(KEY_BLOB(k[j])->data);	/* the image of a datum: what the tuplesort copies */
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
	uint16		part;

	grow_unit_buffers(t, nrow);
	for (int w = 0; w < (nrow + 63) / 64; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			t->cand[nc] = w * 64 + pg_rightmost_one_pos64(b);
			t->snull[(Size) nc * nk] = !row_val(t, 0, key0, t->cand[nc], &t->skey[(Size) nc * nk]);
			nc++;
		}
	if (nc == 0)
		return;
	for (int j = 1; j < nk; j++)
	{
		SiftSrcVec	v;

		sift_colsrc_read_rows(src, unit, t->key[j].leaf, t->cand, nc, &v);
		for (int x = 0; x < nc; x++)
			t->snull[(Size) x * nk + j] = !row_val(t, j, &v, x, &t->skey[(Size) x * nk + j]);
	}
	sift_colsrc_tids(src, unit, t->cand, nc, t->tids);
	part = unit_part(t, src);
	for (int x = 0; x < nc; x++)
		sort_put(t, &t->skey[(Size) x * nk], &t->snull[(Size) x * nk], tie_key(&t->tids[x], part));
	t->nsort += nc;
}

/* The sign of a row of the first key (a NULL, or the entry `code` of dictionary d) against the entry bar, by the first key alone: < 0 before it, > 0 after it */
static int
vs_bar(SiftTopN *t, const SiftSrcDict *d, bool isnull, uint32 code, const TopEnt *bar)
{
	const TopKey *k = &t->key[0];
	bool		bn = bar->nul & 1;
	const char *p;
	uint32		len;
	int			c;

	if (isnull != bn)
		return isnull == k->nulls_first ? -1 : 1;
	if (isnull)
		return 0;
	key_entry(t, 0, d, code, &p, &len);
	c = bytes_cmp(k, p, len, KEY_BLOB(bar->k[0])->data, KEY_BLOB(bar->k[0])->len);
	return k->desc ? -c : c;
}

/* Is the row strictly after the bar? The bar only moves toward the front, so an entry that was after it stays after it: worse[code] remembers it, and a unit of few distinct strings compares each once */
static bool
after_bar(SiftTopN *t, uint8 *worse, const SiftSrcDict *d, bool isnull, uint32 code, const TopEnt *bar)
{
	if (isnull)
		return vs_bar(t, d, true, 0, bar) > 0;
	if (worse[code])
		return true;
	if (vs_bar(t, d, false, code, bar) <= 0)
		return false;
	worse[code] = 1;
	return true;
}

/* Entries of a sorted run are read in windows that double up to this many */
#define RUN_WIN_MIN		32
#define RUN_WIN_MAX		512

/* A unit whose run the first pass left at the group of rank next: the rows before it are taken */
typedef struct RunTrunc
{
	uint64		id;
	uint32		sb;
	Rank		next;
	bool		done;
} RunTrunc;

typedef struct RunWalk
{
	SiftTopN   *t;
	SiftSrcSource *src;
	const SiftSrcUnit *unit;
	const SiftSrcVec *key0;
	const uint64 *rmask;
	uint32		n;				/* rows of the unit: entries of the run */
	uint32		lo,
				hi;				/* the entries rwin holds */
	uint32		win;
	int			nc;				/* candidate rows in t->cand */
	int			passing;		/* rows that pass in the groups taken so far */
	Rank		bar;
	bool		barred;
	bool		have;			/* prev is the rank of the group before */
	Rank		prev;
	int			budget;			/* rows to take before leaving the rest of the unit to the second pass (0: no limit) */
	bool		resumed;		/* the second pass of this unit: the groups before resume are taken */
	Rank		resume;
	bool		trunc;
	Rank		next;			/* trunc: the rank of the first group left */
} RunWalk;

static void
run_corrupt(RunWalk *w, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift_col sorted run is corrupted"),
			 errdetail("The sorted run of the first sort column of a unit of %u rows %s.", w->n, what)));
}

/* The row at position pos of the run; the window is read toward the direction of the walk */
static uint32
run_at(RunWalk *w, uint32 pos, bool back)
{
	if (pos < w->lo || pos >= w->hi)
	{
		uint32		cnt = back ? Min(w->win, pos + 1) : Min(w->win, w->n - pos),
					lo = back ? pos + 1 - cnt : pos;

		sift_colsrc_run_read(w->src, w->unit, w->t->key[0].leaf, lo, cnt, w->t->rwin);
		w->lo = lo;
		w->hi = lo + cnt;
		w->win = Min(w->win * 2, RUN_WIN_MAX);
	}
	return w->t->rwin[pos - w->lo];
}

static void
run_key(RunWalk *w, uint32 row, int64 *v)
{
	if (!row_key(&w->t->key[0], w->key0, row, v))
		run_corrupt(w, "has a NULL row among those with a value");
}

/* A group of equal first keys, rank, begins: false when the first pass took it. The ranks must come in order */
static bool
run_open(RunWalk *w, Rank rank)
{
	if (w->have && !rank_after(rank, w->prev))
		run_corrupt(w, "is not in the order of its keys");
	w->prev = rank;
	w->have = true;
	return !(w->resumed && rank_after(w->resume, rank));
}

/* The group ends; its rows that pass are t->cand[g0, nc), backwards when the walk was. With one sort key the tied rows after the n-th come after it by TID and are not in the result */
static void
run_close(RunWalk *w, int g0, Rank rank, bool back)
{
	SiftTopN   *t = w->t;

	for (int a = g0, b = w->nc - 1; back && a < b; a++, b--)
	{
		uint32		x = t->cand[a];

		t->cand[a] = t->cand[b];
		t->cand[b] = x;
	}
	if (t->nkey == 1)
		w->nc = Min(w->nc, g0 + t->limit - w->passing);
	w->passing += w->nc - g0;
	if (w->passing >= t->limit && (!w->barred || rank_after(w->bar, rank)))
	{
		w->bar = rank;
		w->barred = true;
	}
}

/* Whether the rows of the group at hand reach the n-th, so that the rest of an ascending group is out */
static bool
run_full(const RunWalk *w, int g0)
{
	return w->t->nkey == 1 && w->passing + w->nc - g0 >= w->t->limit;
}

static inline bool
run_pass(const RunWalk *w, uint32 row)
{
	return w->rmask[row >> 6] >> (row & 63) & 1;
}

static bool
run_stop(const RunWalk *w, Rank r)
{
	return w->barred && rank_after(r, w->bar);
}

/* The first pass has its rows: the group of rank r and the ones after are left to the second pass, which the heap's bar may spare */
static bool
run_cut(RunWalk *w, Rank r)
{
	if (w->budget == 0 || w->passing < w->budget)
		return false;
	w->trunc = true;
	w->next = r;
	return true;
}

/* The rows of the NULL group (positions [nvals, nvals + nnull)) */
static void
run_nulls(RunWalk *w, uint32 nvals, uint32 nnull)
{
	Rank		r = rank_of(&w->t->key[0], true, 0);
	int			g0 = w->nc;
	uint32		prev = 0;

	if (nnull == 0 || run_stop(w, r) || !run_open(w, r))
		return;
	for (uint32 pos = nvals; pos < nvals + nnull; pos++)
	{
		uint32		row = run_at(w, pos, false);

		if (pos > nvals && row <= prev)
			run_corrupt(w, "has the rows of equal keys out of order");
		prev = row;
		if (!run_pass(w, row))
			continue;
		w->t->cand[w->nc++] = row;
		if (run_full(w, g0))
			break;
	}
	run_close(w, g0, r, false);
}

/*
 * The rows with a value, best group first, until a group cannot be in the result. A row is tested against the mask before its key is read, so the
 * keys of the rows that do not pass (zero in a vector read for the rows that pass only) are never looked at; a group is the rows that pass with an
 * equal key. The run holds equal keys by row ascending: a descending walk meets them backwards and puts them right at the end of the group.
 */
static void
run_values(RunWalk *w, uint32 nvals)
{
	const TopKey *k0 = &w->t->key[0];
	bool		desc = k0->desc,
				open = false,
				skip = false;
	int			g0 = 0;
	int64		gv = 0;
	uint32		prev = 0;
	Rank		r = {0};

	for (uint32 i = 0; i < nvals; i++)
	{
		uint32		row = run_at(w, desc ? nvals - 1 - i : i, desc);
		int64		v;

		if (!run_pass(w, row))
			continue;
		run_key(w, row, &v);
		if (open && v == gv)
		{
			if (desc ? row >= prev : row <= prev)
				run_corrupt(w, "has the rows of equal keys out of order");
		}
		else
		{
			if (open && !skip)
				run_close(w, g0, r, desc);
			r = rank_of(k0, false, v);
			if (run_stop(w, r) || run_cut(w, r))
				return;
			open = true;
			skip = !run_open(w, r);
			g0 = w->nc;
			gv = v;
		}
		prev = row;
		if (skip)
			continue;
		w->t->cand[w->nc++] = row;
		if (!desc && run_full(w, g0))
			break;
	}
	if (open && !skip)
		run_close(w, g0, r, desc);
}

/*
 * The unit from its sorted run (docs/COL.md 2.14): the groups of equal first keys in the order of the sort, from the best, until one cannot be in the
 * result, where the rows are read and tested one by one otherwise. The candidates are what the other loop finds: the rows that pass whose first key is
 * not after the n-th of the unit's or the heap's, so the rest is the same. False: the unit has no run, or the rows that pass are too few for the walk
 * to pay (it reads about n / share of the rows that pass) and the heap has no bar yet.
 */
static bool
run_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	RunWalk		w = {.t = t, .src = src, .unit = unit, .key0 = key0, .rmask = rmask, .n = nrow, .win = RUN_WIN_MIN};
	uint32		nvals,
				nnull,
				npass = 0;
	uint64		id;
	int			m = 0;
	const TopKey *k0 = &t->key[0];
	RunTrunc   *rt = NULL;

	/* the run orders the column, not the expression of it (coalesce moves the NULLs, lower() folds the strings) */
	if (!sift_topn_runs || k0->xk != TXK_NONE || !sift_colsrc_run_info(src, unit, k0->leaf, &nvals, &nnull, &id))
		return false;
	if (t->revisit)
	{
		/* the units the first pass finished, or took no run of, are not read again */
		for (int i = 0; i < t->ntrunc && rt == NULL; i++)
			if (t->trunc[i].id == id && !t->trunc[i].done)
				rt = &t->trunc[i];
		if (rt == NULL)
			return true;
		rt->done = true;
		w.resumed = true;
		w.resume = rt->next;
	}
	else
	{
		for (int i = 0; i < (nrow + 63) / 64; i++)
			npass += pg_popcount64(rmask[i]);
		/* a unit with fewer rows than the walk would take for its own bar is read through the run only when the heap's bar already cuts it */
		if ((uint64) (t->limit + 1) * 2 > npass && !bar_rank(t, &w.bar))
			return false;
		w.budget = t->budget;
	}
	grow_unit_buffers(t, nrow);
	if (t->rwin == NULL)
		t->rwin = MemoryContextAlloc(GetMemoryChunkContext(t), RUN_WIN_MAX * sizeof(uint32));
	w.barred = bar_rank(t, &w.bar);
	if (rt && run_stop(&w, rt->next))
		return true;
	if (k0->nulls_first)
		run_nulls(&w, nvals, nnull);
	run_values(&w, nvals);
	if (!k0->nulls_first && !w.trunc)
		run_nulls(&w, nvals, nnull);
	/* the rows whose value overflowed the leaf are tested one by one: the run does not order them */
	for (uint32 pos = nvals + nnull; pos < (uint32) nrow && !w.resumed; pos++)
	{
		uint32		row = run_at(&w, pos, false);

		if (rmask[row >> 6] >> (row & 63) & 1)
			t->cand[w.nc++] = row;
	}
	qsort(t->cand, w.nc, sizeof(uint32), row_cmp);
	for (int q = 0; q < w.nc; q++)
	{
		TopEnt	   *e = &t->ce[m];
		int64		v = 0;
		bool		isnull = !row_key(k0, key0, t->cand[q], &v);

		if (q > 0 && t->cand[q] == t->cand[q - 1])
			run_corrupt(&w, "names a row twice");
		if (w.barred && rank_after(rank_of(k0, isnull, v), w.bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->k[0] = v;
		e->nul = isnull;
		e->row = e->tie = t->cand[q];
		t->cand[m++] = e->row;
	}
	t->nrun++;
	if (w.trunc)
	{
		if (t->ntrunc == t->captrunc)
		{
			t->captrunc = Max(16, 2 * t->captrunc);
			t->trunc = grow(t, t->trunc, t->captrunc * sizeof(RunTrunc));
		}
		t->trunc[t->ntrunc++] = (RunTrunc) {id, unit->sb, w.next, false};
	}
	if (m > 0)
		finish_unit(t, src, unit, m);
	return true;
}

/*
 * sift_topn_unit for a first key of byte strings, which has no rank to compare as a number: the bar is the heap's worst entry or the unit's
 * n-th so far (whichever sorts first), and the rows are compared with it by the entry of the dictionary, as the type's comparison says.
 */
static void
unit_blob(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	const uint32 *code = key0->data;
	const TopEnt *bar = t->n == t->limit ? &t->heap[0] : NULL;
	uint8	   *worse = MemoryContextAllocZero(t->ucxt, Max(key0->dict->n, 1));
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;

	grow_unit_buffers(t, nrow);
	t->ln = 0;
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			bool		isnull = key0->isnull && key0->isnull[i];
			TopEnt		e;

			if (bar && after_bar(t, worse, key0->dict, isnull, code[i], bar))
				continue;
			t->crow[m] = i;
			t->ck[m] = code[i];
			t->cnul[m++] = isnull;
			/* a row that ties with the unit's n-th stays a candidate, but it comes after it: the rows are visited in order */
			if (t->ln == t->limit && vs_bar(t, key0->dict, isnull, code[i], &t->lh[0]) >= 0)
				continue;
			memset(&e, 0, sizeof(e));
			e.nul = isnull;
			e.row = e.tie = i;
			if (!isnull)
			{
				const char *p;
				uint32		len;

				key_entry(t, 0, key0->dict, code[i], &p, &len);
				e.k[0] = (int64) (uintptr_t) blob_new(t->ucxt, p, len);
			}
			heap_push(t, t->lh, &t->ln, &e);
			if (t->ln == t->limit && (bar == NULL || key_cmp_n(t, &t->lh[0], bar, 1) < 0))
				bar = &t->lh[0];
		}
	for (int q = 0; q < m; q++)
	{
		TopEnt	   *e = &t->ce[nc];

		if (bar && after_bar(t, worse, key0->dict, t->cnul[q], t->ck[q], bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->nul = t->cnul[q];
		e->row = t->cand[nc] = t->crow[q];
		e->tie = e->row;
		if (!t->cnul[q])
		{
			const char *p;
			uint32		len;

			key_entry(t, 0, key0->dict, t->ck[q], &p, &len);
			e->k[0] = (int64) (uintptr_t) blob_new(t->ucxt, p, len);
		}
		nc++;
	}
	if (nc > 0)
		finish_unit(t, src, unit, nc);
}

/*
 * Sorted runs in a walk over the units: the first pass takes about 2 n / units rows of each (their best, so that together they hold the n best of all but
 * for the odd unit with more), which fills the heap with a bar close to the last, and leaves the units that have more rows below it to a second pass,
 * which reads only them (sift_topn_revisit_next) and takes the rows after the ones taken. One pass over the units, each at most as far as the n-th row
 * of the heap in the end needs, where one unit after the other would take n / k rows of the k-th.
 */
void
sift_topn_run_budget(SiftTopN *t, uint32 units)
{
	int			b;

	if (!sift_topn_runs || t->key[0].xk != TXK_NONE || t->all || t->sort || t->limit <= 0 || units < 8)
		return;
	b = Max(2, (int) (((uint64) t->limit * 2 + units - 1) / units));
	if (b * 2 < t->limit)
		t->budget = b;
}

static int
trunc_cmp(const void *a, const void *b)
{
	const RunTrunc *x = a,
			   *y = b;

	return rank_after(x->next, y->next) ? 1 : rank_after(y->next, x->next) ? -1 : 0;
}

/* The superblock of the next unit the first pass left unfinished whose rest the bar does not exclude; false when there is none */
bool
sift_topn_revisit_next(SiftTopN *t, uint32 *sb)
{
	Rank		bar;

	if (!t->revisit)
	{
		t->revisit = true;
		t->rpos = 0;
		if (t->ntrunc > 1)
			qsort(t->trunc, t->ntrunc, sizeof(RunTrunc), trunc_cmp);
	}
	if (t->yielded)
		t->trunc[t->rpos].done = true;
	t->yielded = false;
	while (t->rpos < t->ntrunc && t->trunc[t->rpos].done)
		t->rpos++;
	if (t->rpos >= t->ntrunc)
		return false;
	if (bar_rank(t, &bar) && rank_after(t->trunc[t->rpos].next, bar))
		return false;
	*sb = t->trunc[t->rpos].sb;
	/* done when the next is asked for, whether the unit was read (it marks itself) or its zone could not beat the bar now */
	t->yielded = true;
	return true;
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

	if (t->ucxt)
	{
		MemoryContextReset(t->ucxt);
		for (int j = 0; j < t->nkey; j++)
			t->key[j].xid = NULL;
	}
	if (t->all)
	{
		sort_unit(t, src, unit, key0, rmask, nrow);
		return;
	}
	if (k0->blob)
	{
		unit_blob(t, src, unit, key0, rmask, nrow);
		return;
	}
	if (t->revisit)
	{
		(void) run_unit(t, src, unit, key0, rmask, nrow);
		return;
	}
	if (run_unit(t, src, unit, key0, rmask, nrow))
		return;
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

/* The zone of the key's values from the zone of its leaf: coalesce(x, c) is c where x is NULL */
static SiftSrcZone
key_zone(const TopKey *k, const SiftSrcZone *z)
{
	SiftSrcZone o = *z;

	if (k->xk != TXK_COALESCE || z->inexact || z->nnull == 0)
		return o;
	if (z->nnull == z->nvals)
		o.min = o.max = k->xc;
	else
	{
		o.min = Min(z->min, k->xc);
		o.max = Max(z->max, k->xc);
	}
	o.nnull = 0;
	return o;
}

bool
sift_topn_wants_zones(const SiftTopN *t)
{
	return !t->all && !t->totals && (t->key[0].vtype == SCV_I32 || t->key[0].vtype == SCV_I64);
}

/*
 * Can a row of a unit with this zone of the first sort column sort before the n-th best so far (or tie with it)? Nulls sort as the query
 * says, the rest between min and max. Rows tied on the first key may win on the others, so a bound equal to the n-th's is kept. The zone
 * counts the dead rows too: it only ever says "no" for a unit that has nothing better.
 */
bool
sift_topn_zone_viable(const SiftTopN *t, const SiftSrcZone *zone)
{
	const TopKey *k = &t->key[0];
	SiftSrcZone zk = key_zone(k, zone);
	const SiftSrcZone *z = &zk;
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
sift_topn_probe(SiftTopN *t, uint32 sb, const SiftSrcZone *zone)
{
	SiftSrcZone zk = key_zone(&t->key[0], zone);
	const SiftSrcZone *z = &zk;

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
zone_best(const TopKey *k, const SiftSrcZone *zone)
{
	SiftSrcZone zk = key_zone(k, zone);
	const SiftSrcZone *z = &zk;

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

/* The next row of the result: the TID of the sort, or of the heap in order, and the partition it is in */
static bool
next_tid(SiftTopN *t, ItemPointerData *tid, uint16 *part)
{
	bool		isnull;
	int64		tie;

	if (t->mem)
	{
		if (t->pos >= t->nmem)
			return false;
		tie = mem_row(t, t->pos++)[t->stride - 1];
		tie_tid(tie, tid, part);
		return true;
	}
	if (!t->all)
	{
		if (t->pos >= t->n)
			return false;
		*tid = t->res[t->pos].tid;
		*part = t->res[t->pos++].part;
		return true;
	}
	if (!tuplesort_gettupleslot(t->sort, true, false, t->sout, NULL))
		return false;
	tie = DatumGetInt64(slot_getattr(t->sout, t->nkey + 1, &isnull));
	tie_tid(tie, tid, part);
	return true;
}

bool
sift_topn_next(SiftTopN *t, TupleTableSlot *slot)
{
	ItemPointerData tid;
	uint16		part;
	TopFetch   *f;
	bool		call_again = false,
				all_dead;

	if (!next_tid(t, &tid, &part))
		return false;
	if (t->emitted++ < t->skip)
	{
		ExecClearTuple(slot);
		memset(slot->tts_isnull, true, t->nout * sizeof(bool));
		ExecStoreVirtualTuple(slot);
		return true;
	}
	f = &t->pf[part];
	if (f->slot == NULL)
		fetch_begin(t, part);
	/* the TID is the root of a HOT chain when the row was updated without a new index entry: the fetch follows the chain to the visible version */
	if (!table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: top-N row (%u,%u) is not in the heap", ItemPointerGetBlockNumber(&tid), ItemPointerGetOffsetNumber(&tid)),
				 errhint("REINDEX the index.")));
	t->fetched++;
	ExecClearTuple(slot);
	for (int c = 0; c < t->nout; c++)
		slot->tts_values[c] = slot_getattr(f->slot, f->attno[c], &slot->tts_isnull[c]);
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	return true;
}

/* ---- parallel scan: the participants keep a heap each, the leader merges them ---- */

#define TOPN_MAGIC	0x544F5031

bool
sift_topn_parallel_ok(const SiftTopN *t)
{
	return !t->all && !t->totals && t->limit > 0;
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
sift_topn_add_skipped(SiftTopN *t, int64 n, int64 runs)
{
	t->skipped += n;
	t->nrun += runs;
}

int64
sift_topn_runs_read(const SiftTopN *t)
{
	return t->nrun;
}

/* The heap of a worker, for the leader: the entries are the keys and the TID of the rows, not the rows */
void
sift_topn_export(const SiftTopN *t, StringInfo b)
{
	uint32		h[2] = {TOPN_MAGIC, t->n};

	appendBinaryStringInfo(b, (const char *) h, sizeof(h));
	appendBinaryStringInfo(b, (const char *) t->heap, t->n * sizeof(TopEnt));
	/* then the byte strings of the entries, in the order of the entries and keys: a KeyBlob each, for the keys that are not NULL */
	for (int i = 0; t->nblob > 0 && i < t->n; i++)
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].blob && !((t->heap[i].nul >> j) & 1))
				appendBinaryStringInfo(b, (const char *) KEY_BLOB(t->heap[i].k[j]), offsetof(KeyBlob, data) + KEY_BLOB(t->heap[i].k[j])->len);
}

void
sift_topn_import(SiftTopN *t, const char *data, Size len)
{
	uint32		h[2];
	Size		off;

	SIFT_INVARIANT(len >= sizeof(h), "sift: bad message of a parallel top-N");
	memcpy(h, data, sizeof(h));
	SIFT_INVARIANT(h[0] == TOPN_MAGIC && (Size) h[1] <= (len - sizeof(h)) / sizeof(TopEnt), "sift: bad message of a parallel top-N");
	off = sizeof(h) + h[1] * sizeof(TopEnt);
	for (uint32 i = 0; i < h[1]; i++)
	{
		TopEnt		e;

		memcpy(&e, data + sizeof(h) + i * sizeof(TopEnt), sizeof(e));
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].blob && !((e.nul >> j) & 1))
			{
				uint32		n;

				SIFT_INVARIANT(len - off >= sizeof(n), "sift: bad message of a parallel top-N");
				memcpy(&n, data + off, sizeof(n));
				SIFT_INVARIANT(len - off - sizeof(n) >= n, "sift: bad message of a parallel top-N");
				e.k[j] = (int64) (uintptr_t) blob_new(CurrentMemoryContext, data + off + sizeof(n), n);
				off += sizeof(n) + n;
			}
		res_push(t, &e);
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].blob && !((e.nul >> j) & 1))
				pfree((void *) KEY_BLOB(e.k[j]));
	}
	SIFT_INVARIANT(off == len, "sift: bad message of a parallel top-N");
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
