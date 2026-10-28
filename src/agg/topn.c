/*
 * agg/topn.c - ORDER BY <columns> LIMIT n from the column units (agg/topn.h).
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_opfamily_d.h"
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/numeric.h"
#include "utils/tuplesort.h"
#include "utils/uuid.h"
#include "utils/varlena.h"

#include "agg/leaf_int.h"
#include "agg/gord.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/kern.h"
#include "col/kern.h"
#include "core/cmp.h"
#include "core/compat.h"

/* Superblocks per window of the visit order at most: the zones are looked at again, and the match set is asked for, once per window */
#define TOPN_WINDOW		8
/* Superblocks whose zones tell whether the sort column follows the table */
#define TOPN_PROBES		6
/* Rows a unit gives the heap, at least, for a column of the output to be read from it: decoding costs as much as fetching this many rows from the heap, and a unit that gives less than half the heap is not one most of the final rows come from */
#define TOPN_OUT_ROWS	16
/* Rows of a unit the first key of which is an integer, at least, for the best value of a segment to be looked at before its rows (vec_rows), and the rows of a segment */
#define TOPN_VEC_ROWS	512
#define TOPN_SEG		256
/*
 * ... and one row in this many of the unit passes the filters at least: the passes of the kernels cost the same whatever the rows are, so fewer rows go
 * to the heap one by one. Measured on 1,000,000 rows (LIMIT 10, keys in no order): at 30% and above the kernels take 25 to 45% fewer instructions, at 15-20%
 * the same to 14% fewer, at 10% and 6% 2 to 10% more (the decode of the filter and the key column is what costs, 28M of 28M instructions) and the time is the same;
 * a key that follows the table gains 1.3M of 2.3M at 10%, which is 0.03 ms. One row in 8 keeps all of it but that.
 */
#define TOPN_VEC_DENSITY	8

bool		sift_topn_zone_keys = true;
#ifdef SIFT_TESTING
bool		sift_topn_heads = true;
#endif
bool		sift_topn_runs = true;
bool		sift_topn_gorder_on = true;

/* Does a heap entry keep the value of the leaf as an integer (leaf_datum gives its Datum): the output of a row is then read from its unit */
bool
sift_topn_out_ok(const SiftSrcLeaf *lf)
{
	switch (lf->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return sift_topn_key_ok(lf) && (lf->vtype == SCV_I32 || lf->vtype == SCV_I64);
		default:
			return false;
	}
}

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
		case NUMERICOID:
			/* a wide numeric is read through the dictionary of its datums (agg/wide.c), a NaN among them, which numeric_cmp sorts last */
			return lf->vtype == SCV_I64 || (lf->wide && lf->vtype == SCV_CODE);
		case INTERVALOID:
			/* read through the dictionary of its datums (agg/wide.c); interval_cmp compares the total, so 1 mon and 30 days tie */
			return lf->wide && lf->vtype == SCV_CODE;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
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
			/* an enum is the vector of the OIDs of its values: the scan ranks them in the order of the type */
			return type_is_enum(lf->typid) && lf->vtype == SCV_I32 && !lf->wide;
	}
}

Node *
sift_topn_spec(int nkey, const int *leaf, const bool *desc, const bool *nulls_first, const Oid *sortop, const Oid *cmp, const Oid *collid, const SiftTopXform *xf, List *exprs, int nout, const int *attno, const int *oleaf, int flags, Node *count, Node *offset)
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
		l = lappend_int(lappend_int(l, attno[c]), oleaf[c]);
	l = lappend_int(l, flags);
	return (Node *) list_make4(l, count, offset, exprs);
}

#define SPEC_KEYS(spec)		((List *) linitial((List *) (spec)))
#define SPEC_COUNT(spec)	((Node *) lsecond((List *) (spec)))
#define SPEC_OFFSET(spec)	((Node *) lthird((List *) (spec)))
#define SPEC_EXPRS(spec)	((List *) lfourth((List *) (spec)))

/* The rows the scan holds for LIMIT and OFFSET, as EXPLAIN shows them: their sum when they are constants; ", sorted" when the plan sorts every row that passes; ", stored dictionary" when the first key is read as the codes of the stored dictionary */
char *
sift_topn_limit_text(Node *spec)
{
	Node	   *count = SPEC_COUNT(spec),
			   *offset = SPEC_OFFSET(spec);
	int			flags = llast_int(SPEC_KEYS(spec));
	const char *how = psprintf("%s%s", (flags & TOPN_SORT) ? ", sorted" : "", (flags & TOPN_RAWKEY) ? ", stored dictionary" : "");
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

/*
 * A sort expression: PostgreSQL's own ExprState over a virtual row of the leaves it reads, evaluated for every row of a unit that passes (the normal plan
 * evaluates a sort expression for each of them, so an error is raised for the same rows), its results kept as a vector of the type the key sorts by
 */
typedef struct TopIn
{
	int			leaf;
	Oid			typid;
	SiftSrcVecType vtype;
	int			scale;			/* numeric leaf: digits after the point */
	const SiftSrcVec *v;		/* the unit's */
	Datum	   *memo;			/* a leaf of strings: the datum of each code of the unit's dictionary, once built */
	bool	   *built;
} TopIn;

typedef struct TopExpr
{
	ExprState  *state;
	ExprContext *econtext;
	TupleTableSlot *slot;
	int			nin;
	TopIn	   *in;
	Oid			rtype;
} TopExpr;

static SiftSrcVecType expr_vtype(Oid typid);
static TopExpr *expr_open(List *xs, const SiftSrcLayout *lay, Oid rtype, ExprContext *outer, MemoryContext cxt);

#define TOP_RESID	4			/* leaves the conditions of a lazy filter may read */
#define TOP_GKEYS 4

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
	int16		typlen;			/* BK_FMGR: the length of the type, -1 for a varlena (an interval is 16 bytes, padded in the dictionary) */
	int			xk;				/* TXK_*: what the key does to its leaf's value (agg/topn.h) */
	int64		xc;				/* TXK_COALESCE: the constant */
	Oid			xcoll;			/* TXK_LOWER, TXK_UPPER: the collation of the call */
	FmgrInfo   *xfn;
	struct TopExpr *x;			/* TXK_EXPR: the evaluator of the sort expression */
	SiftSrcVec	xv;				/* ... its result for the rows of the unit */
	KeyBlob   **xmemo;			/* TXK_LOWER, TXK_UPPER: the result for each code of the dictionary (xid, xbytes, xn) once it is known (NULL: not yet) */
	const void *xid;
	const char *xbytes;
	uint32		xn;
	/* a key of an enum: the OIDs of its values (ascending) and the position of each in the order of the type, which is the key */
	uint32		nenum;
	Oid		   *eoid;
	uint32	   *erank;
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
	uint32		oi;				/* the place of the entry's output columns in SiftTopN.ov (set by res_push) */
	uint8		nul;			/* bit j: key j is NULL */
	bool		hasv;			/* the output columns are in ov: the row is not fetched from the heap */
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

/*
 * The order code of a dictionary of a first key that compares as bytes (text under C, uuid, bytea): ord[code] puts the entries in the key's order, so that a
 * row is compared with the bar as two integers. exact: ord is 1 + the position of the entry in byte order (no two codes tie); else it is the first 8 bytes of the
 * entry, big endian, and entries that share them tie and are compared as bytes. A prefix is made once the rows compared with the dictionary as bytes reach
 * ORD_PREFIX_ROWS of its entries (making it costs as many compares: the table of ranks as well as the prefixes), and dropped for the dictionary when more than a quarter of the comparisons of a unit
 * tie. An exact code is made when the dictionary has its order (SiftSrcDict.order): stored, or sorted after the scans of a backend reached a few times its
 * entries. A dictionary of more than ORD_MAX_ENTRIES entries has none.
 */
#define ORD_CACHE		8
#define ORD_MAX_ENTRIES	(1 << 20)
#define ORD_PREFIX_ROWS	0.5
#define ORD_FORCE_ROWS	1000		/* rows per entry that make a dictionary sort at once (cdict_order) */

typedef struct OrdCode
{
	const void *id;
	const char *bytes;
	uint32		n;
	uint64		rows;			/* rows compared with the dictionary as bytes so far */
	uint64	   *ord;			/* NULL: not made */
	bool		exact;
	bool		useless;		/* a prefix that most rows tie on: the rows are compared as bytes */
} OrdCode;

/* The range of the key that a condition keeps: the ends that it bounds */
typedef struct GBox
{
	bool		empty;
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
} GBox;

struct SiftTopN
{
	int			limit;			/* rows the heap holds; 0: none wanted (a LIMIT that leaves nothing) */
	int			want;			/* TOPN_TIES: LIMIT + OFFSET, the rows that count; limit is more */
	bool		incomplete;		/* TOPN_TIES: the heap is full and its last row ties with the one that counts */
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
	int			nexpr;			/* keys that are sort expressions */
	MemoryContext ucxt;			/* the copies of the unit's entries that the rows of one unit hold (reset at the next unit) */
	MemoryContext ocxt;			/* the order codes of the dictionaries (unit_ord) */
	struct OrdCode *oc;			/* ORD_CACHE of them */
	int			oc_next;
	int64		nord[3];		/* units compared by an exact order code, by a prefix, as bytes */
	int64		nties;			/* comparisons of the unit that a prefix could not decide */
	int			nout;
	int		   *attno;
	int			nov;			/* output columns the units give (the heap holds no row to fetch: all of them are leaves), else 0 */
	int		   *oleaf;			/* ... their leaves, and the types their values are given as */
	Oid		   *otyp;
	bool	   *owide;			/* ... int64 (else int32) vectors */
	int		   *okey;			/* ... the sort key that is the same column (the entry holds its value), else -1 */
	int			nread;			/* ... columns that are not a key: read from the unit */
	int64	   *ov;				/* limit entries of nov values each, and whether each is NULL: the entries of heap take a place each (TopEnt.oi) */
	bool	   *ovn;
	int64	   *uv;				/* the values of the rows of lh */
	bool	   *uvn;
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
	uint32	   *rpass;			/* the rows of the window that pass, whose keys are read */
	uint32	   *rmrow;			/* lazy: those of them that the key does not decide, whose other columns are read */
	uint16	   *rslot;			/* the place of the key of each entry of the window in that read */
	uint16	   *rmay;			/* ... their entries of the window */
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
	const struct WalkBase *wb;	/* the bounds of the superblocks and their order (kept between queries when the source can tell what they depend on) */
	uint32	   *whead;			/* the superblocks that hold rows of a write buffer, which the walk visits first: their zones are not exact */
	int			nhead;
	int			hpos;
	uint32	   *wfull;			/* the whole order, for a parallel scan, when there is a head */
	uint32		wpos;
	Snapshot	snap;
	SiftSrcSource *src;
	Relation	rel;			/* the table the rows are fetched from: partitioned, or the heap */
	int			nfetch;			/* 1, or the partitions of a partitioned table: the fetches are made when a row of the partition is the first */
	TopFetch   *pf;				/* the heap row of a TID: the version an index scan finds, along a HOT chain */
	int64		skipped;
	int64		nrun;			/* units read through their sorted run */
	int64		nlazy;			/* ... of them with no filter run (sift_topn_lazy) */
	int64		nrhead;			/* units answered from the head of their sorted run */
	bool		wkeep;			/* the write buffers' superblocks are known (whead): a superblock outside them has no row in memory */
	int64		fetched;
	GBox		kbox;			/* the range of the first key that the conditions on it keep (sift_topn_key_range): a sorted run is read from its first entry in it */
	const SiftTopGPred *resid;	/* sift_topn_lazy: the conditions of the scan, which a unit read through its sorted run tests on the rows it takes (its filters are not run) */
	int			nresid;
	int			rleaf[TOP_RESID];	/* ... the leaves they read, and whether each is an int64 (else int32) vector */
	bool		rwide[TOP_RESID];
	bool		nogord;			/* TOPN_NOGORD: the plan counts on the heads of the sorted runs */
	bool		gtext;			/* ... the one key is a text column, ranked in the arrays by its collation (key[0].collid) */
	bool		gok;			/* every key is a plain integer or time column of the heap (col/gorder.h can order the first, the others are read from the heap row): gatt and gtyp are theirs */
	int			gatt[TOP_GKEYS];
	Oid			gtyp[TOP_GKEYS];
	struct GRow *gq;			/* sift_topn_gorder with more keys: the rows of the whole groups of the first key that make the n, in order */
	int			gqn;
	int			gqpos;
	const SiftTopGPred *gcond;	/* sift_topn_gorder: what the key of an entry must satisfy */
	struct SiftGOrd *go;		/* the global order of the key read in place of the units (sift_topn_gorder): rows come from the heap in the order of the key */
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

typedef struct EnumItem
{
	Oid			oid;
	float4		order;
	uint32		rank;
} EnumItem;

static int
enum_by_order(const void *a, const void *b)
{
	return SIFT_CMP3(((const EnumItem *) a)->order, ((const EnumItem *) b)->order);
}

static int
enum_by_oid(const void *a, const void *b)
{
	return SIFT_CMP3(((const EnumItem *) a)->oid, ((const EnumItem *) b)->oid);
}

/* The values of an enum in the order of the type (pg_enum.enumsortorder, which ALTER TYPE ... ADD VALUE BEFORE/AFTER changes without touching a row), read when the scan opens */
static void
enum_ranks(TopKey *k, Oid typid)
{
	CatCList   *list = SearchSysCacheList1(ENUMTYPOIDNAME, ObjectIdGetDatum(typid));
	EnumItem   *it = palloc(Max(list->n_members, 1) * sizeof(EnumItem));

	for (int i = 0; i < list->n_members; i++)
	{
		Form_pg_enum e = (Form_pg_enum) GETSTRUCT(&list->members[i]->tuple);

		it[i] = (EnumItem) {.oid = e->oid, .order = e->enumsortorder};
	}
	qsort(it, list->n_members, sizeof(EnumItem), enum_by_order);
	for (int i = 0; i < list->n_members; i++)
		it[i].rank = i;
	qsort(it, list->n_members, sizeof(EnumItem), enum_by_oid);
	k->nenum = list->n_members;
	k->eoid = palloc(Max(k->nenum, 1) * sizeof(Oid));
	k->erank = palloc(Max(k->nenum, 1) * sizeof(uint32));
	for (uint32 i = 0; i < k->nenum; i++)
	{
		k->eoid[i] = it[i].oid;
		k->erank[i] = it[i].rank;
	}
	pfree(it);
	ReleaseCatCacheList(list);
}

/* the heap attribute of a leaf that is a plain column of the table, 0 when it is not */
static int
leaf_var_attno(const SiftSrcLayout *lay, int leaf)
{
	const SiftSrcLeaf *lf = &lay->leaf[leaf];
	Node	   *e;

	if (lf->path != NIL || lf->parent >= 0 || lf->wide || lf->attno < 1 || lf->attno > list_length(lay->attexprs))
		return 0;
	e = list_nth(lay->attexprs, lf->attno - 1);
	return IsA(e, Var) && ((Var *) e)->varattno > 0 ? ((Var *) e)->varattno : 0;
}

int
sift_topn_leaf_attno(const SiftSrcLayout *lay, int leaf)
{
	switch (lay->leaf[leaf].typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return leaf_var_attno(lay, leaf);
		default:
			return 0;
	}
}

/* the integers compare as the numbers they are, whatever the width; a date or a time as its own count */
bool
sift_topn_same_scale(Oid a, Oid b)
{
	return a == b || ((a == INT2OID || a == INT4OID || a == INT8OID) && (b == INT2OID || b == INT4OID || b == INT8OID));
}

/* Can the global order of the first key stand for the scan, the others being read from the heap rows: plain integer and time columns of a heap */
static bool
order_gok(SiftTopN *t, const SiftSrcLayout *lay, Relation heap)
{
	if (t->nkey > TOP_GKEYS || lay->is_table || heap->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return false;
	for (int j = 0; j < t->nkey; j++)
	{
		const TopKey *k = &t->key[j];

		if (k->leaf < 0 || k->xk != TXK_NONE || k->x != NULL || k->nenum || (t->gatt[j] = k->blob ? leaf_var_attno(lay, k->leaf) : sift_topn_leaf_attno(lay, k->leaf)) == 0)
			return false;
		t->gtyp[j] = lay->leaf[k->leaf].typid;
		if (k->blob)
		{
			/* the one key of a text column (text, varchar): its strings are ranked in the arrays under the collation of the query's order */
			if (t->nkey != 1 || k->bkind != BK_TEXT || !OidIsValid(k->collid) || (t->gtyp[0] != TEXTOID && t->gtyp[0] != VARCHAROID))
				return false;
			t->gtext = true;
		}
	}
	return true;
}

SiftTopN *
sift_topn_open(Node *spec, const SiftSrcLayout *lay, SiftSrcSource *src, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt, int widen)
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

		*k = (TopKey) {.leaf = leaf, .desc = fl & 1, .nulls_first = (fl & 2) != 0, .vtype = leaf >= 0 ? lay->leaf[leaf].vtype : expr_vtype(xcoll),
			.xk = xk, .xc = (int64) (((uint64) xhi << 32) | xlo), .xcoll = xcoll};
		if (leaf >= 0 && xk == TXK_NONE && type_is_enum(lay->leaf[leaf].typid))
			enum_ranks(k, lay->leaf[leaf].typid);
		if (xk == TXK_EXPR)
		{
			k->x = expr_open(list_nth(SPEC_EXPRS(spec), (int) xlo), lay, xcoll, econtext, cxt);
			t->nexpr++;
		}
		if (xk == TXK_LOWER || xk == TXK_UPPER)
		{
			k->xfn = palloc0(sizeof(FmgrInfo));
			fmgr_info(xk == TXK_LOWER ? F_LOWER_TEXT : F_UPPER_TEXT, k->xfn);
		}
		if (k->vtype == SCV_CODE)
		{
			k->blob = true;
			k->bkind = cmp == F_BTTEXTCMP ? BK_TEXT : cmp == F_UUID_CMP ? BK_UUID : cmp == F_BYTEACMP ? BK_BYTEA : BK_FMGR;
			k->typid = k->bkind == BK_TEXT ? TEXTOID : leaf >= 0 ? lay->leaf[leaf].typid : xcoll;
			k->sortop = sortop;
			k->collid = collid;
			if (k->bkind == BK_FMGR)
			{
				k->cmp = palloc0(sizeof(FmgrInfo));
				fmgr_info(cmp, k->cmp);
				k->typlen = get_typlen(k->typid);
			}
			t->nblob++;
		}
	}
	if (t->nblob > 0 || t->nexpr > 0)
		t->ucxt = AllocSetContextCreate(cxt, "sift top-N unit", ALLOCSET_DEFAULT_SIZES);
	if (t->nblob > 0)
		t->oc = palloc0(ORD_CACHE * sizeof(OrdCode));
	/* the zones of the next keys tell, for a first key that ties, which superblocks can still hold a row before the n-th: they bound the walk in the order of the keys */
	for (t->wkeys = 1; sift_topn_zone_keys && t->wkeys < t->nkey && t->key[t->wkeys].leaf >= 0 && (t->key[t->wkeys].vtype == SCV_I32 || t->key[t->wkeys].vtype == SCV_I64); t->wkeys++)
		;
	t->nout = list_nth_int(l, i++);
	t->attno = palloc(Max(t->nout, 1) * sizeof(int));
	t->oleaf = palloc(Max(t->nout, 1) * sizeof(int));
	for (int c = 0; c < t->nout; c++)
	{
		t->attno[c] = list_nth_int(l, i++);
		t->oleaf[c] = list_nth_int(l, i++);
	}
	bounded = bound_rows(spec, econtext, parent, &t->limit, &offset);
	/* a heap entry holds SIFT_TOPN_MAX_KEYS keys: more keys sort every row, as no bound does */
	flags = list_nth_int(l, i++);
	if ((flags & TOPN_SORT) || (t->nkey > sift_limit(topn_keys, SIFT_TOPN_MAX_KEYS) && t->limit > 0))
		bounded = false;
	if (bounded && t->limit > 0 && (flags & TOPN_TIES))
	{
		int64		n = (int64) (t->limit + (flags >> TOPN_SLACK_SHIFT)) << (4 * widen);

		t->want = t->limit;
		if (widen < TOPN_WIDEN_MAX && n <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS))
			t->limit = (int) n;
		else
			bounded = false;
	}
	if (flags & TOPN_SKIP_OFFSET)
		t->skip = offset;
	t->all = !bounded;
	t->totals = (flags & TOPN_TOTALS) != 0;
	t->nogord = (flags & TOPN_NOGORD) != 0;
	if (flags & TOPN_RAWKEY)
		sift_colsrc_set_raw(src, t->key[0].leaf);
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
		/* every column of the output is a leaf (the plan says so): the units give them, a heap row is fetched only for the entries they could not */
		if (t->nout > 0 && t->oleaf[0] >= 0 && heap->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			t->nov = t->nout;
			t->otyp = palloc(t->nov * sizeof(Oid));
			t->owide = palloc(t->nov * sizeof(bool));
			t->okey = palloc(t->nov * sizeof(int));
			for (int c = 0; c < t->nov; c++)
			{
				t->otyp[c] = lay->leaf[t->oleaf[c]].typid;
				t->owide[c] = lay->leaf[t->oleaf[c]].vtype == SCV_I64;
				t->okey[c] = -1;
				for (int j = 0; j < t->nkey && t->okey[c] < 0; j++)
				{
					const TopKey *k = &t->key[j];

					if (k->leaf == t->oleaf[c] && k->xk == TXK_NONE && k->x == NULL && !k->blob && !k->nenum && (k->vtype == SCV_I32 || k->vtype == SCV_I64))
						t->okey[c] = j;
				}
				t->nread += t->okey[c] < 0;
			}
			t->ov = palloc((Size) t->limit * t->nov * sizeof(int64));
			t->ovn = palloc((Size) t->limit * t->nov);
			t->uv = palloc((Size) t->limit * t->nov * sizeof(int64));
			t->uvn = palloc((Size) t->limit * t->nov);
		}
	}
	t->gok = order_gok(t, lay, heap);
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
	if (t->ocxt)
		MemoryContextDelete(t->ocxt);
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

#define ST_SORT sort_ents
#define ST_ELEMENT_TYPE TopEnt
#define ST_COMPARE_ARG_TYPE SiftTopN
#define ST_COMPARE(a, b, t) ent_cmp(t, a, b)
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

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

/* the position of the enum value oid in the order of its type */
static inline int64
enum_rank(const TopKey *k, Oid oid)
{
	uint32		lo = 0,
				hi = k->nenum;

	SIFT_BISECT(uint32, mid, lo, hi, k->eoid[mid] < oid);
	if (lo == k->nenum || k->eoid[lo] != oid)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("enum value %u was added while the scan was running", oid)));
	return k->erank[lo];
}

/* A float's place in the sort order as an integer (the order of float8: -0 is +0, NaN is the largest) and back (any NaN key reads as a NaN) */
static inline int64
f8_key(double d)
{
	int64		b;

	if (isnan(d))
		return PG_INT64_MAX;
	d = d == 0 ? 0.0 : d;
	memcpy(&b, &d, sizeof(b));
	return b < 0 ? b ^ PG_INT64_MAX : b;
}

static inline double
f8_of_key(int64 k)
{
	double		d;

	k = k < 0 ? k ^ PG_INT64_MAX : k;
	memcpy(&d, &k, sizeof(d));
	return d;
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
			*out = k->nenum ? enum_rank(k, ((const int32 *) v->data)[i]) : ((const int32 *) v->data)[i];
			break;
		case SCV_I64:
			*out = ((const int64 *) v->data)[i];
			break;
		default:
			*out = f8_key(((const double *) v->data)[i]);
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
	if (k->bkind == BK_BYTEA || (k->bkind == BK_FMGR && k->typlen < 0))
	{
		varattrib_4b h;

		memcpy(&h, *p, sizeof(uint32));
		*len = VARSIZE_4B(&h);
	}
	else if (k->bkind == BK_FMGR)
		*len = k->typlen;
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

/* ---- sort expressions (TXK_EXPR) ---- */

/* The vector a key of this result type is kept in: the integer types by their width, floats widened, anything else as byte strings of a dictionary */
static SiftSrcVecType
expr_vtype(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return SCV_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return SCV_F64;
		default:
			return SCV_CODE;
	}
}

/* xs: the expression over Vars numbered 1.. and the leaves they stand for, in that order */
static TopExpr *
expr_open(List *xs, const SiftSrcLayout *lay, Oid rtype, ExprContext *outer, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	TopExpr    *x = palloc0(sizeof(TopExpr));
	List	   *leaves = lsecond(xs);
	TupleDesc	desc;

	x->nin = list_length(leaves);
	x->in = palloc0(Max(x->nin, 1) * sizeof(TopIn));
	desc = CreateTemplateTupleDesc(Max(x->nin, 1));
	for (int a = 0; a < x->nin; a++)
	{
		const SiftSrcLeaf *lf = &lay->leaf[list_nth_int(leaves, a)];

		x->in[a] = (TopIn) {.leaf = lf->id, .typid = lf->typid, .vtype = lf->vtype, .scale = lf->scale};
		TupleDescInitEntry(desc, a + 1, NULL, lf->typid, -1, 0);
	}
	sift_tupdesc_finalize(desc);
	x->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	x->econtext = CreateStandaloneExprContext();
	x->econtext->ecxt_scantuple = x->slot;
	/* the parameters of the statement (the leader only: the planner does not give a sort expression with one to workers) */
	x->econtext->ecxt_param_list_info = outer->ecxt_param_list_info;
	/* no parent: the Vars are of the slot above whatever the scan's own tuple is */
	x->state = ExecInitExpr((Expr *) linitial(xs), NULL);
	x->rtype = rtype;
	MemoryContextSwitchTo(old);
	return x;
}

/* The datum of the entry c of the string dictionary of an input, built once for the unit */
static Datum
in_entry(SiftTopN *t, TopIn *in, uint32 c)
{
	const SiftSrcDict *d = in->v->dict;
	const char *p = d->bytes + d->off[c];
	uint32		len = d->off[c + 1] - d->off[c];
	char	   *r;

	if (in->built[c])
		return in->memo[c];
	switch (in->typid)
	{
		case TEXTOID:
		case VARCHAROID:
			r = MemoryContextAlloc(t->ucxt, VARHDRSZ + len);
			SET_VARSIZE(r, VARHDRSZ + len);
			memcpy(VARDATA(r), p, len);
			break;
		case UUIDOID:
			r = MemoryContextAlloc(t->ucxt, len);
			memcpy(r, p, len);
			break;
		default:				/* bytea, inet: the image of the datum, its header says the length */
			{
				varattrib_4b h;

				memcpy(&h, p, sizeof(uint32));
				len = VARSIZE_4B(&h);
				r = MemoryContextAlloc(t->ucxt, len);
				memcpy(r, p, len);
			}
	}
	in->built[c] = true;
	return in->memo[c] = PointerGetDatum(r);
}

static Datum
in_datum(SiftTopN *t, TopIn *in, int i, bool *isnull)
{
	const SiftSrcVec *v = in->v;

	*isnull = v->isnull && v->isnull[i];
	if (*isnull)
		return (Datum) 0;
	switch (in->vtype)
	{
		case SCV_I32:
			return Int32GetDatum(((const int32 *) v->data)[i]);
		case SCV_I64:
			if (in->typid != NUMERICOID)
				return Int64GetDatum(((const int64 *) v->data)[i]);
			if (v->ovf && ((v->ovf[i / 64] >> (i % 64)) & 1))
				return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
			return NumericGetDatum(int64_div_fast_to_numeric(((const int64 *) v->data)[i], in->scale));
		case SCV_F64:
			return in->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[i]) : Float8GetDatum(((const double *) v->data)[i]);
		default:
			return in_entry(t, in, ((const uint32 *) v->data)[i]);
	}
}

/*
 * The key j of every row of the unit that passes, from PostgreSQL's evaluation of its expression over those rows' leaves: one result for each
 * (the normal plan computes the sort expression of every row that passes the WHERE, and raises its errors for them). The result is the vector a
 * key of the leaf of that type would be, and the rest of the scan reads it as such.
 */
static void
expr_eval_unit(SiftTopN *t, TopKey *k, SiftTopLeaf leaf, void *ctx, const uint64 *rmask, int nrow)
{
	TopExpr    *x = k->x;
	int			nw = (nrow + 63) / 64,
				npass = 0,
				at = 0,
				esz = k->vtype == SCV_I32 ? sizeof(int32) : k->vtype == SCV_CODE ? sizeof(uint32) : sizeof(int64);
	uint8	   *nul = MemoryContextAllocZero(t->ucxt, Max(nrow, 1));
	char	   *data = MemoryContextAllocZero(t->ucxt, Max(nrow, 1) * (Size) esz);
	SiftSrcDict *dict = NULL;
	uint32	   *off = NULL;
	StringInfoData bytes = {0};
	MemoryContext tup = x->econtext->ecxt_per_tuple_memory;

	for (int w = 0; w < nw; w++)
		npass += pg_popcount64(rmask[w]);
	for (int a = 0; a < x->nin; a++)
	{
		TopIn	   *in = &x->in[a];

		in->v = leaf(ctx, in->leaf);
		if (in->vtype == SCV_CODE)
		{
			in->memo = MemoryContextAllocZero(t->ucxt, Max(in->v->dict->n, 1) * sizeof(Datum));
			in->built = MemoryContextAllocZero(t->ucxt, Max(in->v->dict->n, 1));
		}
	}
	if (k->vtype == SCV_CODE)
	{
		MemoryContext old = MemoryContextSwitchTo(t->ucxt);

		dict = palloc0(sizeof(SiftSrcDict));
		off = palloc((npass + 1) * sizeof(uint32));
		initStringInfo(&bytes);
		off[0] = 0;
		MemoryContextSwitchTo(old);
	}
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			bool		isnull;
			Datum		d;
			MemoryContext old = MemoryContextSwitchTo(tup);

			ExecClearTuple(x->slot);
			for (int a = 0; a < x->nin; a++)
				x->slot->tts_values[a] = in_datum(t, &x->in[a], i, &x->slot->tts_isnull[a]);
			ExecStoreVirtualTuple(x->slot);
			MemoryContextSwitchTo(old);
			d = ExecEvalExprSwitchContext(x->state, x->econtext, &isnull);
			if (isnull)
				nul[i] = 1;
			switch (k->vtype)
			{
				case SCV_I32:
					((int32 *) data)[i] = x->rtype == INT2OID ? DatumGetInt16(d) : x->rtype == BOOLOID ? DatumGetBool(d) : DatumGetInt32(d);
					break;
				case SCV_I64:
					((int64 *) data)[i] = DatumGetInt64(d);
					break;
				case SCV_F64:
					((double *) data)[i] = x->rtype == FLOAT4OID ? DatumGetFloat4(d) : DatumGetFloat8(d);
					break;
				default:
					if (!isnull)
					{
						MemoryContext o2 = MemoryContextSwitchTo(t->ucxt);

						if (k->bkind == BK_UUID)
							appendBinaryStringInfo(&bytes, DatumGetPointer(d), UUID_LEN);
						else
						{
							struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
							uint32		len = VARSIZE_ANY_EXHDR(v);

							if (k->bkind == BK_TEXT)
								appendBinaryStringInfo(&bytes, VARDATA_ANY(v), len);
							else
							{
								/* the image of the datum, with the four byte header the entries of a dictionary have */
								varattrib_4b hdr;

								SET_VARSIZE(&hdr, VARHDRSZ + len);
								appendBinaryStringInfo(&bytes, (char *) &hdr, VARHDRSZ);
								appendBinaryStringInfo(&bytes, VARDATA_ANY(v), len);
							}
						}
						MemoryContextSwitchTo(o2);
					}
					((uint32 *) data)[i] = at;
					off[++at] = bytes.len;
			}
			ResetExprContext(x->econtext);
			if ((i & 4095) == 4095)
				CHECK_FOR_INTERRUPTS();
		}
	k->xv = (SiftSrcVec) {.data = data, .isnull = nul, .n = nrow};
	if (dict)
	{
		*dict = (SiftSrcDict) {.id = dict, .n = at, .off = off, .bytes = bytes.data};
		k->xv.dict = dict;
	}
}

/* The key j of the rows t->cand[0..n): read from the leaf, or taken from the vector of the expression */
static void
key_rows(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int j, int n, SiftSrcVec *v)
{
	const TopKey *k = &t->key[j];
	int			esz = k->vtype == SCV_I32 ? sizeof(int32) : k->vtype == SCV_CODE ? sizeof(uint32) : sizeof(int64);
	char	   *data;
	uint8	   *nul;

	if (k->x == NULL)
	{
		sift_colsrc_read_rows(src, unit, k->leaf, t->cand, n, v);
		return;
	}
	data = MemoryContextAlloc(t->ucxt, Max(n, 1) * (Size) esz);
	nul = MemoryContextAlloc(t->ucxt, Max(n, 1));
	for (int x = 0; x < n; x++)
	{
		memcpy(data + (Size) x * esz, (const char *) k->xv.data + (Size) t->cand[x] * esz, esz);
		nul[x] = k->xv.isnull[t->cand[x]];
	}
	*v = (SiftSrcVec) {.data = data, .isnull = nul, .dict = k->xv.dict, .n = n};
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

/* the place of the entry's output columns and the columns themselves (v: nov values and flags; NULL: the row is fetched from the heap) */
static inline void
res_values(SiftTopN *t, TopEnt *c, uint32 oi, const int64 *v, const bool *vn)
{
	c->oi = oi;
	c->hasv = v != NULL;
	if (v != NULL)
	{
		memcpy(&t->ov[(Size) oi * t->nov], v, t->nov * sizeof(int64));
		memcpy(&t->ovn[(Size) oi * t->nov], vn, t->nov * sizeof(bool));
	}
}

/* An entry takes the place of the one it pushes out, or the next place while the heap fills */
static void
res_push(SiftTopN *t, const TopEnt *e, const int64 *v, const bool *vn)
{
	TopEnt		c = *e,
				old;

	if (t->nblob == 0)
	{
		/* the entries are kept as they come until there are n of them, then made a heap: the fill costs one comparison an entry less log n */
		if (t->n < t->limit)
		{
			res_values(t, &c, t->n, v, vn);
			t->heap[t->n++] = c;
			if (t->n == t->limit)
				for (int i = t->n / 2 - 1; i >= 0; i--)
					heap_down(t, t->heap, t->n, i);
			return;
		}
		if (ent_cmp(t, e, &t->heap[0]) >= 0)
			return;
		res_values(t, &c, t->heap[0].oi, v, vn);
		t->heap[0] = c;
		heap_down(t, t->heap, t->n, 0);
		return;
	}
	if (t->n == t->limit)
	{
		if (ent_cmp(t, e, &t->heap[0]) >= 0)
			return;
		old = t->heap[0];
		res_own(t, &c);
		res_values(t, &c, old.oi, v, vn);
		heap_push(t, t->heap, &t->n, &c);
		res_free(t, &old);
		return;
	}
	res_own(t, &c);
	res_values(t, &c, t->n, v, vn);
	heap_push(t, t->heap, &t->n, &c);
}

#define ST_SORT sort_rows
#define ST_ELEMENT_TYPE uint32
#define ST_COMPARE(a, b) SIFT_CMP3(*(a), *(b))
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

/* the place of row in the ascending rows[0, n), which holds it */
static inline uint32
row_at(const uint32 *rows, int n, uint32 row)
{
	int			lo = 0,
				hi = n;

	SIFT_BISECT(int, mid, lo, hi, rows[mid] < row);
	return lo;
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

		key_rows(t, src, unit, j, nc, &v);
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

/* The output columns of the rows of lh: the value of a sort key is in the entry, the others are read from the unit. The values of row i are uv[i * nov ...] */
static void
read_outputs(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit)
{
	for (int c = 0; c < t->nov; c++)
	{
		SiftSrcVec	v;
		int			j = t->okey[c];

		if (j >= 0)
			for (int i = 0; i < t->ln; i++)
			{
				t->uvn[(Size) i * t->nov + c] = (t->lh[i].nul >> j) & 1;
				t->uv[(Size) i * t->nov + c] = t->lh[i].k[j];
			}
		else
		{
			sift_colsrc_read_rows(src, unit, t->oleaf[c], t->rows, t->ln, &v);
			for (int i = 0; i < t->ln; i++)
			{
				uint32		x = row_at(t->rows, t->ln, t->lh[i].row);
				Size		at = (Size) i * t->nov + c;

				t->uvn[at] = v.isnull && v.isnull[x];
				t->uv[at] = t->uvn[at] ? 0 : t->owide[c] ? ((const int64 *) v.data)[x] : ((const int32 *) v.data)[x];
			}
		}
	}
}

/* The rows of lh that the heap would take as it is now: those whose output the unit has to give */
static int
out_rows(const SiftTopN *t)
{
	int			n = 0;

	if (t->n < t->limit)
		return t->ln;
	for (int i = 0; i < t->ln; i++)
		n += ent_cmp(t, &t->lh[i], &t->heap[0]) < 0;
	return n;
}

/* The best n of those rows by all the keys go to the heap with their TIDs */
static void
finish_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc)
{
	uint16		part;
	bool		vals;

	t->ln = 0;
	read_other_keys(t, src, unit, nc);
	if (nc <= t->limit)
	{
		/* the unit has no more rows than the heap holds: all of them go on */
		memcpy(t->lh, t->ce, nc * sizeof(TopEnt));
		t->ln = nc;
	}
	else
		for (int x = 0; x < nc; x++)
			heap_push(t, t->lh, &t->ln, &t->ce[x]);
	if (t->ln == 0)
		return;
	for (int i = 0; i < t->ln; i++)
		t->rows[i] = t->lh[i].row;
	sort_rows(t->rows, t->ln);
	sift_colsrc_tids(src, unit, t->rows, t->ln, t->tids);
	part = unit_part(t, src);
	for (int i = 0; i < t->ln; i++)
	{
		TopEnt	   *e = &t->lh[i];

		e->tid = t->tids[row_at(t->rows, t->ln, e->row)];
		e->part = part;
		e->tie = tie_key(&e->tid, part);
	}
	vals = t->nov > 0 && (t->nread == 0 || out_rows(t) >= Max(TOPN_OUT_ROWS, t->limit / 2));
	if (vals)
		read_outputs(t, src, unit);
	for (int i = 0; i < t->ln; i++)
		res_push(t, &t->lh[i], vals ? &t->uv[(Size) i * t->nov] : NULL, vals ? &t->uvn[(Size) i * t->nov] : NULL);
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

		key_rows(t, src, unit, j, nc, &v);
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

/* does a value of the column pass the comparison: the one test of a heap row (gcond_test) and of a row of a unit (resid_test) */
static inline bool
gcond_cmp(int op, int64 key, int64 val)
{
	switch (op)
	{
		case SCMP_EQ:
			return key == val;
		case SCMP_NE:
			return key != val;
		case SCMP_LT:
			return key < val;
		case SCMP_LE:
			return key <= val;
		case SCMP_GT:
			return key > val;
		default:
			return key >= val;
	}
}

/* Entries of a sorted run are read in windows that double up to this many */
#define RUN_WIN_MIN		32
#define RUN_WIN_MAX		512
/* the candidate buffers of a unit read through its run start this size and grow with the rows taken (the rows of the unit are most of them never candidates) */
#define RUN_CAND_MIN	64

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
	const SiftSrcVec *key0;		/* the first key of every row, or NULL: the keys of the window's rows are read by position (wkey) */
	SiftSrcVec	wkey;
	const uint64 *rmask;
	uint32		n;				/* rows of the unit: entries of the run */
	uint32		first,
				end;			/* the entries run_values reads: the keys of the others are not read */
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
	bool		lazy;			/* rmask holds the live rows, not the ones that pass: the window's rows are tested by t->resid (rslot is RSLOT_OUT for those that fail) */
	SiftSrcVec	rv[TOP_RESID];	/* lazy: the leaves of t->resid for the rows of the window that rmask keeps */
	uint32		nfail;			/* lazy: the rows that failed t->resid; abort: so many that the filters of the unit are cheaper */
	bool		abort;
} RunWalk;

/* a lazy walk that finds more than a 32nd of the rows of the unit failing the conditions is left to the filters, which test a row for a few instructions */
#define LAZY_FAIL_DIV	32

#define RSLOT_OUT	0xFFFF

static void
run_corrupt(RunWalk *w, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift_col sorted run is corrupted"),
			 errdetail("The sorted run of the first sort column of a unit of %u rows %s.", w->n, what)));
}

static inline bool
run_pass(const RunWalk *w, uint32 row)
{
	return w->rmask[row >> 6] >> (row & 63) & 1;
}

/* the value of a leaf of a condition at a row of the window: ki its place in the read of the key, oi in that of the other leaves; false for NULL */
static bool
resid_value(const RunWalk *w, int leaf, int ki, int oi, int64 *out)
{
	const SiftTopN *t = w->t;
	int			j = 0,
				i = leaf == t->key[0].leaf ? ki : oi;

	while (t->rleaf[j] != leaf)
		j++;
	if (w->rv[j].isnull && w->rv[j].isnull[i])
		return false;
	*out = t->rwide[j] ? ((const int64 *) w->rv[j].data)[i] : ((const int32 *) w->rv[j].data)[i];
	return true;
}

#define RESID_MAYBE	2

/* Does the row pass the conditions: a condition on a column other than the key is not known (RESID_MAYBE) until that column is read (oi >= 0) */
static int
resid_test(const RunWalk *w, const SiftTopGPred *p, int ki, int oi)
{
	bool		unknown = false;
	int64		v;

	if (p->kind == STG_NOTNULL || p->kind == STG_CMP)
	{
		if (oi < 0 && p->leaf != w->t->key[0].leaf)
			return RESID_MAYBE;
		return resid_value(w, p->leaf, ki, oi, &v) && (p->kind == STG_NOTNULL || gcond_cmp(p->op, v, p->val));
	}
	for (int c = 0; c < p->nchild; c++)
	{
		int			r = resid_test(w, p->child[c], ki, oi);

		if (r == (p->kind == STG_OR))
			return r;
		unknown |= r == RESID_MAYBE;
	}
	return unknown ? RESID_MAYBE : p->kind == STG_AND;
}

/* The rows of the window that fail the conditions leave it (rslot): the key decides most, and the other columns are read for the rows it does not */
static void
run_resid(RunWalk *w, uint32 cnt)
{
	SiftTopN   *t = w->t;
	int			k = 0;

	for (int j = 0; j < t->nresid; j++)
		if (t->rleaf[j] == t->key[0].leaf)
			w->rv[j] = w->wkey;
	for (uint32 i = 0; i < cnt; i++)
	{
		int			r;

		if (t->rslot[i] == RSLOT_OUT)
			continue;
		r = resid_test(w, t->resid, t->rslot[i], -1);
		if (r == RESID_MAYBE)
		{
			t->rmrow[k] = t->rpass[t->rslot[i]];
			t->rmay[k++] = i;
		}
		else if (!r)
		{
			t->rslot[i] = RSLOT_OUT;
			w->nfail++;
		}
	}
	if (k > 0)
	{
		for (int j = 0; j < t->nresid; j++)
			if (t->rleaf[j] != t->key[0].leaf)
				sift_colsrc_read_rows(w->src, w->unit, t->rleaf[j], t->rmrow, k, &w->rv[j]);
		for (int q = 0; q < k; q++)
			if (!resid_test(w, t->resid, t->rslot[t->rmay[q]], q))
			{
				t->rslot[t->rmay[q]] = RSLOT_OUT;
				w->nfail++;
			}
	}
	w->abort = w->nfail > Max(w->n / LAZY_FAIL_DIV, 64);
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
		if (w->key0 == NULL)
		{
			/* a key is read for a row that passes only: the walk skips the others, and reading one by position costs ten times what decoding it with its unit does */
			int			m = 0;

			for (uint32 i = 0; i < cnt; i++)
			{
				w->t->rslot[i] = lo + i >= w->first && lo + i < w->end && run_pass(w, w->t->rwin[i]) ? m : RSLOT_OUT;
				if (w->t->rslot[i] != RSLOT_OUT)
					w->t->rpass[m++] = w->t->rwin[i];
			}
			if (m > 0)
				sift_colsrc_read_rows(w->src, w->unit, w->t->key[0].leaf, w->t->rpass, m, &w->wkey);
			if (w->lazy && m > 0)
				run_resid(w, cnt);
		}
		w->lo = lo;
		w->hi = lo + cnt;
		w->win = Min(w->win * 2, RUN_WIN_MAX);
	}
	return w->t->rwin[pos - w->lo];
}

static void
run_key(RunWalk *w, uint32 pos, uint32 row, int64 *v)
{
	if (!(w->key0 ? row_key(&w->t->key[0], w->key0, row, v) : row_key(&w->t->key[0], &w->wkey, w->t->rslot[pos - w->lo], v)))
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

static inline void
run_room(RunWalk *w)
{
	if (w->nc >= w->t->ccap)
		grow_unit_buffers(w->t, w->nc + 1);
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
		run_room(w);
		w->t->cand[w->nc++] = row;
		if (run_full(w, g0))
			break;
	}
	run_close(w, g0, r, false);
}

#define RUN_SEEK_TAIL	16

/* The keys of the entries [pos, pos + cnt) of the run (positions among the values): from key0 when the unit has it whole, which a read of rows would overwrite */
static void
run_keys_of(RunWalk *w, uint32 pos, uint32 cnt, int64 *keys)
{
	const TopKey *k0 = &w->t->key[0];
	uint32		rows[RUN_SEEK_TAIL];
	SiftSrcVec	v = {0};

	sift_colsrc_run_read(w->src, w->unit, k0->leaf, pos, cnt, rows);
	if (!w->key0)
		sift_colsrc_read_rows(w->src, w->unit, k0->leaf, rows, cnt, &v);
	for (uint32 i = 0; i < cnt; i++)
		if (!(w->key0 ? row_key(k0, w->key0, rows[i], &keys[i]) : row_key(k0, &v, i, &keys[i])))
			run_corrupt(w, "has a NULL row among those with a value");
}

/*
 * The first position of the run [0, nvals) whose key is after bound (at bound itself when incl). A key costs the opening of its chunk, and the zone of the unit
 * bounds the keys: the ends answer outright, and the next probe goes where the keys, spread evenly between the ends known so far, would put the bound; a probe
 * that does not halve what is left is followed by one at the middle, so that keys that are not spread evenly cost twice a bisection and not more. The last entries
 * are read together.
 */
static uint32
run_seek(RunWalk *w, uint32 nvals, int64 bound, bool incl)
{
	SiftSrcZone z;
	uint32		lo = 0,
				hi = nvals;
	int64		kl = 0,
				kh = 0;
	bool		even,
				mid = false;

	sift_colsrc_zone(w->src, w->unit, w->t->key[0].leaf, &z);
	even = !z.inexact && z.nvals == nvals;
	if (even)
	{
		if (incl ? z.min > bound : z.min >= bound)
			return 0;
		if (!(incl ? z.max > bound : z.max >= bound))
			return nvals;
		kl = z.min;
		kh = z.max;
	}
	while (lo < hi)
	{
		uint32		left = hi - lo,
					m = lo + left / 2;
		int64		keys[RUN_SEEK_TAIL];

		if (left <= RUN_SEEK_TAIL)
		{
			run_keys_of(w, lo, left, keys);
			for (m = 0; m < left; m++)
				if (incl ? keys[m] > bound : keys[m] >= bound)
					break;
			return lo + m;
		}
		if (even && !mid && kh > kl)
			m = lo + (uint32) ((double) (left - 1) * Min(Max(((double) bound - (double) kl) / ((double) kh - (double) kl), 0.0), 1.0));
		run_keys_of(w, m, 1, keys);
		if (incl ? keys[0] > bound : keys[0] >= bound)
		{
			hi = m;
			kh = keys[0];
		}
		else
		{
			lo = m + 1;
			kl = keys[0];
		}
		mid = (hi - lo) * 2 > left;
	}
	return lo;
}

/*
 * The rows with a value, best group first, until a group cannot be in the result. A row is tested against the mask before its key is read, so the
 * keys of the rows that do not pass (zero in a vector read for the rows that pass only) are never looked at; a group is the rows that pass with an
 * equal key. The run holds equal keys by row ascending: a descending walk meets them backwards and puts them right at the end of the group.
 */
static void
run_values(RunWalk *w, uint32 first, uint32 end)
{
	const TopKey *k0 = &w->t->key[0];
	bool		desc = k0->desc,
				open = false,
				skip = false;
	int			g0 = 0;
	int64		gv = 0;
	uint32		prev = 0;
	Rank		r = {0};

	for (uint32 i = first; i < end; i++)
	{
		uint32		pos = desc ? end - 1 - (i - first) : i,
					row = run_at(w, pos, desc);
		int64		v;

		if (w->abort)
			return;
		if (!run_pass(w, row) || (w->lazy && w->t->rslot[pos - w->lo] == RSLOT_OUT))
			continue;
		run_key(w, pos, row, &v);
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
		run_room(w);
		w->t->cand[w->nc++] = row;
		if (!desc && run_full(w, g0))
			break;
	}
	if (open && !skip)
		run_close(w, g0, r, desc);
}

/* Has the unit a run the walk can read: the run orders the column, not the expression of it (coalesce moves the NULLs, lower() folds the strings) */
static bool
run_ready(const SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	const TopKey *k0 = &t->key[0];

	return sift_topn_runs && k0->xk == TXK_NONE && k0->leaf >= 0 && sift_colsrc_run_info(src, unit, k0->leaf, true, nvals, nnull, id);
}

/*
 * The unit from its sorted run: the groups of equal first keys in the order of the sort, from the best, until one cannot be in the
 * result, where the rows are read and tested one by one otherwise. The candidates are what the other loop finds: the rows that pass whose first key is
 * not after the n-th of the unit's or the heap's, so the rest is the same. False: the unit has no run, or the rows that pass are too few for the walk
 * to pay (it reads about n / share of the rows that pass) and the heap has no bar yet.
 */
static bool
run_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow, bool lazy)
{
	RunWalk		w = {.t = t, .src = src, .unit = unit, .key0 = key0, .rmask = rmask, .n = nrow, .end = nrow, .win = RUN_WIN_MIN, .lazy = lazy};
	uint32		nvals,
				nnull,
				first,
				end,
				npass = 0;
	uint64		id;
	int			m = 0;
	bool		bounded;
	const TopKey *k0 = &t->key[0];
	RunTrunc   *rt = NULL;
	SiftSrcVec	cv = {0};

	if (!run_ready(t, src, unit, &nvals, &nnull, &id))
		return false;
	if (t->revisit)
	{
		/* the units the first pass finished, or took no run of, are not read again */
		for (int i = 0; i < t->ntrunc && rt == NULL; i++)
			if (t->trunc[i].id == id && !t->trunc[i].done)
				rt = &t->trunc[i];
		if (rt == NULL)
			return true;
		w.resumed = true;
		w.resume = rt->next;
	}
	else
	{
		for (int i = 0; i < (nrow + 63) / 64 && !lazy; i++)
			npass += pg_popcount64(rmask[i]);
		/* a unit with fewer rows than the walk would take for its own bar is read through the run only when the heap's bar already cuts it (a lazy unit is not counted: the rows that pass are what the walk finds) */
		if (!lazy && (uint64) (t->limit + 1) * 2 > npass && !bar_rank(t, &w.bar))
			return false;
		w.budget = t->budget;
	}
	grow_unit_buffers(t, Min(nrow, RUN_CAND_MIN));
	if (t->rwin == NULL)
	{
		t->rwin = MemoryContextAlloc(GetMemoryChunkContext(t), RUN_WIN_MAX * (3 * sizeof(uint32) + 2 * sizeof(uint16)));
		t->rpass = t->rwin + RUN_WIN_MAX;
		t->rmrow = t->rpass + RUN_WIN_MAX;
		t->rslot = (uint16 *) (t->rmrow + RUN_WIN_MAX);
		t->rmay = t->rslot + RUN_WIN_MAX;
	}
	w.barred = bar_rank(t, &w.bar);
	if (rt && run_stop(&w, rt->next))
	{
		rt->done = true;
		return true;
	}
	first = 0;
	end = nvals;
	/* the conditions on the key bound the part of the run that can pass: the rows outside it, and the NULLs, fail them */
	bounded = t->kbox.has_lo || t->kbox.has_hi;
	if (bounded)
	{
		first = t->kbox.has_lo ? run_seek(&w, nvals, t->kbox.lo, false) : 0;
		end = t->kbox.has_hi ? run_seek(&w, nvals, t->kbox.hi, true) : nvals;
	}
	if (k0->nulls_first && !bounded)
		run_nulls(&w, nvals, nnull);
	w.first = first;
	w.end = end;
	run_values(&w, first, end);
	if (w.abort)
		return false;
	if (rt)
		rt->done = true;
	if (!k0->nulls_first && !w.trunc && !bounded)
		run_nulls(&w, nvals, nnull);
	/* the rows whose value overflowed the leaf are tested one by one: the run does not order them */
	for (uint32 pos = nvals + nnull; pos < (uint32) nrow && !w.resumed; pos++)
	{
		uint32		row = run_at(&w, pos, false);

		if (rmask[row >> 6] >> (row & 63) & 1)
		{
			run_room(&w);
			t->cand[w.nc++] = row;
		}
	}
	sort_rows(t->cand, w.nc);
	if (key0 == NULL && w.nc > 0)
		sift_colsrc_read_rows(src, unit, k0->leaf, t->cand, w.nc, &cv);
	for (int q = 0; q < w.nc; q++)
	{
		TopEnt	   *e = &t->ce[m];
		int64		v = 0;
		bool		isnull = !(key0 ? row_key(k0, key0, t->cand[q], &v) : row_key(k0, &cv, q, &v));

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
	t->nlazy += lazy;
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

/* Can the heads of the runs of the first key answer: one integer key, no expression, no skipped or reordered input */
static bool
heads_ok(const SiftTopN *t)
{
	const TopKey *k0 = &t->key[0];

	return sift_topn_heads && sift_topn_runs && t->nkey == 1 && t->nexpr == 0 && !t->all && !t->sort && !t->revisit && k0->xk == TXK_NONE && k0->leaf >= 0 && !k0->blob &&
		!k0->nenum && (k0->vtype == SCV_I32 || k0->vtype == SCV_I64) && t->limit > 0;
}

/* The entries of the head from the end the sort wants go to the result, up to the limit and the bar; false when the head is too short to answer */
static bool
heads_take(SiftTopN *t, const SiftSrcHead *lo, uint32 nlo, const SiftSrcHead *hi, uint32 nhi, uint16 part)
{
	const TopKey *k0 = &t->key[0];
	const SiftSrcHead *h = k0->desc ? hi : lo;
	uint32		n = k0->desc ? nhi : nlo;
	Rank		bar;
	bool		barred = bar_rank(t, &bar);

	if (n < (uint32) t->limit && !(barred && n > 0 && rank_after(rank_of(k0, false, h[n - 1].key), bar)))
		return false;
	for (uint32 i = 0; i < Min(n, (uint32) t->limit); i++)
	{
		TopEnt		e = {0};

		if (barred && rank_after(rank_of(k0, false, h[i].key), bar))
			break;
		e.k[0] = h[i].key;
		ItemPointerSet(&e.tid, (BlockNumber) (h[i].addr >> 16), (OffsetNumber) (h[i].addr & 0xFFFF));
		e.part = part;
		e.tie = tie_key(&e.tid, part);
		res_push(t, &e, NULL, NULL);
	}
	share_bar(t);
	t->nrhead++;
	return true;
}

/*
 * A unit of which every row passes, asked for at most as many rows as the head of the run of the first key holds: the n best rows of the unit
 * are the first n entries of the head from the end the sort wants, with their keys and heap addresses, so the unit is not read. Asked for more, the head still
 * answers once the heap is full and the last entry of the head is after its bar: the rows after the head are not before that entry, so none can enter the result.
 * A row that is dead or changed since the unit was written fails the mask (the visibility check clears its bit), and the unit is read as before.
 */
static bool
head_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rmask, int nrow)
{
	const TopKey *k0 = &t->key[0];
	SiftSrcHead lo[SIFT_SRC_HEAD_MAX],
				hi[SIFT_SRC_HEAD_MAX];
	uint32		nlo,
				nhi,
				nvals,
				nnull;
	uint64		id;

	if (!heads_ok(t))
		return false;
	for (int i = 0; i < nrow / 64; i++)
		if (rmask[i] != ~UINT64CONST(0))
			return false;
	if (nrow % 64 && rmask[nrow / 64] != (UINT64CONST(1) << (nrow % 64)) - 1)
		return false;
	if (!sift_colsrc_run_info(src, unit, k0->leaf, false, &nvals, &nnull, &id) || nvals != (uint32) nrow || nnull != 0)
		return false;
	if (sift_colsrc_run_heads(src, unit, k0->leaf, lo, &nlo, hi, &nhi) == 0)
		return false;
	return heads_take(t, lo, nlo, hi, nhi, unit_part(t, src));
}

static bool walk_in_head(const SiftTopN *t, uint32 sb);

bool
sift_topn_sb_head(SiftTopN *t, SiftSrcSource *src, uint32 sb)
{
	const TopKey *k0 = &t->key[0];
	SiftSrcHead lo[SIFT_SRC_HEAD_MAX],
				hi[SIFT_SRC_HEAD_MAX];
	uint32		nlo,
				nhi;

	if (!heads_ok(t))
		return false;
	if (!t->wkeep || walk_in_head(t, sb) || !sift_colsrc_sb_heads(src, sb, k0->leaf, lo, &nlo, hi, &nhi))
		return false;
	return heads_take(t, lo, nlo, hi, nhi, 0);
}

/* the first 8 bytes of an entry (after the varlena header of a bytea), most significant first, zero padded: it orders the entries as they are up to a tie */
static uint64
key_prefix(uint8 bkind, const char *p, uint32 len)
{
	uint64		k = 0;
	uint32		m;

	if (bkind == BK_BYTEA)
	{
		varattrib_4b h;

		/* an entry of the dictionary is padded to its alignment: the bytes after the varlena image are not part of the value (key_entry) */
		if (len >= sizeof(uint32))
		{
			memcpy(&h, p, sizeof(uint32));
			len = Min(len, VARSIZE_4B(&h));
		}
		p += Min(len, VARHDRSZ);
		len -= Min(len, VARHDRSZ);
	}
	m = Min(len, 8);
	for (uint32 i = 0; i < 8; i++)
		k = k << 8 | (i < m ? (uint8) p[i] : 0);
	return k;
}

/* The order code of the dictionary d for a unit with npass rows to compare (NULL: none, they are compared as bytes) */
static OrdCode *
unit_ord(SiftTopN *t, const SiftSrcDict *d, int64 npass)
{
	const TopKey *k = &t->key[0];
	OrdCode    *oc = NULL;
	const uint32 *sorted;

	if (k->leaf < 0 || k->x || k->xfn || d->n == 0 || d->n > sift_limit(topn_code_entries, ORD_MAX_ENTRIES) ||
		!((k->bkind == BK_TEXT && sift_collate_is_c(k->collid)) || k->bkind == BK_UUID || k->bkind == BK_BYTEA))
		return NULL;
	for (int i = 0; i < ORD_CACHE && oc == NULL; i++)
		if (t->oc[i].id == d->id && t->oc[i].bytes == d->bytes && t->oc[i].n == d->n)
			oc = &t->oc[i];
	if (oc == NULL)
	{
		oc = &t->oc[t->oc_next++ % ORD_CACHE];
		if (oc->ord)
			pfree(oc->ord);
		*oc = (OrdCode) {.id = d->id,.bytes = d->bytes,.n = d->n};
	}
	if (oc->exact)
		return oc;
	oc->rows += npass;
	/* the order of a dictionary is that of its entries as bytes: a bytea entry is a varlena image, which starts with its length, not the order of byteacmp */
	sorted = d->order && k->bkind != BK_BYTEA ? d->order(d->octx, npass) : NULL;
	if (oc->rows < d->n * ORD_PREFIX_ROWS || (sorted == NULL && oc->useless))
		return NULL;
	if (sorted == NULL && oc->ord)
		return oc;
	if (t->ocxt == NULL)
		t->ocxt = AllocSetContextCreate(MemoryContextGetParent(t->ucxt), "sift top-N order codes", ALLOCSET_DEFAULT_SIZES);
	if (oc->ord == NULL)
		oc->ord = MemoryContextAlloc(t->ocxt, (Size) d->n * sizeof(uint64));
	if (sorted)
	{
		oc->exact = true;
		for (uint32 i = 0; i < d->n; i++)
			oc->ord[sorted[i]] = i + 1;
	}
	else
		for (uint32 c = 0; c < d->n; c++)
			oc->ord[c] = key_prefix(k->bkind, d->bytes + d->off[c], d->off[c + 1] - d->off[c]);
	return oc;
}

/* the codes of the dictionary in byte order, for an exact code: the dictionary has them, or sorts now (the cache may have parsed it again since the code was made) */
static const uint32 *
dict_sorted(const SiftSrcDict *d)
{
	const uint32 *sorted = d->order(d->octx, 0);

	return sorted ? sorted : d->order(d->octx, (uint64) d->n * ORD_FORCE_ROWS);
}

/* the entry with this code compared with the bytes of a key, as the sort does */
static int
code_vs_blob(SiftTopN *t, const SiftSrcDict *d, uint32 code, const KeyBlob *b)
{
	const char *p;
	uint32		len;

	key_entry(t, 0, d, code, &p, &len);
	return bytes_cmp(&t->key[0], p, len, b->data, b->len);
}

/* The bar as a code (on: false when there is none, or it is NULL, which is compared as a NULL is): a row is strictly after it when its code is above ob.v (below it, for DESC) */
typedef struct OrdBar
{
	bool		on;
	uint64		v;
} OrdBar;

static void
ord_bar(SiftTopN *t, OrdBar *ob, const OrdCode *oc, const SiftSrcDict *d, const uint32 *code, const TopEnt *bar)
{
	const TopKey *k = &t->key[0];

	ob->on = oc != NULL && bar != NULL && !(bar->nul & 1);
	if (!ob->on)
		return;
	if (bar == &t->lh[0])
		ob->v = oc->ord[code[bar->row]];		/* the unit's n-th: an entry of this dictionary */
	else if (oc->exact)
	{
		/* an entry of another unit: its place among the entries, after the last one that is not above it (ASC) or the last one below it (DESC) */
		const KeyBlob *b = KEY_BLOB(bar->k[0]);
		const uint32 *sorted = dict_sorted(d);
		uint32		lo = 0,
					hi = d->n;

		SIFT_BISECT(uint32, m, lo, hi, k->desc ? code_vs_blob(t, d, sorted[m], b) < 0 : code_vs_blob(t, d, sorted[m], b) <= 0);
		ob->v = lo + (k->desc ? 1 : 0);
	}
	else
		ob->v = key_prefix(k->bkind, KEY_BLOB(bar->k[0])->data, KEY_BLOB(bar->k[0])->len);
}

/* Is the row strictly after the bar? By the codes unless they tie (not exact), then as bytes; without a code, as after_bar does */
static inline bool
row_after(SiftTopN *t, const OrdCode *oc, const OrdBar *ob, uint8 *worse, const SiftSrcDict *d, bool isnull, uint32 code, const TopEnt *bar)
{
	if (oc && ob->on && !isnull)
	{
		uint64		a = oc->ord[code];

		if (a != ob->v)
			return t->key[0].desc ? a < ob->v : a > ob->v;
		if (oc->exact)
			return false;
		t->nties++;
		return vs_bar(t, d, false, code, bar) > 0;
	}
	return worse ? after_bar(t, worse, d, isnull, code, bar) : vs_bar(t, d, isnull, code, bar) > 0;
}

/* Does the row come after the unit's n-th, or tie with it (the rows are visited in order, so a tie comes after)? */
static inline bool
row_not_before(SiftTopN *t, const OrdCode *oc, const uint32 *codes, const SiftSrcDict *d, bool isnull, uint32 code)
{
	const TopEnt *l0 = &t->lh[0];

	if (oc && !isnull && !(l0->nul & 1))
	{
		uint64		a = oc->ord[code],
					c = oc->ord[codes[l0->row]];

		if (a != c)
			return t->key[0].desc ? a < c : a > c;
		if (oc->exact)
			return true;
		t->nties++;
	}
	return vs_bar(t, d, isnull, code, l0) >= 0;
}

/*
 * sift_topn_unit for a first key of byte strings: the bar is the heap's worst entry or the unit's n-th so far (whichever sorts first), and the rows are
 * compared with it by the entry of the dictionary, as the type's comparison says, or by the order code of the dictionary when it has one.
 */
static void
unit_blob(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	const uint32 *code = key0->data;
	const TopEnt *bar = t->n == t->limit ? &t->heap[0] : NULL;
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;
	int64		npass = 0;
	OrdCode    *oc;
	uint8	   *worse = NULL;
	OrdBar		ob;

	for (int w = 0; w < nw; w++)
		npass += pg_popcount64(rmask[w]);
	oc = unit_ord(t, key0->dict, npass);
	/* a string that was after the bar is remembered by its code: worth the byte a code when the rows of the unit are not few against the entries */
	if (oc == NULL && npass * 8 >= key0->dict->n)
		worse = MemoryContextAllocZero(t->ucxt, Max(key0->dict->n, 1));
	t->nord[oc ? (oc->exact ? 0 : 1) : 2]++;
	t->nties = 0;
	grow_unit_buffers(t, nrow);
	t->ln = 0;
	ord_bar(t, &ob, oc, key0->dict, code, bar);
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			bool		isnull = key0->isnull && key0->isnull[i];
			TopEnt		e;

			if (bar && row_after(t, oc, &ob, worse, key0->dict, isnull, code[i], bar))
				continue;
			t->crow[m] = i;
			t->ck[m] = code[i];
			t->cnul[m++] = isnull;
			/* a row that ties with the unit's n-th stays a candidate, but it comes after it: the rows are visited in order */
			if (t->ln == t->limit && row_not_before(t, oc, code, key0->dict, isnull, code[i]))
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
			if (bar == &t->lh[0])
				ord_bar(t, &ob, oc, key0->dict, code, bar);
		}
	for (int q = 0; q < m; q++)
	{
		TopEnt	   *e = &t->ce[nc];

		if (bar && row_after(t, oc, &ob, worse, key0->dict, t->cnul[q], t->ck[q], bar))
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
	if (oc && !oc->exact && t->nties * 4 > npass)
		oc->useless = true;
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

	if (!sift_topn_runs || t->key[0].xk != TXK_NONE || t->key[0].leaf < 0 || t->all || t->sort || t->limit <= 0 || units < 8)
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
 * The rows of the words w0..w0+nw of mask (a unit's row mask) go to the unit's heap in row order: those the bar leaves, with their keys in crow, ck and cnul
 * (the candidates finish_unit reads the other keys of). The bar tightens as the heap fills.
 */
static void
scan_rows(SiftTopN *t, const TopKey *k0, const SiftSrcVec *key0, const uint64 *mask, int w0, int nw, Rank *bar, bool *barred, int *m)
{
	for (int w = 0; w < nw; w++)
		for (uint64 b = mask[w]; b; b &= b - 1)
		{
			int			i = (w0 + w) * 64 + pg_rightmost_one_pos64(b);
			int64		v = 0;
			bool		isnull = !row_key(k0, key0, i, &v);
			TopEnt		e;

			if (*barred && rank_after(rank_of(k0, isnull, v), *bar))
				continue;
			memset(&e, 0, sizeof(e));
			e.k[0] = v;
			e.nul = isnull;
			e.row = e.tie = i;
			t->crow[*m] = i;
			t->ck[*m] = v;
			t->cnul[(*m)++] = isnull;
			heap_push(t, t->lh, &t->ln, &e);	/* the other keys are 0 in all of them: the order is the first key's, then the row */
			if (t->ln == t->limit)
			{
				Rank		mine = ent_rank(t, &t->lh[0]);

				if (!*barred || rank_after(*bar, mine))
				{
					*bar = mine;
					*barred = true;
				}
			}
		}
}

typedef struct SegBest
{
	int64		best;
	int			seg;
} SegBest;

static int
segbest_asc(const void *a, const void *b)
{
	int64		x = ((const SegBest *) a)->best,
				y = ((const SegBest *) b)->best;

	return x < y ? -1 : x > y;
}

static int
segbest_desc(const void *a, const void *b)
{
	return segbest_asc(b, a);
}

/*
 * scan_rows for an integer or float first key, TOPN_SEG rows at a time: the segments are taken by the best value they hold (a descending scan of a column that
 * ascends, or the reverse, would push every row of the unit in turn), a segment whose best cannot pass the bar ends the unit, and for the others a range
 * kernel marks the rows that can pass: only those reach the heap. A value equal to the bar's passes, as in scan_rows. The NULL rows (the vector's value
 * there is undefined) go first, through scan_rows.
 */
static void
vec_rows(SiftTopN *t, const TopKey *k0, const SiftSrcVec *key0, const uint64 *rmask, int nrow, Rank *bar, bool *barred, int *m)
{
	const bool	wide = k0->vtype == SCV_I64,
				isf = k0->vtype == SCV_F64;
	const char *data = key0->data;
	const int	esz = wide || isf ? 8 : 4;
	int			nw = (nrow + 63) / 64,
				nseg = (nrow + TOPN_SEG - 1) / TOPN_SEG,
				nb = 0;
	bool		all;
	uint64	   *vm = palloc0((SIFT_COL_NWORDS(nrow) + SIFT_COL_SELWORDS) * sizeof(uint64));
	uint64		hit[SIFT_COL_SELWORDS];
	SegBest    *sb = palloc(nseg * sizeof(SegBest));

	memcpy(vm, rmask, nw * sizeof(uint64));
	if (key0->isnull)
	{
		uint64	   *nullw = palloc((SIFT_COL_NWORDS(nrow) + SIFT_COL_SELWORDS) * sizeof(uint64));
		bool		any = false;

		sift_colk_nonzero(key0->isnull, nrow, nullw);
		for (int w = 0; w < nw; w++)
		{
			uint64		nm = vm[w] & nullw[w];

			vm[w] &= ~nm;
			nullw[w] = nm;
			any |= nm != 0;
		}
		if (any)
			scan_rows(t, k0, key0, nullw, 0, nw, bar, barred, m);
		pfree(nullw);
	}
	/* every row of the unit is selected: the kernels need no mask */
	all = sift_colk->bits_count(vm, nw) == nrow;
	for (int s = 0; s < nseg; s++)
	{
		int			off = s * TOPN_SEG,
					len = Min(TOPN_SEG, nrow - off);
		int64		mn,
					mx;
		bool		some;

		if (isf)
		{
			double		a,
						b;

			some = sift_aggk->minmax_f64(&a, &b, (const double *) data + off, all ? NULL : vm + off / 64, len);
			mn = f8_key(a);
			mx = f8_key(b);
		}
		else if (wide)
			some = sift_aggk->minmax_i64(&mn, &mx, (const int64 *) data + off, all ? NULL : vm + off / 64, len);
		else
		{
			int32		a,
						b;

			some = sift_aggk->minmax_i32(&a, &b, (const int32 *) data + off, all ? NULL : vm + off / 64, len);
			mn = a;
			mx = b;
		}
		if (some)
		{
			sb[nb].best = k0->desc ? mx : mn;
			sb[nb++].seg = s;
		}
	}
	qsort(sb, nb, sizeof(SegBest), k0->desc ? segbest_desc : segbest_asc);
	for (int q = 0; q < nb; q++)
	{
		int			off = sb[q].seg * TOPN_SEG,
					len = Min(TOPN_SEG, nrow - off),
					sw = (len + 63) / 64;
		Rank		g;

		if (bar_rank(t, &g) && (!*barred || rank_after(*bar, g)))
		{
			*bar = g;
			*barred = true;
		}
		if (*barred && bar->cls == 0)
			break;				/* the worst row is a NULL, which sorts before every value */
		if (*barred && bar->cls == 1)
		{
			if (rank_after(rank_of(k0, false, sb[q].best), *bar))
				break;			/* the segments after this one hold worse values */
			if (isf)
				sift_aggk->cmp_f64(hit, (const double *) data + off, len, k0->desc ? SCMP_GE : SCMP_LE,
								   f8_of_key((int64) ((k0->desc ? ~bar->ord : bar->ord) ^ UINT64CONST(0x8000000000000000))));
			else if (wide)
			{
				uint64		bv = (uint64) (~bar->ord ^ UINT64CONST(0x8000000000000000));

				sift_colk->range[SIFT_COLW_64](data + (Size) off * esz, len, k0->desc ? bv : UINT64CONST(0x8000000000000000),
											   k0->desc ? (uint64) PG_INT64_MAX - bv : bar->ord, hit);
			}
			else
			{
				uint32		bv = (uint32) (~bar->ord ^ UINT64CONST(0x8000000000000000));

				sift_colk->range[SIFT_COLW_32](data + (Size) off * esz, len, k0->desc ? bv : 0x80000000u,
											   k0->desc ? (uint32) PG_INT32_MAX - bv : (uint32) bar->ord ^ 0x80000000u, hit);
			}
			for (int w = 0; w < sw; w++)
				hit[w] &= vm[off / 64 + w];
			scan_rows(t, k0, key0, hit, off / 64, sw, bar, barred, m);
		}
		else
			scan_rows(t, k0, key0, vm + off / 64, off / 64, sw, bar, barred, m);
	}
	pfree(sb);
	pfree(vm);
}

static void
unit_reset(SiftTopN *t)
{
	if (t->ucxt)
	{
		MemoryContextReset(t->ucxt);
		for (int j = 0; j < t->nkey; j++)
			t->key[j].xid = NULL;
	}
}

/* The unit from its sorted run with no filter run: false when the walk finds the conditions fail for too many rows (the unit goes on to its filters) */
bool
sift_topn_unit_lazy(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rmask, int nrow)
{
	unit_reset(t);
	return run_unit(t, src, unit, NULL, rmask, nrow, true);
}

/*
 * The rows of the unit are cut in two steps, so that the leaves of the other sort keys are read for a handful of rows and not for all of
 * them. First the first key of every row that passes: a row strictly worse on it than the n-th of the heap, or than the n-th of the unit
 * so far, cannot be in the result whatever its other keys are. Then the others, for the rows that remain (the n best and those tied with
 * the n-th).
 */
void
sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow, SiftTopLeaf leaf, void *ctx)
{
	const TopKey *k0 = &t->key[0];
	Rank		bar = {0, 0};
	bool		barred = false;
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;

	unit_reset(t);
	/* the first key of the rows is read whole only by the paths that test every row: a unit from its sorted run reads the keys of the rows it takes, by position */
	if (key0 == NULL && k0->leaf >= 0 && (t->nexpr > 0 || t->all || k0->blob || !sift_topn_runs))
		key0 = leaf(ctx, k0->leaf);
	if (t->nexpr > 0)
	{
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].x)
				expr_eval_unit(t, &t->key[j], leaf, ctx, rmask, nrow);
		if (t->key[0].x)
			key0 = &t->key[0].xv;
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
		(void) run_unit(t, src, unit, key0, rmask, nrow, false);
		return;
	}
	if (key0 == NULL && head_unit(t, src, unit, rmask, nrow))
		return;
	if (run_unit(t, src, unit, key0, rmask, nrow, false))
		return;
	if (key0 == NULL && k0->leaf >= 0)
		key0 = leaf(ctx, k0->leaf);
	grow_unit_buffers(t, nrow);
	t->ln = 0;
	barred = bar_rank(t, &bar);
	if (nrow >= TOPN_VEC_ROWS && k0->xk == TXK_NONE && k0->nenum == 0 && (k0->vtype == SCV_I32 || k0->vtype == SCV_I64 || k0->vtype == SCV_F64) &&
		sift_colk->bits_count(rmask, nw) * TOPN_VEC_DENSITY >= nrow)
		vec_rows(t, k0, key0, rmask, nrow, &bar, &barred, &m);
	else
		scan_rows(t, k0, key0, rmask, 0, nw, &bar, &barred, &m);
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

	/* the zone is of OIDs, which are not in the order of the values */
	o.inexact |= k->nenum > 0;
	if (k->xk != TXK_COALESCE || o.inexact || z->nnull == 0)
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
	return !t->all && !t->totals && t->key[0].leaf >= 0 && t->key[0].nenum == 0 && (t->key[0].vtype == SCV_I32 || t->key[0].vtype == SCV_I64);
}

/*
 * Can a row of a unit with this zone of the first sort column sort before the n-th best so far (or tie with it)? Nulls sort as the query
 * says, the rest between min and max. Rows tied on the first key may win on the others, so a bound equal to the n-th's is kept; with one key
 * a tie wins nothing (the query does not say which of the tied rows comes), so only a better bound is. The zone counts the dead rows too: it
 * only ever says "no" for a unit that has nothing better.
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
	return t->nkey == 1 ? rank_after(bar, best) : !rank_after(best, bar);
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
 * their zone, not in table order. Every superblock's zone is read first (sift_topn_walk_setup, headers only), the superblocks are sorted by the
 * best row they can hold, and sift_topn_walk_next hands them out until the zone of the next cannot beat the n-th best:
 * the zones after it are no better, so the walk ends there. The result does not depend on the order: a superblock is left out only when no
 * row in it can sort before the n-th (a bound equal to it is kept: ties go on to the other keys and the TID).
 */
bool
sift_topn_follows(const SiftTopN *t)
{
	return t->follows;
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

static int
rank_cmp(Rank a, Rank b)
{
	return rank_after(a, b) ? 1 : rank_after(b, a) ? -1 : 0;
}

/* The superblocks by the best row they can hold, key by key: a unit whose bound on the keys so far ties with another is ordered by the next key */
typedef struct WalkSort
{
	const Rank *bound;
	int			wkeys;
} WalkSort;

static int
walk_cmp(const void *pa, const void *pb, void *arg)
{
	const WalkSort *w = arg;
	uint32		a = *(const uint32 *) pa,
				b = *(const uint32 *) pb;

	for (int j = 0; j < w->wkeys; j++)
	{
		int			c = rank_cmp(w->bound[(Size) a * w->wkeys + j], w->bound[(Size) b * w->wkeys + j]);

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
	return t->nkey > 1;
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

/*
 * The order of the walk, kept by the backend: the bounds of all the superblocks and the order of their best rows depend on the zones of the stored units alone,
 * and a query that sorts by the same keys of the same units (the same file and version of the directory) walks the same order. Reading every zone and sorting
 * the superblocks cost O(units) for each such query; the kept order costs the superblocks it visits. The superblocks that hold rows of a write buffer are
 * not in the zones (the buffers change with no new version): each query lists them (sift_colsrc_walk_stamp) and visits them first, as their bound, which is
 * none, puts them.
 */
#define WALK_CACHED		4			/* orders kept */
#define WALK_MEM_SBS	64			/* superblocks with write buffer rows beyond which an order is not kept */
#define WALK_KEEP_BYTES	((Size) 16 << 20)

typedef struct WalkKey
{
	SiftSrcWalkStamp st;
	uint32		nsb;
	int			wkeys;
	struct
	{
		int			leaf;
		int			vtype;
		int			xk;
		int64		xc;
		bool		desc;
		bool		nulls_first;
	}			k[SIFT_TOPN_MAX_KEYS];
} WalkKey;

typedef struct WalkBase
{
	WalkKey		key;
	uint32		nsb;
	Rank	   *bound;			/* wkeys per superblock */
	uint32	   *units;			/* the units of the stored segments in the superblock */
	uint32	   *ord;			/* the superblocks by their best bound */
	uint32	   *pos;			/* the place of each superblock in ord */
	uint32	   *tail;			/* nsb + 1: the units of ord[i ..] */
	MemoryContext cx;
	uint64		used;
} WalkBase;

static MemoryContext walk_cx;
static WalkBase *walk_kept[WALK_CACHED];
static uint64 walk_clock;

static bool
walk_viable(const SiftTopN *t, uint32 sb)
{
	return bounds_viable(t, &t->wb->bound[(Size) sb * t->wkeys]);
}

static WalkBase *
walk_find(const WalkKey *key)
{
	for (int i = 0; i < WALK_CACHED; i++)
		if (walk_kept[i] != NULL && memcmp(&walk_kept[i]->key, key, sizeof(WalkKey)) == 0)
		{
			walk_kept[i]->used = ++walk_clock;
			return walk_kept[i];
		}
	return NULL;
}

/* Keeps wb, in a context of its own; the kept orders of the same file of an older version, and the least used when all places are taken, go */
static void
walk_keep(WalkBase *wb)
{
	int			slot = -1;

	for (int i = 0; i < WALK_CACHED; i++)
	{
		WalkBase   *o = walk_kept[i];

		if (o != NULL && o->key.st.index == wb->key.st.index && o->key.st.file == wb->key.st.file && o->key.st.version != wb->key.st.version)
		{
			MemoryContextDelete(o->cx);
			walk_kept[i] = NULL;
		}
		if (walk_kept[i] == NULL)
			slot = i;
	}
	if (slot < 0)
	{
		slot = 0;
		for (int i = 1; i < WALK_CACHED; i++)
			if (walk_kept[i]->used < walk_kept[slot]->used)
				slot = i;
		MemoryContextDelete(walk_kept[slot]->cx);
	}
	wb->used = ++walk_clock;
	walk_kept[slot] = wb;
}

static WalkBase *
walk_build(SiftTopN *t, MemoryContext cxt, const WalkKey *key, bool segs)
{
	uint32		nsb = t->nsb;
	int			wkeys = t->wkeys;
	MemoryContext cx = key != NULL ? AllocSetContextCreate(cxt, "sift cache topn walk", ALLOCSET_DEFAULT_SIZES) : AllocSetContextCreate(cxt, "sift topn walk", ALLOCSET_DEFAULT_SIZES);
	WalkBase   *wb = MemoryContextAllocZero(cx, sizeof(WalkBase));
	WalkSort	ws;

	wb->cx = cx;
	wb->nsb = nsb;
	if (key != NULL)
		wb->key = *key;
	wb->bound = MemoryContextAllocZero(cx, (Size) nsb * wkeys * sizeof(Rank));
	wb->units = MemoryContextAllocZero(cx, nsb * sizeof(uint32));
	wb->ord = MemoryContextAlloc(cx, nsb * sizeof(uint32));
	wb->pos = MemoryContextAlloc(cx, nsb * sizeof(uint32));
	wb->tail = MemoryContextAllocZero(cx, ((Size) nsb + 1) * sizeof(uint32));
	for (uint32 sb = 0; sb < nsb; sb++)
	{
		for (int j = 0; j < wkeys; j++)
		{
			SiftSrcZone z;
			int			leaf = t->key[j].leaf;

			wb->units[sb] = segs ? sift_colsrc_zone_range_segs(t->src, sb, sb + 1, leaf, &z) : sift_colsrc_zone_range(t->src, sb, sb + 1, leaf, &z);
			wb->bound[(Size) sb * wkeys + j] = zone_best(&t->key[j], &z);
		}
		wb->ord[sb] = sb;
	}
	ws = (WalkSort) {wb->bound, wkeys};
	qsort_arg(wb->ord, nsb, sizeof(uint32), walk_cmp, &ws);
	for (uint32 i = nsb; i > 0; i--)
	{
		wb->pos[wb->ord[i - 1]] = i - 1;
		wb->tail[i - 1] = wb->tail[i] + wb->units[wb->ord[i - 1]];
	}
	return wb;
}

/* The walk of a first sort column that does not follow the table (t->nsb superblocks): its bounds and order, from the backend's kept ones when the source allows */
void
sift_topn_walk_setup(SiftTopN *t)
{
	WalkKey		key;
	SiftSrcWalkStamp st;
	uint32		mem[WALK_MEM_SBS];
	int			nmem = 0;
	bool		keep = sift_colsrc_walk_stamp(t->src, &st, mem, WALK_MEM_SBS, &nmem);
	WalkBase   *wb = NULL;

	t->wpos = 0;
	t->hpos = t->nhead = 0;
	if (keep)
	{
		memset(&key, 0, sizeof(key));
		key.st = st;
		key.nsb = t->nsb;
		key.wkeys = t->wkeys;
		for (int j = 0; j < t->wkeys; j++)
		{
			key.k[j].leaf = t->key[j].leaf;
			key.k[j].vtype = t->key[j].vtype;
			key.k[j].xk = t->key[j].xk;
			key.k[j].xc = t->key[j].xc;
			key.k[j].desc = t->key[j].desc;
			key.k[j].nulls_first = t->key[j].nulls_first;
		}
		wb = walk_find(&key);
		if (wb == NULL)
		{
			/* kept as long as the backend: "sift cache" contexts are not leaks to scripts/leakcheck.sh */
			if (walk_cx == NULL)
				walk_cx = AllocSetContextCreate(TopMemoryContext, "sift cache topn walks", ALLOCSET_DEFAULT_SIZES);
			if ((Size) t->nsb * (t->wkeys * sizeof(Rank) + 4 * sizeof(uint32)) <= WALK_KEEP_BYTES)
			{
				wb = walk_build(t, walk_cx, &key, true);
				walk_keep(wb);
			}
			else
				keep = false;
		}
	}
	if (!keep)
	{
		/* the zones with the write buffers in them: no superblock is left out of the order */
		wb = walk_build(t, GetMemoryChunkContext(t), NULL, false);
		nmem = 0;
	}
	t->wb = wb;
	t->wkeep = keep;
	if (nmem > 0)
	{
		t->whead = MemoryContextAlloc(GetMemoryChunkContext(t), nmem * sizeof(uint32));
		memcpy(t->whead, mem, nmem * sizeof(uint32));
		t->nhead = nmem;
	}
}

static bool
walk_in_head(const SiftTopN *t, uint32 sb)
{
	int			lo = 0,
				hi = t->nhead;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (t->whead[mid] == sb)
			return true;
		if (t->whead[mid] < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

/* The units of the superblocks the walk has not reached: those the head has visited are not among them */
static int64
walk_rest(const SiftTopN *t)
{
	const WalkBase *wb = t->wb;
	int64		n = wb->tail[t->wpos];

	for (int i = 0; i < t->nhead; i++)
		if (t->whead[i] < wb->nsb && wb->pos[t->whead[i]] >= t->wpos)
			n -= wb->units[t->whead[i]];
	return n;
}

bool
sift_topn_walk_next(SiftTopN *t, uint32 *sb)
{
	const WalkBase *wb = t->wb;

	if (t->hpos < t->nhead)
	{
		*sb = t->whead[t->hpos++];
		return true;
	}
	for (;;)
	{
		if (t->wpos >= wb->nsb)
			return false;
		*sb = wb->ord[t->wpos];
		if (t->nhead > 0 && walk_in_head(t, *sb))
		{
			t->wpos++;
			continue;
		}
		if (!walk_viable(t, *sb))
		{
			t->skipped += walk_rest(t);
			t->wpos = wb->nsb;
			return false;
		}
		t->wpos++;
		return true;
	}
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
		sort_ents(t->res, t->n, t);
		if (t->want > 0 && t->n >= t->want)
		{
			const TopEnt *w = &t->res[t->want - 1];

			if (t->n == t->limit && key_cmp(t, w, &t->res[t->n - 1]) == 0)
				t->incomplete = true;
			else
				while (t->n > t->want && key_cmp(t, w, &t->res[t->n - 1]) != 0)
					t->n--;
		}
	}
}

bool
sift_topn_incomplete(const SiftTopN *t)
{
	return t->incomplete;
}

int
sift_topn_candidates(const SiftTopN *t)
{
	return t->want > 0 && !t->all ? t->limit : 0;
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

/* The value of the key as the entries of the global order hold it */
static inline int64
heap_key(Oid typid, Datum d)
{
	switch (typid)
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

static GBox
gcond_box(const SiftTopGPred *p)
{
	GBox		r = {false, false, false, 0, 0};

	/* a value is not NULL: the entries of the values, whatever the key (those of the NULLs come after them) */
	if (p->kind == STG_NOTNULL)
	{
		r.has_lo = p->lead;
		r.lo = PG_INT64_MIN;
		return r;
	}
	if (p->kind == STG_CMP)
	{
		if (!p->lead)
			return r;
		switch (p->op)
		{
			case SCMP_EQ:
				r.has_lo = r.has_hi = true;
				r.lo = r.hi = p->val;
				break;
			case SCMP_LT:
				r.empty = p->val == PG_INT64_MIN;
				r.has_hi = true;
				r.hi = p->val - !r.empty;
				break;
			case SCMP_LE:
				r.has_hi = true;
				r.hi = p->val;
				break;
			case SCMP_GT:
				r.empty = p->val == PG_INT64_MAX;
				r.has_lo = true;
				r.lo = p->val + !r.empty;
				break;
			case SCMP_GE:
				r.has_lo = true;
				r.lo = p->val;
				break;
			default:
				break;
		}
		return r;
	}
	r = gcond_box(p->child[0]);
	for (int i = 1; i < p->nchild; i++)
	{
		GBox		c = gcond_box(p->child[i]);

		if (p->kind == STG_AND)
		{
			r.empty |= c.empty;
			if (c.has_lo && (!r.has_lo || c.lo > r.lo))
				r.has_lo = true, r.lo = c.lo;
			if (c.has_hi && (!r.has_hi || c.hi < r.hi))
				r.has_hi = true, r.hi = c.hi;
		}
		else if (c.empty)
			continue;
		else if (r.empty)
			r = c;
		else
		{
			r.has_lo = r.has_lo && c.has_lo;
			r.lo = Min(r.lo, c.lo);
			r.has_hi = r.has_hi && c.has_hi;
			r.hi = Max(r.hi, c.hi);
		}
	}
	return r;
}

/* The conditions of the scan on the first key as the range of it that they keep, which a sorted run is read within (only integer and time columns make a condition: sift_topn_leaf_attno) */
void
sift_topn_key_range(SiftTopN *t, const SiftTopGPred *cond)
{
	if (cond != NULL)
		t->kbox = gcond_box(cond);
}

/* does the heap row pass: a NULL column passes no comparison */
static bool
gcond_test(const SiftTopGPred *p, TupleTableSlot *slot)
{
	if (p->kind == STG_NOTNULL)
	{
		bool		isnull;

		(void) slot_getattr(slot, p->attno, &isnull);
		return !isnull;
	}
	if (p->kind == STG_CMP)
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, p->attno, &isnull);

		if (isnull)
			return false;
		return gcond_cmp(p->op, heap_key(p->typid, d), p->val);
	}
	for (int i = 0; i < p->nchild; i++)
		if (gcond_test(p->child[i], slot) == (p->kind == STG_OR))
			return p->kind == STG_OR;
	return p->kind == STG_AND;
}

/* a row of a group of the first key: its address and the values of the keys, which the heap row gave */
typedef struct GRow
{
	ItemPointerData tid;
	bool		isnull[TOP_GKEYS];
	int64		key[TOP_GKEYS];
} GRow;

/* the keys after the first, then the address */
static int
grow_cmp(const void *a, const void *b, void *arg)
{
	const SiftTopN *t = arg;
	const GRow *x = a,
			   *y = b;

	for (int j = 1; j < t->nkey; j++)
	{
		const TopKey *k = &t->key[j];

		if (x->isnull[j] != y->isnull[j])
			return x->isnull[j] == k->nulls_first ? -1 : 1;
		if (!x->isnull[j] && x->key[j] != y->key[j])
			return (x->key[j] < y->key[j]) != k->desc ? -1 : 1;
	}
	return ItemPointerCompare(unconstify(ItemPointerData *, &x->tid), unconstify(ItemPointerData *, &y->tid));
}

/*
 * More keys than the first: the array gives the rows in the order of the first key only, so the scan takes the whole groups of rows that share a value of it
 * (the heap row says what the others hold, and whether it passes the condition) until the groups make the n rows, and puts each group in order. A scan that
 * reads more entries than GORD_SCAN_MIN, or 64 for each row wanted, before the n are in has a first key that many rows share: the units read it better.
 */
#define GORD_SCAN_MIN	4096

static bool
gord_prep(SiftTopN *t)
{
	TopFetch   *f = &t->pf[0];
	int64		cap = Max(GORD_SCAN_MIN, 64 * (int64) t->limit),
				scanned = 0,
				gkey = 0,
				key;
	bool		isnull,
				gnull = false,
				have = false,
				closed = false;
	int			gstart = 0,
				room = 64;
	ItemPointerData tid;

	t->gq = MemoryContextAlloc(GetMemoryChunkContext(t), room * sizeof(GRow));
	while (sift_gord_next(t->go, &key, &isnull, &tid))
	{
		bool		call_again = false,
					all_dead,
					hnull;
		Datum		d;
		GRow	   *r;
		ItemPointerData entry = tid;	/* the fetch leaves the address of the version it found in tid: the entry is the root of its chain, which a second fetch starts from */

		if (++scanned > cap)
			return false;
		if (have && (key != gkey || isnull != gnull))
		{
			qsort_arg(t->gq + gstart, t->gqn - gstart, sizeof(GRow), grow_cmp, t);
			if (t->gqn >= t->limit)
			{
				closed = true;
				break;
			}
			gstart = t->gqn;
		}
		have = true;
		gkey = key;
		gnull = isnull;
		if (!table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
			continue;
		t->fetched++;
		d = slot_getattr(f->slot, t->gatt[0], &hnull);
		if (hnull != isnull || (!hnull && heap_key(t->gtyp[0], d) != key))
			continue;
		if (t->gcond != NULL && !gcond_test(t->gcond, f->slot))
			continue;
		if (t->gqn == room)
		{
			room *= 2;
			t->gq = repalloc(t->gq, room * sizeof(GRow));
		}
		r = &t->gq[t->gqn++];
		r->tid = entry;
		for (int j = 0; j < t->nkey; j++)
		{
			d = slot_getattr(f->slot, t->gatt[j], &r->isnull[j]);
			r->key[j] = r->isnull[j] ? 0 : heap_key(t->gtyp[j], d);
		}
	}
	/* the end of the array closes the last group; a group that ended the scan above is sorted already */
	if (!closed && t->gqn - gstart > 1)
		qsort_arg(t->gq + gstart, t->gqn - gstart, sizeof(GRow), grow_cmp, t);
	return true;
}

/*
 * The scan reads the global order of the key (one entry for every row ever indexed, in the order of the key) in place of the units: the rows of the heap come in
 * the order of the key and the n-th one ends the scan. An entry is for the heap to confirm, as a b-tree's is: the row of the address may be dead, or another one
 * where the first was vacuumed, which holds another key.
 */
/* the conditions on a column other than the first key that the array read does not narrow: those outside an equality of the group leaf gl at the top of the conjunction, with the first key not pinned */
static int
gcond_loose(const SiftTopGPred *p, int gl, bool top)
{
	int			n = 0;

	if (p->kind == STG_NOTNULL)
		return p->lead || p->pinned ? 0 : 1;
	if (p->kind == STG_CMP)
		return p->lead || p->pinned || (top && p->op == SCMP_EQ && p->leaf == gl) ? 0 : 1;
	for (int i = 0; i < p->nchild; i++)
		n += gcond_loose(p->child[i], gl, top && p->kind == STG_AND);
	return n;
}

/* the leaves of a condition, into t->rleaf: false when there are more than it holds */
static bool
resid_leaves(SiftTopN *t, const SiftSrcLayout *lay, const SiftTopGPred *p)
{
	int			j = 0;

	if (p->kind != STG_AND && p->kind != STG_OR)
	{
		while (j < t->nresid && t->rleaf[j] != p->leaf)
			j++;
		if (j == t->nresid)
		{
			if (j == TOP_RESID)
				return false;
			t->rleaf[j] = p->leaf;
			t->rwide[j] = lay->leaf[p->leaf].vtype == SCV_I64;
			t->nresid++;
		}
		return true;
	}
	for (int i = 0; i < p->nchild; i++)
		if (!resid_leaves(t, lay, p->child[i]))
			return false;
	return true;
}

/*
 * The conditions of the scan are all of them on integer and time columns (cond), and the range of the first key that they keep is bounded: the units read through their
 * sorted run do not run the filters of the scan, they test cond on the rows the walk takes from the range (sift_topn_lazy_unit). A condition on another column that
 * does not narrow the walk (not pinned by an equality of the first key) is left to the filters: the walk would read the column for rows that fail it.
 */
void
sift_topn_lazy(SiftTopN *t, const SiftSrcLayout *lay, const SiftTopGPred *cond)
{
	t->resid = NULL;
	t->nresid = 0;
	if (cond == NULL || !(t->kbox.has_lo || t->kbox.has_hi) || gcond_loose(cond, -1, true) > 0 || !resid_leaves(t, lay, cond))
		return;
	t->resid = cond;
}

/* Does the unit go to its sorted run without its filters (the paths that test every row of the mask come before the run in sift_topn_unit) */
bool
sift_topn_lazy_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit)
{
	uint32		nvals,
				nnull;
	uint64		id;

	return t->resid != NULL && !t->all && t->nexpr == 0 && run_ready(t, src, unit, &nvals, &nnull, &id);
}

/* the equalities of the top of the conjunction on columns other than the first key: a global order grouped by one of them reads the entries of that group */
static int
gcond_pins(const SiftTopGPred *p, SiftGBound *b)
{
	if (p->kind == STG_CMP)
	{
		if (!p->lead && p->op == SCMP_EQ && b->npin < 4)
		{
			b->pinleaf[b->npin] = p->leaf;
			b->pinval[b->npin++] = p->val;
		}
		return b->npin;
	}
	if (p->kind == STG_AND)
		for (int i = 0; i < p->nchild; i++)
			if (p->child[i]->kind == STG_CMP)
				gcond_pins(p->child[i], b);
	return b->npin;
}

/*
 * The scan reads the global order of the key (one entry for every row ever indexed, in the order of the key) in place of the units: the rows of the heap come in
 * the order of the key and the n-th one ends the scan. An entry is for the heap to confirm, as a b-tree's is: the row of the address may be dead, or another one
 * where the first was vacuumed, which holds another key.
 */
bool
sift_topn_gorder(SiftTopN *t, SiftSrcSource *src, const SiftTopGPred *cond)
{
	SiftGBound	b = {0};
	bool		bounds = false;
	int			gl;

	/* the array costs the same as the heads of the runs for a few rows on a table of a few units, and does not grow with the units (a key-cut index has thousands) */
	if (!t->gok || t->nogord || t->all || (t->gtext && cond != NULL))
		return false;
	if (cond != NULL)
	{
		GBox		box = gcond_box(cond);

		if (box.empty)
			b.has_lo = b.has_hi = true, b.lo = 1, b.hi = 0;
		else
			b.has_lo = box.has_lo, b.has_hi = box.has_hi, b.lo = box.lo, b.hi = box.hi;
		bounds = box.empty || box.has_lo || box.has_hi;
		(void) gcond_pins(cond, &b);
	}
	t->go = sift_gord_open(src, t->key[0].leaf, t->limit, t->key[0].desc, t->key[0].nulls_first, t->gtext ? (uint32) t->key[0].collid : 0, cond != NULL ? &b : NULL, GetMemoryChunkContext(t));
	if (t->go == NULL)
		return false;
	gl = sift_gord_group(t->go);
	/* a condition that leaves no bound on the key and no group is a filter of the whole array: the units, with their zones, read less */
	if (cond != NULL && ((!bounds && gl < 0) || gcond_loose(cond, gl, true) > 0))
	{
		t->go = NULL;
		return false;
	}
	t->gcond = cond;
	if (t->nkey > 1 && !gord_prep(t))
		t->go = NULL;
	return t->go != NULL;
}

bool
sift_topn_gordered(const SiftTopN *t)
{
	return t->go != NULL;
}

/* a row the Limit above throws away: NULLs, the heap row is not read */
static bool
skipped_row(SiftTopN *t, TupleTableSlot *slot)
{
	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, t->nout * sizeof(bool));
	ExecStoreVirtualTuple(slot);
	return true;
}

static bool
gord_next(SiftTopN *t, TupleTableSlot *slot)
{
	TopFetch   *f = &t->pf[0];
	int64		key;
	bool		isnull;
	ItemPointerData tid;

	while (t->emitted < t->limit && (t->nkey > 1 ? t->gqpos < t->gqn : sift_gord_next(t->go, &key, &isnull, &tid)))
	{
		bool		call_again = false,
					all_dead,
					hnull;
		Datum		d;

		if (t->nkey > 1)
			tid = t->gq[t->gqpos++].tid;
		if (!table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
			continue;
		if (t->nkey == 1)
		{
			d = slot_getattr(f->slot, t->gatt[0], &hnull);
			if (hnull != isnull)
				continue;
			if (!hnull && t->gtext)
			{
				/* the entry's string is the heap row's: a row an UPDATE or a vacuumed address replaced holds another */
				uint32		slen;
				const char *str = sift_gord_str(t->go, &slen);
				struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				if (VARSIZE_ANY_EXHDR(v) != slen || memcmp(VARDATA_ANY(v), str, slen) != 0)
					continue;
			}
			else if (!hnull && heap_key(t->gtyp[0], d) != key)
				continue;
			if (t->gcond != NULL && !gcond_test(t->gcond, f->slot))
				continue;
		}
		t->fetched++;
		if (t->emitted++ < t->skip)
			return skipped_row(t, slot);
		ExecClearTuple(slot);
		for (int c = 0; c < t->nout; c++)
			slot->tts_values[c] = slot_getattr(f->slot, f->attno[c], &slot->tts_isnull[c]);
		ExecStoreVirtualTuple(slot);
		ExecMaterializeSlot(slot);
		return true;
	}
	return false;
}

bool
sift_topn_next(SiftTopN *t, TupleTableSlot *slot)
{
	ItemPointerData tid;
	uint16		part;
	TopFetch   *f;
	const TopEnt *e = !t->all && t->pos < t->n ? &t->res[t->pos] : NULL;
	bool		call_again = false,
				all_dead;

	if (t->go)
		return gord_next(t, slot);
	if (!next_tid(t, &tid, &part))
		return false;
	if (t->emitted++ < t->skip)
		return skipped_row(t, slot);
	if (e != NULL && e->hasv)
	{
		/* the row came from a unit: its columns are the heap row's (an update that keeps the TID's chain keeps the columns of the index) */
		ExecClearTuple(slot);
		for (int c = 0; c < t->nov; c++)
		{
			slot->tts_isnull[c] = t->ovn[(Size) e->oi * t->nov + c];
			slot->tts_values[c] = slot->tts_isnull[c] ? (Datum) 0 : leaf_datum(t->otyp[c], t->ov[(Size) e->oi * t->nov + c]);
		}
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
sift_topn_walk_order(SiftTopN *t)
{
	const WalkBase *wb = t->wb;
	uint32		n = 0;

	if (t->nhead == 0)
		return wb->ord;
	t->wfull = MemoryContextAlloc(GetMemoryChunkContext(t), wb->nsb * sizeof(uint32));
	for (int i = 0; i < t->nhead; i++)
		if (t->whead[i] < wb->nsb)
			t->wfull[n++] = t->whead[i];
	for (uint32 i = 0; i < wb->nsb; i++)
		if (!walk_in_head(t, wb->ord[i]))
			t->wfull[n++] = wb->ord[i];
	return t->wfull;
}

void
sift_topn_add_skipped(SiftTopN *t, int64 n, int64 runs, int64 lazy)
{
	t->skipped += n;
	t->nrun += runs;
	t->nlazy += lazy;
}

int64
sift_topn_heads_used(const SiftTopN *t)
{
	return t->nrhead;
}

int64
sift_topn_runs_read(const SiftTopN *t)
{
	return t->nrun;
}

int64
sift_topn_runs_lazy(const SiftTopN *t)
{
	return t->nlazy;
}

/* units compared with the bar by an exact order code (0), by a prefix (1), as bytes (2) */
int64
sift_topn_ord_units(const SiftTopN *t, int how)
{
	return t->nord[how];
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
		res_push(t, &e, NULL, NULL);
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
	t->gqpos = 0;
	if (t->go)
		sift_gord_rewind(t->go);
	if (t->sort)
		tuplesort_rescan(t->sort);
}

int64
sift_topn_sorted(const SiftTopN *t)
{
	return t->all ? t->nsort : -1;
}
