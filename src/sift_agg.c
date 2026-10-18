/*
 * sift_agg.c - executor of Custom Scan (SiftAgg) and the GUCs of the aggregate pushdown.
 *
 * The scan reads the units of the column source (sift_colsrc.h), one superblock at a time. For every unit it
 *   1. evaluates the row level predicates to a bitmap (the kernels of sift_aggkern.h),
 *   2. for a query that unnests an array leaf, evaluates the element level predicates in vectors of SIFT_AGG_VEC elements,
 *      the row bitmap lifted to the elements through their parent ordinals,
 *   3. keeps only the rows that are visible (sift_colsrc_vet: rows on all-visible pages are, the others are checked in the heap
 *      after the value filters), and
 *   4. adds the surviving tuples to the groups: group keys become dense ids (dictionary codes and integer ranges through
 *      tables built once per dictionary / value range, evaluating the key expression with PostgreSQL's own evaluator on each
 *      distinct value), the ids a group index (direct arrays, a hash table beyond that), and the aggregates accumulate by
 *      group index in tuple order (so float sums do not depend on the SIMD level).
 * The groups are turned into the rows of custom_scan_tlist when the scan has finished; the plan above projects and filters.
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "sift_agg.h"
#include "sift_aggkern.h"
#include "sift_aggspec.h"
#include "sift_colsrc.h"

bool		sift_enable_agg = true;

#define VEC			SIFT_AGG_VEC
#define VECW		SIFT_AGG_VECW
#define PADW		8
#define MAX_RANGE	(1 << 22)		/* values a range table covers */
#define DIRECT_MAX	(1 << 20)		/* groups of the direct arrays */
#define PRESIZE_MIN		4096		/* groups the planner expects above which a single key starts with room for them */
#define LANE_DIRECT_MAX	(1 << 15)	/* a lane table is built and dumped sixteen times: sparse group spaces go to the hash table */
#define NDICT_SLOTS	8

void
sift_agg_init(void)
{
	sift_aggkern_init();
	sift_aggcol_register();
	sift_aggcte_init();
	DefineCustomBoolVariable("sift.enable_agg",
							 "Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)).",
							 NULL, &sift_enable_agg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
}

/* ---------- distinct output values of a key ---------- */

typedef struct OutTable
{
	Oid			collid;
	bool		byval;
	int16		len;
	FmgrInfo	hashfn;
	FmgrInfo	eqfn;
	uint32		n;				/* ids 1..n */
	uint32		cap;
	Datum	   *val;
	uint32	   *hv;
	uint32		nslot;
	uint32	   *slot;			/* id or 0 */
} OutTable;

static void
ot_init(OutTable *t, Oid eqop, Oid typid, Oid collid)
{
	RegProcedure lhs,
				rhs;

	if (!get_op_hash_functions(eqop, &lhs, &rhs))
		elog(ERROR, "sift: no hash function for operator %u", eqop);
	fmgr_info(lhs, &t->hashfn);
	fmgr_info(get_opcode(eqop), &t->eqfn);
	get_typlenbyval(typid, &t->len, &t->byval);
	t->collid = collid;
	t->nslot = 64;
	t->slot = palloc0(t->nslot * sizeof(uint32));
	t->cap = 16;
	t->val = palloc(t->cap * sizeof(Datum));
	t->hv = palloc(t->cap * sizeof(uint32));
}

static void
ot_rehash(OutTable *t)
{
	t->nslot *= 2;
	pfree(t->slot);
	t->slot = palloc0(t->nslot * sizeof(uint32));
	for (uint32 id = 1; id <= t->n; id++)
	{
		uint32		h = t->hv[id - 1] & (t->nslot - 1);

		while (t->slot[h])
			h = (h + 1) & (t->nslot - 1);
		t->slot[h] = id;
	}
}

/* the id (1..) of a non-null output value; the value is copied into the current context when new */
static uint32
ot_id(OutTable *t, Datum d)
{
	uint32		hv = DatumGetUInt32(FunctionCall1Coll(&t->hashfn, t->collid, d)),
				h = hv & (t->nslot - 1);

	while (t->slot[h])
	{
		uint32		id = t->slot[h];

		if (t->hv[id - 1] == hv && DatumGetBool(FunctionCall2Coll(&t->eqfn, t->collid, t->val[id - 1], d)))
			return id;
		h = (h + 1) & (t->nslot - 1);
	}
	if (t->n == t->cap)
	{
		t->cap *= 2;
		t->val = repalloc(t->val, t->cap * sizeof(Datum));
		t->hv = repalloc(t->hv, t->cap * sizeof(uint32));
	}
	t->val[t->n] = datumCopy(d, t->byval, t->len);
	t->hv[t->n] = hv;
	t->slot[h] = ++t->n;
	if (t->n * 2 > t->nslot)
		ot_rehash(t);
	return t->n;
}

/* ---------- tables over a range of integer values ---------- */

typedef struct RangeTab
{
	int32		lo;
	int32		hi;				/* covered values lo..hi; lo > hi: none */
	void	   *tab;			/* esize bytes per value, the entry of lo first */
	int			esize;
	char	   *mem;			/* the allocation: it holds the values plo..phi, so that growing a range is not a copy */
	int32		plo;
	int32		phi;
} RangeTab;

static void
rt_init(RangeTab *r, int esize)
{
	r->lo = 1;
	r->hi = 0;
	r->tab = NULL;
	r->mem = NULL;
	r->esize = esize;
}

/* values the table would cover after adding [lo, hi] */
static int64
rt_span(const RangeTab *r, int32 lo, int32 hi)
{
	bool		had = r->lo <= r->hi;

	return (int64) (had ? Max(r->hi, hi) : hi) - (had ? Min(r->lo, lo) : lo) + 1;
}

/* fill(arg, v, entry) is called for each value newly covered, ascending */
typedef void (*FillFn) (void *arg, int32 v, void *entry);

static void
rt_cover(RangeTab *r, int32 lo, int32 hi, FillFn fill, void *arg)
{
	bool		had = r->lo <= r->hi;
	int32		old_lo = r->lo,
				old_hi = r->hi,
				nlo = had ? Min(r->lo, lo) : lo,
				nhi = had ? Max(r->hi, hi) : hi;
	int64		span = (int64) nhi - nlo + 1;

	if (had && lo >= old_lo && hi <= old_hi)
		return;
	if (span > MAX_RANGE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: a column range of %lld values is too wide for the aggregate scan", (long long) span),
				 errhint("Set sift.enable_agg to off for this query.")));
	if (r->mem == NULL || nlo < r->plo || nhi > r->phi)
	{
		/* twice the room, on the side the range grows (bounded by MAX_RANGE): a range that creeps along is not copied every time */
		int64		room = Min(span * 2, Max(MAX_RANGE, span)) - span,
					plo = nlo - (had && nlo < old_lo ? room : 0),
					phi = nhi + (had && nlo < old_lo ? 0 : room);
		char	   *nt;

		plo = Max(plo, PG_INT32_MIN);
		phi = Min(phi, PG_INT32_MAX);
		nt = MemoryContextAllocZero(CurrentMemoryContext, (phi - plo + 1) * r->esize + SIFT_AGG_PAD);
		if (had)
			memcpy(nt + (int64) (old_lo - plo) * r->esize, r->tab, ((int64) old_hi - old_lo + 1) * r->esize);
		if (r->mem)
			pfree(r->mem);
		r->mem = nt;
		r->plo = (int32) plo;
		r->phi = (int32) phi;
	}
	r->tab = r->mem + (int64) (nlo - r->plo) * r->esize;
	r->lo = nlo;
	r->hi = nhi;
	for (int64 v = nlo; v <= (had ? old_lo - 1 : nhi); v++)
		fill(arg, (int32) v, (char *) r->tab + (v - nlo) * r->esize);
	for (int64 v = had ? old_hi + 1 : nhi + 1; v <= nhi; v++)
		fill(arg, (int32) v, (char *) r->tab + (v - nlo) * r->esize);
}

/* ---------- executor state ---------- */

typedef struct DictSlot
{
	const void *id;
	uint8	  **lut;			/* per predicate id: n + 1 bytes (slot n: the NULL input) */
	uint32	  **kmap;			/* per key: n + 1 key ids */
} DictSlot;

typedef struct PredState
{
	bool		ready;
	bool		cnull;
	int64		ci;
	double		cd;
	bool		in_hasnull;
	int32		in_lo;
	uint32		in_nlut;
	uint8	   *in_lut;			/* a table over [in_lo, in_lo + in_nlut) ... */
	int32	   *in_sorted;		/* ... or, when the values are too far apart for one, the sorted values */
	uint32		in_nsorted;
	ExprState  *expr;			/* SAP_LUT */
	Param	   *param;
	bool		null_result;	/* SAP_LUT: the expression for a NULL input */
	RangeTab	rt;				/* SAP_LUT on an integer leaf: result bytes */
	uint64	   *rowmask;		/* element queries, a row predicate: the unit's rows */
	bool		rowmask_ok;
	int			nop;			/* numeric operand: the comparison of the scaled integers equivalent to the query's (SiftCmpOp) ... */
	sift_i128	cn;				/* ... with this operand, or ... */
	int			konst;			/* ... -1, or 1 / 0: it holds for every / no value */
} PredState;

/* distinct numbers to ids 1.. in order of appearance (open addressing) */
typedef struct IdMap
{
	MemoryContext cxt;
	uint32		n;
	uint32		cap;
	sift_i128  *val;
	uint32		nslot;
	uint32	   *slot;			/* id or 0 */
} IdMap;

typedef struct KeyState
{
	OutTable	ot;				/* distinct output values (ids 1..) */
	ExprState  *expr;
	Param	   *param;
	RangeTab	rt;				/* integer leaves: value -> id */
	bool		ident;			/* integer or float leaf as the key (and the grouping set number): ids are given to the values in order of appearance */
	int64	   *idval;			/* ident: the value of id i + 1 (a float: float_key) */
	uint32		nident;
	uint32		capident;
	uint32		wcap;			/* ident, values too far apart for a table: open addressing value -> id (0 = free), power of 2 */
	int64	   *wval;
	uint32	   *wid;
	uint32		nullid;			/* LUT keys on integer leaves: the id of expr(NULL) */
	uint32		cap;			/* direct mode: ids 0..cap-1 */
	uint32		mult;
	bool		numk;			/* SAK_NUM: ids of the program's values */
	IdMap		im;
} KeyState;

/* open addressing set of (group, value id) pairs; a slot holds the pair with bit 63 set, 0 is empty */
typedef struct DSet
{
	uint64	   *slot;
	int64	   *cnt;			/* counted sets: the times each pair was added, per slot */
	uint64		n;
	uint64		cap;
	bool		counted;
} DSet;

/*
 * The values of an ordered-set aggregate, kept exactly (the multiset PostgreSQL sorts): a histogram of the distinct (group,
 * value) pairs while it fits work_mem, and every value in a tuplesort (work_mem, spilling to disk as the normal plan's sort)
 * once it does not. A value is its order key (float8 in the order of float8_cmp: -0 = 0, NaN last), a group the id of its
 * sorted values (in order of appearance; acc->i of the aggregate that owns the values, which the group renumbering carries).
 */
typedef struct OsaSlot
{
	uint64		key;
	uint32		sid1;			/* sorted-values id + 1, 0: free */
	uint32		cnt;
} OsaSlot;

typedef struct Osa
{
	OsaSlot    *slot;			/* histogram; NULL once spilled */
	uint64		cap;
	uint64		n;
	uint64		maxn;
	uint32		nsid;
	uint64		next;			/* read position in the compacted histogram */
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
} Osa;

typedef struct AggAcc
{
	int64	   *n;				/* tuples / non-null values */
	int64	   *i;
	sift_i128  *w;				/* numeric sums: the exact sum of the scaled values; numeric min and max: the value */
	double	   *d;
	uint64	   *last;			/* count distinct of the rows: the last row counted */
	uint8	   *nf;				/* float sums: a non-finite input was added */
	Datum	   *v;				/* aggregates over distinct values (SAA_M_*): the result, made by mapped_finish */
} AggAcc;

/* ---- combos and lookup joins: the executor state of SiftAggCombo ---- */

/* the distinct values of a dictionary leaf over all the dictionaries of the scan: ids 1.. */
typedef struct Intern
{
	uint32		n;
	uint32		cap;
	uint32		nslot;
	const char **ptr;
	uint32	   *len;
	uint32	   *hv;
	uint32	   *slot;
} Intern;

/* the rows of a joined table, hashed by their key columns; the columns the expressions read are kept as Datums */
typedef struct MapTab
{
	int			nkey;
	int			ncol;
	bool		ktext[SIFT_AGG_MAX_JKEYS];
	uint32		nrow;
	uint32		cap;
	const char **kptr;			/* [row * nkey + k] text keys */
	uint32	   *klen;
	int64	   *kint;			/* integer keys */
	Datum	   *col;			/* [row * ncol + c] */
	bool	   *cnull;
	uint32	   *hv;
	uint32	   *next;			/* row + 1 of the next row of the bucket */
	uint32		nslot;
	uint32	   *head;
} MapTab;

typedef struct ComboEnt
{
	uint32		nout;			/* output rows of the tuple: its matches, one row of NULLs (LEFT JOIN) or none (INNER) */
	uint32		base;			/* first output row */
} ComboEnt;

typedef struct ComboSt
{
	Intern		in[SIFT_AGG_MAX_CLEAVES];
	MapTab	   *map;
	ExprState **slot;
	uint32		nent;
	uint32		entcap;
	uint64	   *vid;			/* [ent * SIFT_AGG_MAX_CLEAVES]: 0 NULL, else the value's id in `in` / the integer + 1 */
	ComboEnt   *ent;
	uint32		nht;
	uint32	   *ht;				/* ent + 1 */
	uint32		nout;
	uint32		outcap;
	uint32	   *val;			/* [out * nslots + slot]: key id (0 NULL) or boolean */
} ComboSt;

/* the tuples of a chunk that reach the aggregates: positions in the chunk, element and row ordinals, and the output row of every join combo */
typedef struct Tup
{
	int			n;
	uint32	   *sel;
	uint32	   *tidx;
	uint32	   *ridx;
	uint32	   *cout[SIFT_AGG_MAX_COMBOS];
} Tup;

/* the state of one arm of a two level query (SiftAggArm): per tuple of the chunk, and per outer group the inner aggregates of the current row */
typedef struct ArmSt
{
	uint32	   *gid;
	uint8	   *ok;				/* the arm's filters hold */
	uint8	  **iok;			/* per inner aggregate: it takes the tuple */
	double	  **dx;				/* its float8 value */
	int64	  **ix;				/* its integer value */
	uint64	   *ord;			/* per outer group: the row its cell holds, UINT64_MAX none */
	int64	  **n;				/* per inner aggregate and group */
	int64	  **i;
	double	  **d;
	uint8	  **nf;
	double	   *amax;			/* per inner aggregate: the largest absolute float input */
} ArmSt;

typedef struct Touch
{
	uint32		arm;
	uint32		g;
} Touch;

typedef struct AggExec
{
	CustomScanState css;
	SiftAggSpec *spec;
	Relation	heap;
	bool		heap_opened;
	Relation	index;
	SiftSrcSource *src;
	const SiftSrcLayout *lay;
	int			nbase;			/* leaves of the source: the ones after are SiftAggSpec.vleaf */
	struct
	{
		int			key;		/* leaves of the object: its "key" and "value" */
		int			val;
		const void *dict;		/* the dictionary of the last unit and the code of the key in it */
		uint32		code;
	}			vl[SIFT_AGG_MAX_VLEAVES];
	MemoryContext cxt;			/* the scan */
	MemoryContext tcxt;			/* the group table: keys, groups, aggregate states (reset by table_reset) */
	MemoryContext ucxt;			/* one unit */
	ExprContext *lutcx;
	ParamExecData lutparams[SIFT_AGG_MAX_PARAMS];
	ComboSt    *cs;
	int			clbase[SIFT_AGG_MAX_COMBOS];	/* per combo: its first dictionary map slot (after the keys and aggregates) */
	int			ncl;
	int			nj;				/* join combos, in the order the stages apply them */
	int			jcombo[SIFT_AGG_MAX_JOINS];
	MemoryContext comcxt;			/* temporaries of one combo entry */
	ArmSt	   *arm;
	uint32		arm_mask;		/* the keys the tuples are grouped by: all, or an arm's, or a grouping set's */
	int			curset;			/* the grouping set being aggregated */
	uint32		gen;			/* changes when the group indexes are renumbered */
	Touch	   *touched;		/* the cells of the current row */
	uint32		ntouch;
	uint32		captouch;
	uint64		cur_order;
	PredState  *ps;
	KeyState   *ks;
	DictSlot	dslot[NDICT_SLOTS];
	int			dnext;
	bool		started;
	/* groups */
	bool		hash;
	uint32		direct_max;		/* groups above which the table is a hash table */
	uint32		ngroups;		/* hash mode: groups made */
	uint32		gcap;			/* allocated groups */
	int64	   *seen;
	AggAcc	   *acc;
	uint32	   *hkey;			/* hash mode: SIFT_AGG_MAX_KEYS ids per group */
	uint32		hkcap;
	uint32	   *hslot;
	uint32		nhslot;
	DSet	   *dset;			/* count distinct: per aggregate, the (group, value) pairs seen */
	OutTable   *aot;			/* count distinct of a dictionary leaf: the distinct values over all dictionaries */
	double	   *amax;			/* avg(float8): the largest absolute input */
	IdMap	   *amap;			/* count(DISTINCT numeric): the ids of the values */
	Osa		  **osa;			/* ordered-set aggregates: the values of the aggregate (shared by those on one argument) */
	double	  **oval;			/* ... and their results by sorted-values id */
	int			n_osa_spilled;
	/* output */
	uint32	   *glist;
	uint32		nglist;
	uint32		gpos;
	/* items of work, lanes and workers (run_scan) */
	struct ScanShared *shared;
	struct AggExec *gtab;		/* the table of the result: this one, or (lanes) its own view */
	bool		lanes;			/* this table is one of the lanes */
	char	  **pending;		/* lanes: the partial tables that wait for the lanes before them */
	uint32		next_lane;		/* lanes: the next one to combine */
	struct AggPar *par;			/* workers launched by the leader */
	SiftSrcPrune *prune;		/* the zone tests of the conjuncts */
	int			nprune;
	List	   *pack;			/* the plan's spec, for the workers */
	ParamListInfo params;
	Snapshot	snap;
	struct shm_mq_handle *mqh;	/* in a worker: the queue to the leader */
	/* the unit */
	SiftSrcUnit unit;
	SiftSrcVec *uvec;
	bool	   *uvec_ok;
	uint32	  **gofs;			/* per array leaf: elem_offs of the unit, NULL when not built yet */
	int			qlevel;			/* 1: element queries */
	uint32	   *parent;
	uint32	   *offs;
	int			nelem;
	uint64		row_base;		/* unique number of the unit's first row */
	/* the tuples being aggregated (the arrays of cur) */
	Tup			t0;				/* the chunk's selection */
	Tup			stage[SIFT_AGG_MAX_JOINS];	/* the output of each join stage that repeats or drops tuples */
	Tup		   *cur;
	int			cstart;			/* the chunk: [cstart, cstart + cn) of the unit's rows / elements */
	int			cn;
	uint32	   *sel;
	uint32	   *tidx;
	uint32	   *ridx;
	uint32	   *seid[SIFT_AGG_MAX_JOINS];	/* the entries of the tuples a stage that repeats them is working on */
	/* scratch of combos */
	uint64	   *cvid[SIFT_AGG_MAX_CLEAVES];
	uint32	   *cfree[SIFT_AGG_MAX_COMBOS];	/* output row per tuple of a combo without a join */
	bool		cfree_ok[SIFT_AGG_MAX_COMBOS];
	uint32	   *peid;
	uint32	   *pti;
	uint32	   *pri;
	uint32	   *gid;
	uint32	   *kids;			/* hash mode: ids per key, SIFT_AGG_MAX_KEYS * VEC */
	uint32	   *cu32;
	uint64	   *c64;
	uint8	   *okb;
	uint8	   *nb;
	double	   *vstk;			/* float8 aggregate arguments: SIFT_AGG_MAX_VX vectors */
	sift_i128  *nstk;			/* numeric programs: SIFT_AGG_MAX_VX vectors */
	uint32	   *pix;			/* the data index of the rows a predicate evaluates: start + i */
	uint64	   *okm;
	uint64	   *tmpm;
	uint64	   *tmpm2;
	uint32	   *tmpu32;
	/* counters */
	int64		n_units;
	int64		n_rows;
	int64		n_tuples;
	int64		n_groups;
} AggExec;

/* the parent of the expressions: a parallel worker has no plan state */
#define EXPR_PARENT(ex)	((ex)->css.ss.ps.state ? &(ex)->css.ss.ps : NULL)

/* ---------- value conversions ---------- */

/* The group of a float: its bits, with the one zero and the one NaN of float8_eq / hashfloat8 (-0 = 0, NaN = NaN) */
static int64
float_key(double d)
{
	int64		bits;

	d = d == 0 ? 0.0 : isnan(d) ? get_float8_nan() : d;
	memcpy(&bits, &d, sizeof(bits));
	return bits;
}

static double
float_from_key(int64 bits)
{
	double		d;

	memcpy(&d, &bits, sizeof(d));
	return d;
}

static Datum
leaf_datum(Oid typid, int64 iv)
{
	switch (typid)
	{
		case BOOLOID:
			return BoolGetDatum(iv != 0);
		case INT2OID:
			return Int16GetDatum((int16) iv);
		case INT4OID:
			return Int32GetDatum((int32) iv);
		case DATEOID:
			return DateADTGetDatum((DateADT) iv);
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return Int64GetDatum(iv);
		case FLOAT4OID:			/* the bits of a float key (float_key) */
			return Float4GetDatum((float4) float_from_key(iv));
		case FLOAT8OID:
			return Float8GetDatum(float_from_key(iv));
		default:
			elog(ERROR, "sift: unexpected integer leaf type %u", typid);
	}
	return (Datum) 0;
}

/* a dictionary value as the Datum of type typid */
static Datum
text_datum(Oid typid, int32 typmod, const char *bytes, uint32 len)
{
	Oid			infn,
				ioparam;
	FmgrInfo	f;

	if (typid == TEXTOID || typid == VARCHAROID)
		return PointerGetDatum(cstring_to_text_with_len(bytes, len));
	getTypeInputInfo(typid, &infn, &ioparam);
	fmgr_info(infn, &f);
	return InputFunctionCall(&f, pnstrdup(bytes, len), ioparam, typmod);
}

static bool
param_walker(Node *n, Param **found)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		*found = (Param *) n;
		return true;
	}
	return expression_tree_walker(n, param_walker, found);
}

/* the placeholder Param 0 of an expression: the type its input has */
static Param *
find_param(Node *expr)
{
	Param	   *p = NULL;

	param_walker(expr, &p);
	if (p == NULL)
		elog(ERROR, "sift: aggregate plan expression without its input");
	return p;
}

/* ---------- evaluation of the single input expressions ---------- */

static Datum
eval_with(AggExec *ex, ExprState *e, Datum in, bool in_null, bool *isnull)
{
	ex->lutparams[0].value = in;
	ex->lutparams[0].isnull = in_null;
	return ExecEvalExprSwitchContext(e, ex->lutcx, isnull);
}

static bool
eval_bool(AggExec *ex, ExprState *e, Datum in, bool in_null)
{
	bool		isnull;
	Datum		r = eval_with(ex, e, in, in_null, &isnull);
	bool		res = !isnull && DatumGetBool(r);

	ResetExprContext(ex->lutcx);
	return res;
}

/* ---------- dictionaries ---------- */

static DictSlot *
dict_slot(AggExec *ex, const SiftSrcDict *d)
{
	DictSlot   *s;
	int			np = Max(ex->spec->npreds_total, 1),
				nk = Max(ex->spec->nkeys + ex->spec->naggs + ex->ncl, 1);

	for (int i = 0; i < NDICT_SLOTS; i++)
		if (ex->dslot[i].id == d->id && d->id != NULL)
			return &ex->dslot[i];
	s = &ex->dslot[ex->dnext];
	ex->dnext = (ex->dnext + 1) % NDICT_SLOTS;
	if (s->lut == NULL)
	{
		s->lut = MemoryContextAllocZero(ex->cxt, np * sizeof(uint8 *));
		s->kmap = MemoryContextAllocZero(ex->cxt, nk * sizeof(uint32 *));
	}
	else
	{
		for (int i = 0; i < np; i++)
			if (s->lut[i])
				pfree(s->lut[i]);
		for (int i = 0; i < nk; i++)
			if (s->kmap[i])
				pfree(s->kmap[i]);
		memset(s->lut, 0, np * sizeof(uint8 *));
		memset(s->kmap, 0, nk * sizeof(uint32 *));
	}
	s->id = d->id;
	return s;
}

static uint8 *
dict_lut(AggExec *ex, DictSlot *s, const SiftSrcDict *d, SiftAggPred *p)
{
	uint8	   *lut = s->lut[p->id];
	PredState  *st = &ex->ps[p->id];
	MemoryContext old;

	if (lut)
		return lut;
	lut = MemoryContextAlloc(ex->cxt, d->n + 1 + SIFT_AGG_PAD);
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	for (uint32 c = 0; c < d->n; c++)
	{
		Datum		v = text_datum(st->param->paramtype, st->param->paramtypmod, d->bytes + d->off[c],
								   d->off[c + 1] - d->off[c]);

		lut[c] = eval_bool(ex, st->expr, v, false);
	}
	lut[d->n] = eval_bool(ex, st->expr, (Datum) 0, true);
	MemoryContextSwitchTo(old);
	s->lut[p->id] = lut;
	return lut;
}

/* the key id of a distinct input value of a key whose input is a dictionary entry or an integer */
static uint32
key_value_id(AggExec *ex, int ki, Datum in, bool in_null)
{
	KeyState   *k = &ex->ks[ki];
	SiftAggKey *sk = &ex->spec->key[ki];
	Datum		out = in;
	bool		isnull = in_null;
	uint32		id;
	MemoryContext old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);

	if (sk->kind == SAK_LUT)
		out = eval_with(ex, k->expr, in, in_null, &isnull);
	if (isnull)
		id = 0;
	else
	{
		MemoryContextSwitchTo(ex->tcxt);
		id = ot_id(&k->ot, out);
	}
	MemoryContextSwitchTo(old);
	ResetExprContext(ex->lutcx);
	return id;
}

static uint32 *
dict_kmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ki)
{
	KeyState   *k = &ex->ks[ki];
	uint32	   *map = s->kmap[ki];
	MemoryContext old;

	if (map)
		return map;
	map = MemoryContextAlloc(ex->tcxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	for (uint32 c = 0; c < d->n; c++)
	{
		Datum		v = text_datum(k->param->paramtype, k->param->paramtypmod, d->bytes + d->off[c],
								   d->off[c + 1] - d->off[c]);

		map[c] = key_value_id(ex, ki, v, false);
		ResetExprContext(ex->lutcx);
	}
	map[d->n] = key_value_id(ex, ki, (Datum) 0, true);
	MemoryContextSwitchTo(old);
	s->kmap[ki] = map;
	return map;
}

/* the id (1..) over all dictionaries of each entry of d, for the values count(DISTINCT) of aggregate ai counts */
static uint32 *
dict_amap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ai)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	int			slot = ex->spec->nkeys + ai;
	uint32	   *map = s->kmap[slot];
	MemoryContext old;

	if (map)
		return map;
	map = MemoryContextAlloc(ex->tcxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	old = MemoryContextSwitchTo(ex->tcxt);
	for (uint32 c = 0; c < d->n; c++)
		map[c] = ot_id(&ex->aot[ai], text_datum(a->argtype, -1, d->bytes + d->off[c], d->off[c + 1] - d->off[c]));
	MemoryContextSwitchTo(old);
	map[d->n] = 0;
	s->kmap[slot] = map;
	return map;
}

/* ---------- unit data ---------- */

static const SiftSrcVec *get_vec(AggExec *ex, int leaf);

static void
extend_layout(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcLayout *lay = MemoryContextAlloc(ex->cxt, sizeof(SiftSrcLayout));

	ex->nbase = ex->lay->nleaves;
	if (s->nvleaf == 0)
		return;
	*lay = *ex->lay;
	lay->leaf = MemoryContextAllocZero(ex->cxt, (ex->nbase + s->nvleaf) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, ex->lay->leaf, ex->nbase * sizeof(SiftSrcLeaf));
	for (int j = 0; j < s->nvleaf; j++)
	{
		SiftSrcLeaf *v = &lay->leaf[ex->nbase + j];

		for (int i = 0; i < ex->nbase; i++)
			if (lay->leaf[i].parent == s->vleaf[j].obj)
				*(strcmp(lay->leaf[i].name, "key") == 0 ? &ex->vl[j].key : &ex->vl[j].val) = i;
		v->id = ex->nbase + j;
		v->attno = lay->leaf[s->vleaf[j].obj].attno;
		v->parent = -1;
		v->kind = SCL_SCALAR;
		v->vtype = lay->leaf[ex->vl[j].val].vtype;
		v->typid = lay->leaf[ex->vl[j].val].typid;
	}
	lay->nleaves += s->nvleaf;
	ex->lay = lay;
}

static uint32
dict_code(const SiftSrcDict *d, const char *key)
{
	size_t		len = strlen(key);

	for (uint32 c = 0; c < d->n; c++)
		if (d->off[c + 1] - d->off[c] == len && memcmp(d->bytes + d->off[c], key, len) == 0)
			return c;
	return PG_UINT32_MAX;
}

/* the leaf of a SiftAggVLeaf: per row the value of its element with the key, NULL where it has none */
static void
read_vleaf(AggExec *ex, int leaf, SiftSrcVec *out)
{
	int			j = leaf - ex->nbase,
				nrow = ex->unit.nrow;
	const SiftSrcVec *cnt = get_vec(ex, ex->spec->vleaf[j].obj),
			   *key = get_vec(ex, ex->vl[j].key),
			   *val = get_vec(ex, ex->vl[j].val);
	size_t		esz = ex->lay->leaf[leaf].vtype == SCV_I64 || ex->lay->leaf[leaf].vtype == SCV_F64 ? 8 : 4;
	const int32 *c = cnt->data;
	const uint32 *kc = key->data;
	char	   *data = (char *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAlloc(ex->ucxt, nrow * esz + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));
	uint8	   *isnull = MemoryContextAlloc(ex->ucxt, nrow + SIFT_AGG_PAD);
	uint32		e = 0;

	if (ex->vl[j].dict != key->dict->id)
	{
		ex->vl[j].dict = key->dict->id;
		ex->vl[j].code = dict_code(key->dict, ex->spec->vleaf[j].key);
	}
	memset(isnull, 1, nrow);
	for (int r = 0; r < nrow; r++)
		for (int n = (cnt->isnull && cnt->isnull[r]) ? 0 : Max(c[r], 0); n > 0; n--, e++)
			if (kc[e] == ex->vl[j].code)
			{
				memcpy(data + r * esz, (const char *) val->data + e * esz, esz);
				isnull[r] = val->isnull ? val->isnull[e] : 0;
			}
	out->data = data;
	out->isnull = isnull;
	out->dict = val->dict;
	out->n = nrow;
}

static const SiftSrcVec *
get_vec(AggExec *ex, int leaf)
{
	if (!ex->uvec_ok[leaf])
	{
		if (leaf >= ex->nbase)
			read_vleaf(ex, leaf, &ex->uvec[leaf]);
		else
			sift_colsrc_read(ex->src, &ex->unit, leaf, &ex->uvec[leaf]);
		ex->uvec_ok[leaf] = true;
	}
	return &ex->uvec[leaf];
}

static int
leaf_lvl(AggExec *ex, int leaf)
{
	return ex->lay->leaf[leaf].parent >= 0;
}

/* row r of the unit owns the elements [offs[r], offs[r + 1]) of the children of the array leaf */
static uint32 *
elem_offs(AggExec *ex, int leaf)
{
	if (ex->gofs[leaf] == NULL)
	{
		const SiftSrcVec *cnt = get_vec(ex, leaf);
		const int32 *c = cnt->data;
		int			nrow = ex->unit.nrow;
		uint32	   *offs = MemoryContextAlloc(ex->ucxt, (nrow + 1) * sizeof(uint32)),
					m = 0;

		for (int r = 0; r < nrow; r++)
		{
			offs[r] = m;
			if (!(cnt->isnull && cnt->isnull[r]) && c[r] > 0)
				m += c[r];
		}
		offs[nrow] = m;
		ex->gofs[leaf] = offs;
	}
	return ex->gofs[leaf];
}

/* ---------- numbers ---------- */

static void
idmap_init(IdMap *m)
{
	m->cxt = CurrentMemoryContext;
	m->cap = 64;
	m->nslot = 128;
	m->val = palloc(m->cap * sizeof(sift_i128));
	m->slot = palloc0(m->nslot * sizeof(uint32));
}

static inline uint32
idmap_hash(sift_i128 v)
{
	uint64		x = (uint64) v * UINT64CONST(0x9E3779B97F4A7C15) ^ (uint64) (v >> 64) * UINT64CONST(0xC2B2AE3D27D4EB4F);

	x ^= x >> 31;
	return (uint32) ((x * UINT64CONST(0xD6E8FEB86659FD93)) >> 32);
}

static uint32
idmap_id(IdMap *m, sift_i128 v)
{
	uint32		mask = m->nslot - 1,
				h = idmap_hash(v) & mask;

	for (; m->slot[h]; h = (h + 1) & mask)
		if (m->val[m->slot[h] - 1] == v)
			return m->slot[h];
	if (m->n == m->cap)
	{
		m->cap *= 2;
		m->val = repalloc(m->val, m->cap * sizeof(sift_i128));
	}
	m->val[m->n] = v;
	m->slot[h] = ++m->n;
	if (m->n * 2 > m->nslot)
	{
		pfree(m->slot);
		m->nslot *= 2;
		m->slot = MemoryContextAllocZero(m->cxt, m->nslot * sizeof(uint32));
		for (uint32 id = 1; id <= m->n; id++)
		{
			for (h = idmap_hash(m->val[id - 1]) & (m->nslot - 1); m->slot[h]; h = (h + 1) & (m->nslot - 1))
				;
			m->slot[h] = id;
		}
	}
	return m->n;
}

/* v / d rounded as the program step asks: ROUND half away from zero as numeric_round, TRUNC toward zero, FLOOR, CEIL */
static sift_i128
num_div(SiftAggNxOp op, sift_i128 v, sift_i128 d)
{
	sift_i128	q = v / d,
				r = v % d,
				ar = r < 0 ? -r : r;

	switch (op)
	{
		case SNX_ROUND:
			return ar >= d - ar ? q + (v < 0 ? -1 : 1) : q;
		case SNX_FLOOR:
			return r < 0 ? q - 1 : q;
		case SNX_CEIL:
			return r > 0 ? q + 1 : q;
		default:
			return q;
	}
}

#define NSTK(i)	(ex->nstk + (Size) (i) * (VEC + PADW))

/*
 * The values of a numeric program for nsel tuples, scaled by 10^p->scale. The planner proved that no step overflows, but the
 * data slot of a NULL leaf holds anything: its tuple's value is garbage and is dropped through the null flags (isnull, when given).
 * iq indexes the data of the leaves at the query's level, ir those of the row level.
 */
static const sift_i128 *
eval_nx(AggExec *ex, const SiftAggNum *p, int nsel, const uint32 *iq, const uint32 *ir, uint8 *isnull)
{
	int			sp = 0;

	if (ex->nstk == NULL)
		ex->nstk = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * (VEC + PADW) * sizeof(sift_i128));
	if (isnull)
		memset(isnull, 0, nsel);
	for (int k = 0; k < p->n; k++)
	{
		const SiftAggNx *x = &p->step[k];
		sift_i128  *top = NSTK(sp),
					m,
					d;

		switch (x->op)
		{
			case SNX_LEAF:
				{
					const SiftSrcVec *v = get_vec(ex, x->leaf);
					const uint32 *idx = leaf_lvl(ex, x->leaf) == ex->qlevel ? iq : ir;

					if (ex->lay->leaf[x->leaf].vtype == SCV_I32)
						for (int j = 0; j < nsel; j++)
							top[j] = ((const int32 *) v->data)[idx[j]];
					else
						for (int j = 0; j < nsel; j++)
							top[j] = ((const int64 *) v->data)[idx[j]];
					if (isnull && v->isnull)
						for (int j = 0; j < nsel; j++)
							isnull[j] |= v->isnull[idx[j]];
					sp++;
				}
				break;
			case SNX_CONST:
				for (int j = 0; j < nsel; j++)
					top[j] = x->c;
				sp++;
				break;
			case SNX_ADD:
			case SNX_SUB:
				d = sift_pow10(x->a);
				m = x->op == SNX_ADD ? sift_pow10(x->b) : -sift_pow10(x->b);
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 2)[j] = NSTK(sp - 2)[j] * d + NSTK(sp - 1)[j] * m;
				sp--;
				break;
			case SNX_MUL:
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 2)[j] *= NSTK(sp - 1)[j];
				sp--;
				break;
			case SNX_NEG:
			case SNX_ABS:
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 1)[j] = (x->op == SNX_ABS && NSTK(sp - 1)[j] >= 0) ? NSTK(sp - 1)[j] : -NSTK(sp - 1)[j];
				break;
			default:
				d = sift_pow10(x->a);
				m = sift_pow10(x->b);
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 1)[j] = num_div(x->op, NSTK(sp - 1)[j], d) * m;
				break;
		}
	}
	return ex->nstk;
}

/* ---------- predicates ---------- */

static int
int32_cmp_asc(const void *a, const void *b)
{
	return pg_cmp_s32(*(const int32 *) a, *(const int32 *) b);
}

/*
 * c (a numeric) floored (up: ceilinged) to a multiple of 10^-scale, as that integer: PostgreSQL's own numeric_mul, numeric_floor and
 * numeric_ceil do the arithmetic. -1 / 1 when it is below / above every integer within lim, which is where NaN (numeric order puts it
 * above every number), the infinities and the digits the scaled values cannot hold go.
 */
static int
scaled_bound(Datum c, int scale, bool up, sift_i128 lim, sift_i128 *out)
{
	Datum		unit = DirectFunctionCall3(numeric_in, CStringGetDatum(psprintf("1e%d", scale)), ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(-1)),
				t = DirectFunctionCall2(numeric_mul, c, unit);
	char	   *txt = DatumGetCString(DirectFunctionCall1(numeric_out, DirectFunctionCall1(up ? numeric_ceil : numeric_floor, t)));
	int			ignored;

	if (strcmp(txt, "-Infinity") == 0)
		return -1;
	if (strcmp(txt, "NaN") == 0 || strcmp(txt, "Infinity") == 0 || !sift_parse_decimal(txt, out, &ignored))
		return txt[0] == '-' ? -1 : 1;
	return *out > lim ? 1 : *out < -lim ? -1 : 0;
}

/*
 * `value op c` for integers scaled by 10^scale, c a numeric of any scale: the comparison st->nop with st->cn that holds for exactly the
 * same values (st->konst: the one result of every value). x = c needs c to be a multiple of the unit, x < c is x < ceil(c), x <= c is
 * x <= floor(c), x > c is x > floor(c), x >= c is x >= ceil(c).
 */
static void
numeric_operand(PredState *st, int op, int scale, sift_i128 lim, Datum c)
{
	sift_i128	fl,
				ce,
				v;
	int			sf = scaled_bound(c, scale, false, lim, &fl),
				sc = scaled_bound(c, scale, true, lim, &ce),
				side;

	st->konst = -1;
	switch (op)
	{
		case SCMP_EQ:
		case SCMP_NE:
			if (sf == 0 && sc == 0 && fl == ce)
			{
				st->nop = op;
				st->cn = fl;
			}
			else
				st->konst = op == SCMP_NE;
			return;
		case SCMP_LT:
		case SCMP_GE:
			side = sc;
			v = ce;
			break;
		default:
			side = sf;
			v = fl;
	}
	if (side)
		st->konst = (op == SCMP_LT || op == SCMP_LE) == (side > 0);
	else
	{
		st->nop = op;
		st->cn = v;
	}
}

static void
eval_operand(AggExec *ex, SiftAggPred *p, PredState *st)
{
	bool		isnull;
	Datum		d;
	ExprState  *e;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
	d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
	st->cnull = isnull;
	if ((p->kind == SAP_CMP || p->kind == SAP_NCMP) && !isnull)
	{
		if (p->argtype == NUMERICOID)
		{
			if (p->kind == SAP_NCMP)
				numeric_operand(st, p->op, p->num.scale, sift_pow10(SIFT_NUM_MAX_DIGITS), d);
			else
				numeric_operand(st, p->op, ex->lay->leaf[p->leaf].scale, PG_INT64_MAX, d);
		}
		else if (p->argtype == FLOAT4OID)
			st->cd = DatumGetFloat4(d);
		else if (p->argtype == FLOAT8OID)
			st->cd = DatumGetFloat8(d);
		else
			st->ci = sift_aggspec_operand_int(p->argtype, d);
	}
	else if (p->kind == SAP_IN && !isnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(d);
		Datum	   *el;
		bool	   *nl;
		int			ne,
					nv = 0;
		int16		len;
		bool		byval;
		char		align;
		int64		lo = PG_INT64_MAX,
					hi = PG_INT64_MIN;
		int32	   *vals;

		get_typlenbyvalalign(p->argtype, &len, &byval, &align);
		deconstruct_array(arr, p->argtype, len, byval, align, &el, &nl, &ne);
		vals = palloc(Max(ne, 1) * sizeof(int32));
		for (int i = 0; i < ne; i++)
		{
			int64		v;

			if (nl[i])
			{
				st->in_hasnull = true;
				continue;
			}
			v = sift_aggspec_operand_int(p->argtype, el[i]);
			if (v < PG_INT32_MIN || v > PG_INT32_MAX)
				continue;		/* no int4 value equals it */
			vals[nv++] = (int32) v;
			lo = Min(lo, v);
			hi = Max(hi, v);
		}
		if (nv > 0)
		{
			int64		span = hi - lo + 1;

			if (span > MAX_RANGE)
			{
				qsort(vals, nv, sizeof(int32), int32_cmp_asc);
				st->in_sorted = vals;
				st->in_nsorted = nv;
			}
			else
			{
			st->in_lo = (int32) lo;
			st->in_nlut = (uint32) span;
			st->in_lut = palloc0(span + SIFT_AGG_PAD);
			for (int i = 0; i < nv; i++)
				st->in_lut[vals[i] - lo] = 1;
			}
		}
		else
		{
			st->in_lo = 0;
			st->in_nlut = 0;
			st->in_lut = palloc0(SIFT_AGG_PAD);
		}
	}
	MemoryContextSwitchTo(old);
	st->ready = true;
}

static void
fill_ones(uint64 *m, int n)
{
	int			nw = (n + 63) / 64;

	for (int i = 0; i < nw; i++)
		m[i] = ~UINT64CONST(0);
	if (n & 63)
		m[nw - 1] = (UINT64CONST(1) << (n & 63)) - 1;
}

static void
clear_words(uint64 *m, int n)
{
	memset(m, 0, ((n + 63) / 64) * sizeof(uint64));
}

/* rows whose leaf value is NULL are not in out */
static void
clear_nulls(AggExec *ex, const SiftSrcVec *v, int start, int n, uint64 *out)
{
	if (v->isnull)
	{
		sift_aggk->bytes_mask(ex->tmpm, v->isnull + start, n);
		sift_aggk->andnot_w(out, ex->tmpm, (n + 63) / 64);
	}
}

static void
eval_cmp(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];
	SiftCmpOp	op = (SiftCmpOp) p->op;

	if (st->cnull)
	{
		clear_words(out, n);
		return;
	}
	switch (lf->vtype)
	{
		case SCV_I32:
			{
				int64		c = st->ci;

				if (c > PG_INT32_MAX || c < PG_INT32_MIN)
				{
					/* beyond int32 the comparison is the same for every value */
					bool		above = c > PG_INT32_MAX,
								res;

					switch (op)
					{
						case SCMP_EQ:
							res = false;
							break;
						case SCMP_NE:
							res = true;
							break;
						case SCMP_LT:
						case SCMP_LE:
							res = above;
							break;
						default:
							res = !above;
					}
					if (res)
						fill_ones(out, n);
					else
						clear_words(out, n);
				}
				else
					sift_aggk->cmp_i32(out, (const int32 *) v->data + start, n, op, (int32) c);
			}
			break;
		case SCV_I64:
			if (lf->typid != NUMERICOID)
				sift_aggk->cmp_i64(out, (const int64 *) v->data + start, n, op, st->ci);
			else if (st->konst >= 0)
			{
				if (st->konst)
					fill_ones(out, n);
				else
					clear_words(out, n);
			}
			else
				sift_aggk->cmp_i64(out, (const int64 *) v->data + start, n, (SiftCmpOp) st->nop, (int64) st->cn);
			break;
		case SCV_F64:
			sift_aggk->cmp_f64(out, (const double *) v->data + start, n, op, st->cd);
			break;
		default:
			elog(ERROR, "sift: comparison on a dictionary leaf");
	}
	clear_nulls(ex, v, start, n, out);
}

/* bit (sign + 1) of the mask of a SiftCmpOp: the comparison holds when the three-way result has that sign */
static const uint8 cmp_signs[] = {2, 5, 1, 3, 4, 6};

/* the program's values against the scaled operand, the NULL rows out */
static void
eval_ncmp(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const sift_i128 *x;

	clear_words(out, n);
	if (st->cnull || st->konst == 0)
		return;
	for (int i = 0; i < n; i++)
		ex->pix[i] = start + i;
	x = eval_nx(ex, &p->num, n, ex->pix, ex->pix, ex->nb);
	for (int i = 0; i < n; i++)
		if (!ex->nb[i] && (st->konst == 1 || (cmp_signs[st->nop] >> ((x[i] > st->cn) - (x[i] < st->cn) + 1) & 1)))
			out[i >> 6] |= UINT64CONST(1) << (i & 63);
}

/* out = ~out, then the NULL rows out */
static void
negate(AggExec *ex, uint64 *out, int n)
{
	uint64	   *all = ex->tmpm2;

	fill_ones(all, n);
	sift_aggk->andnot_w(all, out, (n + 63) / 64);
	memcpy(out, all, ((n + 63) / 64) * sizeof(uint64));
}

static void
eval_in(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);

	if (st->cnull || (p->op && st->in_hasnull))
	{
		clear_words(out, n);	/* x IN NULL, x NOT IN (.., NULL): never true */
		return;
	}
	if (st->in_sorted)
	{
		const int32 *x = (const int32 *) v->data + start;

		clear_words(out, n);
		for (int i = 0; i < n; i++)
			if (bsearch(&x[i], st->in_sorted, st->in_nsorted, sizeof(int32), int32_cmp_asc))
				out[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	else
		sift_aggk->lut_i32(out, (const int32 *) v->data + start, n, st->in_lo, st->in_nlut, st->in_lut);
	if (p->op)
		negate(ex, out, n);
	clear_nulls(ex, v, start, n, out);
}

static void
eval_null(AggExec *ex, SiftAggPred *p, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);

	if (v->isnull == NULL)
	{
		if (p->op)
			fill_ones(out, n);
		else
			clear_words(out, n);
		return;
	}
	sift_aggk->bytes_mask(out, v->isnull + start, n);
	if (p->op)
		negate(ex, out, n);
}

/* ---------- combos and lookup joins ---------- */

#define CHUNK_PAD	(VEC + PADW)

/* the id (1..) of a value among the distinct values of a dictionary leaf; the bytes are copied when new */
static uint32
intern_id(AggExec *ex, Intern *t, const char *b, uint32 len)
{
	uint32		hv = hash_bytes((const unsigned char *) b, len),
				h;
	char	   *cp;

	if (t->nslot == 0)
	{
		t->nslot = 64;
		t->cap = 16;
		t->slot = MemoryContextAllocZero(ex->cxt, t->nslot * sizeof(uint32));
		t->ptr = MemoryContextAlloc(ex->cxt, t->cap * sizeof(char *));
		t->len = MemoryContextAlloc(ex->cxt, t->cap * sizeof(uint32));
		t->hv = MemoryContextAlloc(ex->cxt, t->cap * sizeof(uint32));
	}
	h = hv & (t->nslot - 1);
	for (uint32 id; (id = t->slot[h]) != 0; h = (h + 1) & (t->nslot - 1))
		if (t->hv[id - 1] == hv && t->len[id - 1] == len && memcmp(t->ptr[id - 1], b, len) == 0)
			return id;
	if (t->n == t->cap)
	{
		t->cap *= 2;
		t->ptr = repalloc(t->ptr, t->cap * sizeof(char *));
		t->len = repalloc(t->len, t->cap * sizeof(uint32));
		t->hv = repalloc(t->hv, t->cap * sizeof(uint32));
	}
	cp = MemoryContextAlloc(ex->cxt, len + 1);
	memcpy(cp, b, len);
	cp[len] = '\0';
	t->ptr[t->n] = cp;
	t->len[t->n] = len;
	t->hv[t->n] = hv;
	t->slot[h] = ++t->n;
	if (t->n * 2 > t->nslot)
	{
		pfree(t->slot);
		t->nslot *= 2;
		t->slot = MemoryContextAllocZero(ex->cxt, t->nslot * sizeof(uint32));
		for (uint32 id = 1; id <= t->n; id++)
		{
			h = t->hv[id - 1] & (t->nslot - 1);
			while (t->slot[h])
				h = (h + 1) & (t->nslot - 1);
			t->slot[h] = id;
		}
	}
	return t->n;
}

/* the ids over all dictionaries of the entries of d, for dictionary leaf p of combo ci */
static uint32 *
dict_cmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ci, int p)
{
	int			slot = ex->spec->nkeys + ex->spec->naggs + ex->clbase[ci] + p;
	uint32	   *map = s->kmap[slot];

	if (map)
		return map;
	map = MemoryContextAlloc(ex->cxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	for (uint32 c = 0; c < d->n; c++)
		map[c] = intern_id(ex, &ex->cs[ci].in[p], d->bytes + d->off[c], d->off[c + 1] - d->off[c]);
	map[d->n] = 0;
	s->kmap[slot] = map;
	return map;
}

static uint32
map_hash(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv)
{
	uint32		h = 0x9E3779B9;

	for (int k = 0; k < m->nkey; k++)
		h = hash_combine(h, m->ktext[k] ? hash_bytes((const unsigned char *) ptr[k], len[k]) :
						 murmurhash32((uint32) (iv[k] ^ (iv[k] >> 32))));
	return h;
}

static bool
map_key_eq(const MapTab *m, uint32 row, const char **ptr, const uint32 *len, const int64 *iv)
{
	for (int k = 0; k < m->nkey; k++)
	{
		size_t		i = (size_t) row * m->nkey + k;

		if (m->ktext[k] ? (m->klen[i] != len[k] || memcmp(m->kptr[i], ptr[k], len[k]) != 0) : m->kint[i] != iv[k])
			return false;
	}
	return true;
}

/* the rows whose key equals the one given; palloc'd in the current context */
static int
map_find(const MapTab *m, const char **ptr, const uint32 *len, const int64 *iv, uint32 **rows)
{
	uint32		h = map_hash(m, ptr, len, iv);
	int			n = 0;

	*rows = palloc(Max(m->nrow, 1) * sizeof(uint32));
	for (uint32 r = m->head[h & (m->nslot - 1)]; r != 0; r = m->next[r - 1])
		if (m->hv[r - 1] == h && map_key_eq(m, r - 1, ptr, len, iv))
			(*rows)[n++] = r - 1;
	return n;
}

static void
map_add(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j)
{
	size_t		kb = (size_t) m->nrow * m->nkey,
				cb = (size_t) m->nrow * m->ncol;
	Datum		kd[SIFT_AGG_MAX_JKEYS];

	for (int k = 0; k < m->nkey; k++)
	{
		bool		isnull;

		kd[k] = slot_getattr(slot, j->keyatt[k], &isnull);
		if (isnull)
			return;				/* NULL equals nothing */
	}
	if (m->nrow == m->cap)
	{
		m->cap = Max(m->cap * 2, 256);
		m->kptr = m->kptr ? repalloc(m->kptr, (size_t) m->cap * m->nkey * sizeof(char *)) :
			MemoryContextAlloc(ex->cxt, (size_t) m->cap * m->nkey * sizeof(char *));
		m->klen = m->klen ? repalloc(m->klen, (size_t) m->cap * m->nkey * sizeof(uint32)) :
			MemoryContextAlloc(ex->cxt, (size_t) m->cap * m->nkey * sizeof(uint32));
		m->kint = m->kint ? repalloc(m->kint, (size_t) m->cap * m->nkey * sizeof(int64)) :
			MemoryContextAlloc(ex->cxt, (size_t) m->cap * m->nkey * sizeof(int64));
		m->col = m->col ? repalloc(m->col, (size_t) m->cap * Max(m->ncol, 1) * sizeof(Datum)) :
			MemoryContextAlloc(ex->cxt, (size_t) m->cap * Max(m->ncol, 1) * sizeof(Datum));
		m->cnull = m->cnull ? repalloc(m->cnull, (size_t) m->cap * Max(m->ncol, 1)) :
			MemoryContextAlloc(ex->cxt, (size_t) m->cap * Max(m->ncol, 1));
	}
	for (int k = 0; k < m->nkey; k++)
	{
		if (m->ktext[k])
		{
			struct varlena *v = PG_DETOAST_DATUM_COPY(kd[k]);

			m->kptr[kb + k] = VARDATA_ANY(v);
			m->klen[kb + k] = VARSIZE_ANY_EXHDR(v);
		}
		else
			m->kint[kb + k] = sift_aggspec_operand_int(j->keytype[k], kd[k]);
	}
	for (int c = 0; c < m->ncol; c++)
	{
		int16		len;
		bool		byval;
		Datum		d = slot_getattr(slot, j->colatt[c], &m->cnull[cb + c]);

		get_typlenbyval(j->coltype[c], &len, &byval);
		m->col[cb + c] = m->cnull[cb + c] ? (Datum) 0 : byval ? d : len == -1 ? PointerGetDatum(PG_DETOAST_DATUM_COPY(d)) :
			datumCopy(d, byval, len);
	}
	m->nrow++;
}

/* the rows of the joined table, visible to the scan's snapshot, hashed by key */
static MapTab *
map_load(AggExec *ex, const SiftAggJoin *j)
{
	MapTab	   *m;
	Relation	rel = table_open(j->relid, AccessShareLock);
	TableScanDesc scan = table_beginscan(rel, ex->css.ss.ps.state->es_snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	m = palloc0(sizeof(MapTab));
	m->nkey = j->nkey;
	m->ncol = j->ncol;
	for (int k = 0; k < j->nkey; k++)
		m->ktext[k] = j->keytype[k] == TEXTOID || j->keytype[k] == VARCHAROID;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		map_add(ex, m, slot, j);
	m->nslot = 64;
	while (m->nslot < 2 * m->nrow)
		m->nslot *= 2;
	m->head = palloc0(m->nslot * sizeof(uint32));
	m->next = palloc(Max(m->nrow, 1) * sizeof(uint32));
	m->hv = palloc(Max(m->nrow, 1) * sizeof(uint32));
	for (uint32 r = m->nrow; r-- > 0;)
	{
		uint32		h = map_hash(m, m->kptr + (size_t) r * m->nkey, m->klen + (size_t) r * m->nkey, m->kint + (size_t) r * m->nkey);

		m->hv[r] = h;
		m->next[r] = m->head[h & (m->nslot - 1)];
		m->head[h & (m->nslot - 1)] = r + 1;
	}
	MemoryContextSwitchTo(old);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(rel, NoLock);
	return m;
}

static void
bind_param(AggExec *ex, int i, Datum d, bool isnull)
{
	ex->lutparams[i].value = d;
	ex->lutparams[i].isnull = isnull;
}

/* the leaf values of an entry (v: ids) as the Params of the combo's expressions */
static void
combo_bind_leaves(AggExec *ex, int ci, const uint64 *v)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];

	for (int p = 0; p < cb->nleaves; p++)
	{
		if (v[p] == 0)
			bind_param(ex, p, (Datum) 0, true);
		else if (ex->lay->leaf[cb->leaf[p]].vtype == SCV_CODE)
			bind_param(ex, p, text_datum(cb->ptype[p], cb->ptypmod[p], cs->in[p].ptr[v[p] - 1], cs->in[p].len[v[p] - 1]), false);
		else
			bind_param(ex, p, leaf_datum(cb->ptype[p], (int32) (v[p] - 1)), false);
	}
}

/* the matching rows of the entry's key (none when a key leaf is NULL) */
static int
combo_matches(AggExec *ex, int ci, const uint64 *v, uint32 **rows)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	const char *ptr[SIFT_AGG_MAX_JKEYS] = {0};
	uint32		len[SIFT_AGG_MAX_JKEYS] = {0};
	int64		iv[SIFT_AGG_MAX_JKEYS] = {0};

	*rows = NULL;
	for (int k = 0; k < j->nkey; k++)
	{
		int			p = j->keypos[k];

		if (v[p] == 0)
			return 0;
		if (cs->map->ktext[k])
		{
			ptr[k] = cs->in[p].ptr[v[p] - 1];
			len[k] = cs->in[p].len[v[p] - 1];
		}
		else
			iv[k] = (int32) (v[p] - 1);
	}
	return map_find(cs->map, ptr, len, iv, rows);
}

static void
combo_grow_out(AggExec *ex, ComboSt *cs, int nslots, uint32 add)
{
	if (cs->nout + add > cs->outcap)
	{
		cs->outcap = Max(Max(cs->outcap * 2, cs->nout + add), 64);
		cs->val = cs->val ? repalloc(cs->val, (size_t) cs->outcap * Max(nslots, 1) * sizeof(uint32)) :
			MemoryContextAlloc(ex->cxt, (size_t) cs->outcap * Max(nslots, 1) * sizeof(uint32));
	}
}

/* the table of the distinct values a slot gives ids to: of the key, or of the aggregate it is the argument of */
static OutTable *
slot_ot(AggExec *ex, const SiftAggSlot *sl)
{
	return sl->agg >= 0 ? &ex->aot[sl->agg] : &ex->ks[sl->key].ot;
}

/* evaluates the slots of a new entry for every output row */
static void
combo_make(AggExec *ex, int ci, const uint64 *v, ComboEnt *ent)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = cb->join >= 0 ? &ex->spec->join[cb->join] : NULL;
	ComboSt    *cs = &ex->cs[ci];
	MapTab	   *m = cs->map;
	MemoryContext old = MemoryContextSwitchTo(ex->comcxt);
	uint32	   *rows = NULL;
	int			nm = 0;
	uint32		nout;

	combo_bind_leaves(ex, ci, v);
	if (j)
		nm = combo_matches(ex, ci, v, &rows);
	nout = j == NULL ? 1 : nm > 0 ? nm : j->left ? 1 : 0;
	combo_grow_out(ex, cs, cb->nslots, nout);
	ent->nout = nout;
	ent->base = cs->nout;
	for (uint32 o = 0; o < nout; o++)
	{
		for (int c = 0; j && c < j->ncol; c++)
			bind_param(ex, cb->nleaves + c, nm > 0 ? m->col[(size_t) rows[o] * m->ncol + c] : (Datum) 0, nm == 0 || m->cnull[(size_t) rows[o] * m->ncol + c]);
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			bool		isnull;
			Datum		d = ExecEvalExprSwitchContext(cs->slot[sl], ex->lutcx, &isnull);
			uint32		r;

			if (!cb->slot[sl].is_key)
				r = !isnull && DatumGetBool(d);
			else if (isnull)
				r = 0;
			else
			{
				MemoryContextSwitchTo(ex->cxt);
				r = ot_id(slot_ot(ex, &cb->slot[sl]), d);
				MemoryContextSwitchTo(ex->comcxt);
			}
			cs->val[(size_t) cs->nout * cb->nslots + sl] = r;
		}
		cs->nout++;
		ResetExprContext(ex->lutcx);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
}

static inline uint32
combo_hash(const uint64 *v)
{
	uint64		h = (v[0] * UINT64CONST(0x9E3779B97F4A7C15)) ^ (v[1] * UINT64CONST(0xC2B2AE3D27D4EB4F)) ^ (v[2] * UINT64CONST(0x165667B19E3779F9));

	return (uint32) (h ^ (h >> 29));
}

/* the entry of a tuple of leaf values, made on its first sight */
static uint32
combo_ent(AggExec *ex, int ci, const uint64 *v)
{
	ComboSt    *cs = &ex->cs[ci];
	uint32		h = combo_hash(v) & (cs->nht - 1),
				e;

	for (; (e = cs->ht[h]) != 0; h = (h + 1) & (cs->nht - 1))
		if (memcmp(&cs->vid[(size_t) (e - 1) * SIFT_AGG_MAX_CLEAVES], v, SIFT_AGG_MAX_CLEAVES * sizeof(uint64)) == 0)
			return e - 1;
	if (cs->nent == cs->entcap)
	{
		cs->entcap = Max(cs->entcap * 2, 64);
		cs->vid = cs->vid ? repalloc(cs->vid, (size_t) cs->entcap * SIFT_AGG_MAX_CLEAVES * sizeof(uint64)) :
			MemoryContextAlloc(ex->cxt, (size_t) cs->entcap * SIFT_AGG_MAX_CLEAVES * sizeof(uint64));
		cs->ent = cs->ent ? repalloc(cs->ent, (size_t) cs->entcap * sizeof(ComboEnt)) :
			MemoryContextAlloc(ex->cxt, (size_t) cs->entcap * sizeof(ComboEnt));
	}
	if (cs->nent >= (uint32) 1 << 24)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: too many distinct value combinations for the aggregate scan"),
				 errhint("Set sift.enable_agg to off for this query.")));
	memcpy(&cs->vid[(size_t) cs->nent * SIFT_AGG_MAX_CLEAVES], v, SIFT_AGG_MAX_CLEAVES * sizeof(uint64));
	combo_make(ex, ci, v, &cs->ent[cs->nent]);
	cs->ht[h] = ++cs->nent;
	if (cs->nent * 2 > cs->nht)
	{
		pfree(cs->ht);
		cs->nht *= 2;
		cs->ht = MemoryContextAllocZero(ex->cxt, cs->nht * sizeof(uint32));
		for (uint32 x = 0; x < cs->nent; x++)
		{
			h = combo_hash(&cs->vid[(size_t) x * SIFT_AGG_MAX_CLEAVES]) & (cs->nht - 1);
			while (cs->ht[h])
				h = (h + 1) & (cs->nht - 1);
			cs->ht[h] = x + 1;
		}
	}
	return cs->nent - 1;
}

/* eid[i] = the entry of the combo's leaf values at tuple i: an element's leaf is read at tidx, a row's at ridx */
static void
combo_resolve(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *eid)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];

	for (int p = 0; p < SIFT_AGG_MAX_CLEAVES; p++)
	{
		uint64	   *vid = ex->cvid[p];

		if (p >= cb->nleaves)
		{
			memset(vid, 0, n * sizeof(uint64));
			continue;
		}
		else
		{
			const SiftSrcVec *v = get_vec(ex, cb->leaf[p]);
			const uint32 *idx = leaf_lvl(ex, cb->leaf[p]) == qlevel ? tidx : ridx;
			const uint32 *map = NULL;

			if (ex->lay->leaf[cb->leaf[p]].vtype == SCV_CODE)
				map = dict_cmap(ex, dict_slot(ex, v->dict), v->dict, ci, p);
			for (int i = 0; i < n; i++)
			{
				if (v->isnull && v->isnull[idx[i]])
					vid[i] = 0;
				else if (map)
					vid[i] = map[((const uint32 *) v->data)[idx[i]]];
				else
					vid[i] = (uint64) (uint32) ((const int32 *) v->data)[idx[i]] + 1;
			}
		}
	}
	for (int i = 0; i < n; i++)
	{
		uint64		v[SIFT_AGG_MAX_CLEAVES] = {ex->cvid[0][i], ex->cvid[1][i], ex->cvid[2][i]};

		eid[i] = combo_ent(ex, ci, v);
	}
}

/* the output row of the combo for each of the current tuples */
static const uint32 *
combo_outs(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	uint32	   *o = ex->cfree[ci];

	if (ex->spec->combo[ci].join >= 0)
		return ex->cur->cout[ci];
	if (!ex->cfree_ok[ci])
	{
		combo_resolve(ex, ci, ex->qlevel, ex->cur->n, ex->tidx, ex->ridx, o);
		for (int i = 0; i < ex->cur->n; i++)
			o[i] = cs->ent[o[i]].base;
		ex->cfree_ok[ci] = true;
	}
	return o;
}

/* boolean slot p->op of a combo without a join, over [start, start + n) of the elements (qlevel 1) or rows */
static void
eval_combo(AggExec *ex, SiftAggPred *p, int qlevel, int start, int n, uint64 *out)
{
	ComboSt    *cs = &ex->cs[p->leaf];
	int			ns = ex->spec->combo[p->leaf].nslots;

	for (int i = 0; i < n; i++)
	{
		ex->pti[i] = start + i;
		ex->pri[i] = qlevel ? ex->parent[start + i] : start + i;
	}
	combo_resolve(ex, p->leaf, qlevel, n, ex->pti, ex->pri, ex->peid);
	clear_words(out, n);
	for (int i = 0; i < n; i++)
		if (cs->val[(size_t) cs->ent[ex->peid[i]].base * ns + p->op])
			out[i >> 6] |= UINT64CONST(1) << (i & 63);
}

typedef struct LutFill
{
	AggExec    *ex;
	SiftAggPred *p;
} LutFill;

static void
pred_lut_fill(void *arg, int32 v, void *entry)
{
	LutFill    *f = arg;
	PredState  *st = &f->ex->ps[f->p->id];

	*(uint8 *) entry = eval_bool(f->ex, st->expr, leaf_datum(st->param->paramtype, v), false);
}

static void
eval_lut(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];

	if (lf->vtype == SCV_CODE)
	{
		DictSlot   *s = dict_slot(ex, v->dict);
		uint8	   *lut = dict_lut(ex, s, v->dict, p);
		const uint32 *codes = (const uint32 *) v->data + start;

		if (v->isnull)
		{
			for (int i = 0; i < n; i++)
				ex->tmpu32[i] = v->isnull[start + i] ? v->dict->n : codes[i];
			codes = ex->tmpu32;
		}
		sift_aggk->lut_code(out, codes, n, lut);
	}
	else
	{
		const int32 *vals = (const int32 *) v->data + start;
		int32		mn,
					mx;
		LutFill		f = {ex, p};
		int			nw = (n + 63) / 64;

		fill_ones(ex->tmpm, n);
		if (v->isnull)
		{
			sift_aggk->bytes_mask(ex->tmpm2, v->isnull + start, n);
			sift_aggk->andnot_w(ex->tmpm, ex->tmpm2, nw);
		}
		if (sift_aggk->minmax_i32(&mn, &mx, vals, ex->tmpm, n))
		{
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

			rt_cover(&st->rt, mn, mx, pred_lut_fill, &f);
			MemoryContextSwitchTo(old);
			/* a null row reads whatever its slot holds: the result is replaced below */
			sift_aggk->lut_i32(out, vals, n, st->rt.lo, (uint32) ((int64) st->rt.hi - st->rt.lo + 1), (uint8 *) st->rt.tab);
		}
		else
			clear_words(out, n);
		if (v->isnull)
		{
			sift_aggk->bytes_mask(ex->tmpm2, v->isnull + start, n);
			sift_aggk->andnot_w(out, ex->tmpm2, nw);
			if (st->null_result)
				sift_aggk->or_w(out, ex->tmpm2, nw);
		}
	}
}

static void eval_pred(AggExec *ex, SiftAggPred *p, int qlevel, int start, int n, uint64 *out);

/* is a bit set in [lo, hi) */
static bool
any_bit(const uint64 *b, uint32 lo, uint32 hi)
{
	uint32		w0 = lo >> 6,
				w1;
	uint64		m0 = ~UINT64CONST(0) << (lo & 63),
				m1;

	if (lo >= hi)
		return false;
	w1 = (hi - 1) >> 6;
	m1 = ~UINT64CONST(0) >> (63 - ((hi - 1) & 63));
	if (w0 == w1)
		return (b[w0] & m0 & m1) != 0;
	if ((b[w0] & m0) || (b[w1] & m1))
		return true;
	for (uint32 w = w0 + 1; w < w1; w++)
		if (b[w])
			return true;
	return false;
}

/* SAP_ANY_ELEM on the unit's rows: the element predicate on every element of the unit, reduced to the rows that own a passing one */
static void
any_elem_rows(AggExec *ex, SiftAggPred *p, uint64 *out)
{
	const uint32 *offs = elem_offs(ex, p->leaf);
	int			nrow = ex->unit.nrow,
				nw = (nrow + 63) / 64;
	uint32		m = offs[nrow];
	uint64	   *eb = MemoryContextAlloc(ex->ucxt, ((m + 63) / 64 + PADW) * sizeof(uint64));
	uint64		t[VECW + PADW];

	for (uint32 s = 0; s < m; s += VEC)
	{
		int			n = Min(VEC, m - s);

		eval_pred(ex, p->child[0], 1, s, n, t);
		memcpy(eb + s / 64, t, ((n + 63) / 64) * sizeof(uint64));
	}
	clear_words(out, nrow);
	for (int r = 0; r < nrow; r++)
		if (any_bit(eb, offs[r], offs[r + 1]))
			out[r >> 6] |= UINT64CONST(1) << (r & 63);
	if (p->op)
	{
		for (int w = 0; w < nw; w++)
			out[w] = ~out[w];
		if (nrow & 63)
			out[nw - 1] &= (UINT64CONST(1) << (nrow & 63)) - 1;
	}
}

/* the bitmap of a row level predicate over the unit's rows, computed once */
static uint64 *
row_bitmap(AggExec *ex, SiftAggPred *p)
{
	PredState  *st = &ex->ps[p->id];

	if (!st->rowmask_ok)
	{
		int			nrow = ex->unit.nrow;

		st->rowmask = MemoryContextAlloc(ex->ucxt, ((nrow + 63) / 64 + PADW) * sizeof(uint64));
		if (p->kind == SAP_ANY_ELEM)
			any_elem_rows(ex, p, st->rowmask);
		else
			for (int s = 0; s < nrow; s += VEC)
				eval_pred(ex, p, 0, s, Min(VEC, nrow - s), st->rowmask + s / 64);
		st->rowmask_ok = true;
	}
	return st->rowmask;
}

static void
eval_pred(AggExec *ex, SiftAggPred *p, int qlevel, int start, int n, uint64 *out)
{
	PredState  *st = &ex->ps[p->id];
	int			nw = (n + 63) / 64;

	if (p->level < qlevel && p->kind != SAP_TRUE && p->kind != SAP_FALSE)
	{
		/* a row level predicate seen from the elements */
		sift_aggk->gather_bits(out, row_bitmap(ex, p), ex->parent + start, n);
		return;
	}
	if (!st->ready && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NCMP))
		eval_operand(ex, p, st);
	switch (p->kind)
	{
		case SAP_TRUE:
			fill_ones(out, n);
			break;
		case SAP_FALSE:
			clear_words(out, n);
			break;
		case SAP_AND:
		case SAP_OR:
			{
				uint64		tmp[VECW + PADW];

				eval_pred(ex, p->child[0], qlevel, start, n, out);
				for (int i = 1; i < p->nchild; i++)
				{
					eval_pred(ex, p->child[i], qlevel, start, n, tmp);
					if (p->kind == SAP_AND)
						sift_aggk->and_w(out, tmp, nw);
					else
						sift_aggk->or_w(out, tmp, nw);
				}
			}
			break;
		case SAP_CMP:
			eval_cmp(ex, p, st, start, n, out);
			break;
		case SAP_NCMP:
			eval_ncmp(ex, p, st, start, n, out);
			break;
		case SAP_IN:
			eval_in(ex, p, st, start, n, out);
			break;
		case SAP_NULL:
			eval_null(ex, p, start, n, out);
			break;
		case SAP_LUT:
			eval_lut(ex, p, st, start, n, out);
			break;
		case SAP_COMBO:
			eval_combo(ex, p, qlevel, start, n, out);
			break;
		case SAP_ANY_ELEM:
			memcpy(out, row_bitmap(ex, p) + start / 64, nw * sizeof(uint64));
			break;
	}
}

/* ---------- count(DISTINCT) sets ---------- */

#define DSET_PAIR(g, v)		(UINT64CONST(1) << 63 | (uint64) (g) << 32 | (uint32) (v))

static inline uint64
dset_hash(uint64 x)
{
	x *= UINT64CONST(0x9E3779B97F4A7C15);
	return x ^ (x >> 32);
}

static uint64
dset_insert_raw(DSet *s, uint64 pair)
{
	uint64		h = dset_hash(pair) & (s->cap - 1);

	while (s->slot[h])
		h = (h + 1) & (s->cap - 1);
	s->slot[h] = pair;
	s->n++;
	return h;
}

/* twice the slots, the groups renumbered through remap (NULL: as they are) */
static void
dset_grow(AggExec *ex, DSet *s, const uint32 *remap)
{
	uint64	   *old = s->slot;
	int64	   *ocnt = s->cnt;
	uint64		ocap = s->cap;

	s->cap = Max(ocap * 2, 1024);
	s->slot = MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(uint64));
	s->cnt = s->counted ? MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(int64)) : NULL;
	s->n = 0;
	for (uint64 i = 0; i < ocap; i++)
		if (old[i])
		{
			uint64		h = dset_insert_raw(s, remap ? DSET_PAIR(remap[(old[i] >> 32) & 0x7fffffff], old[i]) : old[i]);

			if (s->counted)
				s->cnt[h] = ocnt[i];
		}
	if (old)
		pfree(old);
	if (ocnt)
		pfree(ocnt);
}

/* true when the pair is new */
static bool
dset_add(AggExec *ex, DSet *s, uint32 g, uint32 v)
{
	uint64		pair = DSET_PAIR(g, v),
				h;

	if (s->n * 2 >= s->cap)
		dset_grow(ex, s, NULL);
	h = dset_hash(pair) & (s->cap - 1);
	while (s->slot[h] && s->slot[h] != pair)
		h = (h + 1) & (s->cap - 1);
	if (s->slot[h])
	{
		if (s->counted)
			s->cnt[h]++;
		return false;
	}
	s->slot[h] = pair;
	s->n++;
	if (s->counted)
		s->cnt[h] = 1;
	return true;
}

/* ---------- groups ---------- */

static AggAcc *
acc_alloc(AggExec *ex, uint32 cap)
{
	AggAcc	   *a = MemoryContextAllocZero(ex->tcxt, Max(ex->spec->naggs, 1) * sizeof(AggAcc));

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		a[i].n = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));
		switch (ex->spec->agg[i].kind)
		{
			case SAA_SUM_INT:
			case SAA_AVG_INT:
			case SAA_PCT_CONT:
			case SAA_PCT_DISC:
				a[i].i = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));
				break;
			case SAA_SUM_NUM:
			case SAA_AVG_NUM:
				a[i].w = MemoryContextAllocZero(ex->tcxt, cap * sizeof(sift_i128));
				break;
			case SAA_MIN:
			case SAA_MAX:
				a[i].i = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));
				a[i].d = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
				if (ex->spec->agg[i].num.n)
					a[i].w = MemoryContextAllocZero(ex->tcxt, cap * sizeof(sift_i128));
				break;
			case SAA_SUM_F64:
			case SAA_AVG_F64:
			case SAA_O_SUM_F64:
			case SAA_O_AVG_F64:
				a[i].d = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
				a[i].nf = MemoryContextAllocZero(ex->tcxt, cap);
				break;
			case SAA_O_SUM_I64:
				a[i].i = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));
				break;
			case SAA_M_SUM:
			case SAA_M_AVG:
				a[i].d = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
				a[i].nf = MemoryContextAllocZero(ex->tcxt, cap);
				/* fall through */
			case SAA_M_MIN:
			case SAA_M_MAX:
				a[i].v = MemoryContextAllocZero(ex->tcxt, cap * sizeof(Datum));
				break;
			case SAA_O_COUNT_ROWS:
			case SAA_COUNT_ROWS:
				a[i].last = MemoryContextAlloc(ex->tcxt, cap * sizeof(uint64));
				memset(a[i].last, 0xff, cap * sizeof(uint64));
				break;
			default:
				break;
		}
	}
	return a;
}

static void
acc_free(AggAcc *a, int naggs)
{
	for (int i = 0; i < naggs; i++)
	{
		void	   *col[] = {a[i].n, a[i].i, a[i].w, a[i].d, a[i].v, a[i].last, a[i].nf};

		for (int c = 0; c < lengthof(col); c++)
			if (col[c])
				pfree(col[c]);
	}
	pfree(a);
}

static void arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap);

/* every per-group array to newcap groups; remap[old] = new index (UINT32_MAX: drop), NULL: identity */
static void
groups_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	AggAcc	   *na = acc_alloc(ex, newcap);
	int64	   *nseen = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64));

	for (uint32 g = 0; g < oldcap; g++)
	{
		uint32		t = remap ? remap[g] : g;

		if (t == UINT32_MAX || ex->seen[g] == 0)
			continue;
		nseen[t] = ex->seen[g];
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			AggAcc	   *o = &ex->acc[i],
					   *n = &na[i];

			n->n[t] = o->n[g];
			if (o->i)
				n->i[t] = o->i[g];
			if (o->w)
				n->w[t] = o->w[g];
			if (o->d)
				n->d[t] = o->d[g];
			if (o->last)
				n->last[t] = o->last[g];
			if (o->nf)
				n->nf[t] = o->nf[g];
		}
	}
	if (remap)
		for (int i = 0; i < ex->spec->naggs; i++)
			if (ex->dset[i].slot)
				dset_grow(ex, &ex->dset[i], remap);
	arms_resize(ex, newcap, remap, oldcap);
	acc_free(ex->acc, ex->spec->naggs);
	pfree(ex->seen);
	ex->gen++;
	ex->seen = nseen;
	ex->acc = na;
	ex->gcap = newcap;
}

static void
direct_layout(AggExec *ex)
{
	uint64		m = 1;

	for (int j = 0; j < ex->spec->nkeys; j++)
	{
		ex->ks[j].mult = (uint32) m;
		m *= ex->ks[j].cap;
	}
}

static inline uint32
ids_hash(const uint32 *ids, uint32 nslot)
{
	uint64		h = 0;

	for (int j = 0; j < SIFT_AGG_MAX_KEYS; j++)
	{
		h = (h ^ ids[j]) * UINT64CONST(0x9E3779B97F4A7C15);
		h ^= h >> 32;
	}
	return (uint32) h & (nslot - 1);
}

/* the group of an id tuple in hash mode */
static uint32
hash_group(AggExec *ex, const uint32 *ids)
{
	uint32		pos = ids_hash(ids, ex->nhslot),
				g;

	while (ex->hslot[pos])
	{
		const uint32 *k = &ex->hkey[(ex->hslot[pos] - 1) * SIFT_AGG_MAX_KEYS];

		if (memcmp(k, ids, SIFT_AGG_MAX_KEYS * sizeof(uint32)) == 0)
			return ex->hslot[pos] - 1;
		pos = (pos + 1) & (ex->nhslot - 1);
	}
	g = ex->ngroups++;
	if (g >= ex->hkcap)
	{
		ex->hkcap = Max(ex->hkcap * 2, 1024);
		ex->hkey = ex->hkey ? repalloc(ex->hkey, (Size) ex->hkcap * SIFT_AGG_MAX_KEYS * sizeof(uint32)) :
			MemoryContextAlloc(ex->tcxt, (Size) ex->hkcap * SIFT_AGG_MAX_KEYS * sizeof(uint32));
	}
	memcpy(&ex->hkey[(Size) g * SIFT_AGG_MAX_KEYS], ids, SIFT_AGG_MAX_KEYS * sizeof(uint32));
	ex->hslot[pos] = g + 1;
	if ((uint64) ex->ngroups * 2 > ex->nhslot)
	{
		uint32		nn = ex->nhslot * 2;
		uint32	   *ns = MemoryContextAllocZero(ex->tcxt, nn * sizeof(uint32));

		for (uint32 x = 0; x < ex->ngroups; x++)
		{
			uint32		p = ids_hash(&ex->hkey[(Size) x * SIFT_AGG_MAX_KEYS], nn);

			while (ns[p])
				p = (p + 1) & (nn - 1);
			ns[p] = x + 1;
		}
		pfree(ex->hslot);
		ex->hslot = ns;
		ex->nhslot = nn;
	}
	return g;
}

/* the id of key j in group g (0: NULL) */
static uint32
key_id(AggExec *ex, int j, uint32 g)
{
	return ex->hash ? ex->hkey[(Size) g * SIFT_AGG_MAX_KEYS + j] : (g / ex->ks[j].mult) % ex->ks[j].cap;
}

static uint32
key_used(KeyState *k)
{
	return (k->numk ? k->im.n : k->ident ? k->nident : k->ot.n) + 1;
}

/* a key outgrew its capacity: more room per key, or the hash table when the arrays would be too big */
static void
direct_grow(AggExec *ex)
{
	int			nk = ex->spec->nkeys;
	uint32		oldcap = ex->gcap,
				oldcaps[SIFT_AGG_MAX_KEYS],
				oldmult[SIFT_AGG_MAX_KEYS];
	uint64		total = 1;
	uint32	   *remap;

	for (int j = 0; j < nk; j++)
	{
		KeyState   *k = &ex->ks[j];

		oldcaps[j] = k->cap;
		oldmult[j] = k->mult;
		while (k->cap < key_used(k))
			k->cap *= 2;
		total *= k->cap;
	}
	remap = palloc(oldcap * sizeof(uint32));
	if (total > ex->direct_max)
	{
		/* the existing groups keep their ids in the hash table */
		ex->hash = true;
		ex->nhslot = 1024;
		ex->hslot = MemoryContextAllocZero(ex->tcxt, ex->nhslot * sizeof(uint32));
		ex->ngroups = 0;
		for (uint32 g = 0; g < oldcap; g++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			if (ex->seen[g] == 0)
			{
				remap[g] = UINT32_MAX;
				continue;
			}
			for (int j = 0; j < nk; j++)
				ids[j] = (g / oldmult[j]) % oldcaps[j];
			remap[g] = hash_group(ex, ids);
		}
		groups_resize(ex, Max(ex->ngroups * 2, 1024), remap, oldcap);
	}
	else
	{
		direct_layout(ex);
		for (uint32 g = 0; g < oldcap; g++)
		{
			uint64		t = 0;

			for (int j = 0; j < nk; j++)
				t += (uint64) ((g / oldmult[j]) % oldcaps[j]) * ex->ks[j].mult;
			remap[g] = (uint32) t;
		}
		groups_resize(ex, (uint32) total, remap, oldcap);
	}
	pfree(remap);
}

/* ---------- group keys ---------- */

typedef struct KeyFill
{
	AggExec    *ex;
	int			ki;
} KeyFill;

/* the next id of an ident key, for the value v */
static uint32
ident_add(AggExec *ex, KeyState *k, int64 v)
{
	if (k->nident == k->capident)
	{
		k->capident = Max(k->capident * 2, 64);
		k->idval = k->idval ? repalloc(k->idval, k->capident * sizeof(int64)) :
			MemoryContextAlloc(ex->tcxt, k->capident * sizeof(int64));
	}
	k->idval[k->nident++] = v;
	return k->nident;
}

static inline uint32
wide_hash(int64 v, uint32 cap)
{
	return (uint32) (((uint64) v * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & (cap - 1);
}

/* the id of an ident key's value when the range table is not used: ids are given in order of appearance, as the table does */
static uint32
ident_wide_id(AggExec *ex, KeyState *k, int64 v)
{
	uint32		h = wide_hash(v, k->wcap);

	while (k->wid[h])
	{
		if (k->wval[h] == v)
			return k->wid[h];
		h = (h + 1) & (k->wcap - 1);
	}
	k->wval[h] = v;
	k->wid[h] = ident_add(ex, k, v);
	if ((uint64) k->nident * 2 > k->wcap)
	{
		uint32		ocap = k->wcap;
		int64	   *ov = k->wval;
		uint32	   *oi = k->wid;

		k->wcap *= 2;
		k->wval = MemoryContextAlloc(ex->tcxt, k->wcap * sizeof(int64));
		k->wid = MemoryContextAllocZero(ex->tcxt, k->wcap * sizeof(uint32));
		for (uint32 s = 0; s < ocap; s++)
			if (oi[s])
			{
				uint32		p = wide_hash(ov[s], k->wcap);

				while (k->wid[p])
					p = (p + 1) & (k->wcap - 1);
				k->wval[p] = ov[s];
				k->wid[p] = oi[s];
			}
		pfree(ov);
		pfree(oi);
	}
	return k->nident;
}

/* from now on the ident key's ids come from the hash table; the values the range table gave ids to are in it */
static void
ident_go_wide(AggExec *ex, KeyState *k)
{
	uint32		n = k->nident;

	k->wcap = 1024;
	while (k->wcap < 4 * n)
		k->wcap *= 2;
	k->wval = MemoryContextAlloc(ex->tcxt, k->wcap * sizeof(int64));
	k->wid = MemoryContextAllocZero(ex->tcxt, k->wcap * sizeof(uint32));
	k->nident = 0;				/* ident_wide_id gives the same ids back, in the same order */
	for (uint32 i = 0; i < n; i++)
		(void) ident_wide_id(ex, k, k->idval[i]);
}

static void
key_range_fill(void *arg, int32 v, void *entry)
{
	KeyFill    *f = arg;
	KeyState   *k = &f->ex->ks[f->ki];

	if (k->ident)
		*(uint32 *) entry = ident_add(f->ex, k, v);
	else
		*(uint32 *) entry = key_value_id(f->ex, f->ki, leaf_datum(k->param->paramtype, v), false);
}

/*
 * Adds the ids of the selected tuples' key j to gid[i] (times mult); the data of the key's leaf is gathered through idx.
 * Ids that do not exist yet are made first, so key_used() is final when this returns.
 */
static void
key_prepare(AggExec *ex, int ki, int nsel, const uint32 *idx, uint32 **map_out, int32 *base_out, uint32 *vals_out)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcVec *v;
	const SiftSrcLeaf *lf;

	*map_out = NULL;			/* the values are the ids */
	*base_out = 0;
	if (!(ex->arm_mask >> ki & 1))
	{
		memset(vals_out, 0, nsel * sizeof(uint32));	/* NULL in this arm, or in this grouping set */
		return;
	}
	if (sk->kind == SAK_SET)
	{
		for (int i = 0; i < nsel; i++)
			vals_out[i] = ex->curset + 1;
		return;
	}
	if (sk->kind == SAK_COMBO)
	{
		const uint32 *out = combo_outs(ex, sk->leaf);
		const uint32 *val = ex->cs[sk->leaf].val;
		int			ns = ex->spec->combo[sk->leaf].nslots;

		for (int i = 0; i < nsel; i++)
			vals_out[i] = val[(size_t) out[i] * ns + sk->slot];
		return;
	}
	if (sk->kind == SAK_NUM)
	{
		const sift_i128 *x = eval_nx(ex, &sk->num, nsel, ex->tidx, ex->ridx, ex->nb);

		for (int i = 0; i < nsel; i++)
			vals_out[i] = ex->nb[i] ? 0 : idmap_id(&k->im, x[i]);
		return;
	}
	v = get_vec(ex, sk->leaf);
	lf = &ex->lay->leaf[sk->leaf];
	if (lf->vtype == SCV_F64)
	{
		for (int i = 0; i < nsel; i++)
			vals_out[i] = (v->isnull && v->isnull[idx[i]]) ? 0 : ident_wide_id(ex, k, float_key(((const double *) v->data)[idx[i]]));
		return;
	}
	sift_aggk->gather_u32(vals_out, (const uint32 *) v->data, idx, nsel);
	if (lf->vtype == SCV_CODE)
	{
		DictSlot   *s = dict_slot(ex, v->dict);
		uint32	   *map = dict_kmap(ex, s, v->dict, ki);

		if (v->isnull)
			for (int i = 0; i < nsel; i++)
				if (v->isnull[idx[i]])
					vals_out[i] = v->dict->n;
		*map_out = map;
		*base_out = 0;
	}
	else
	{
		int32		mn,
					mx;
		int			nw = (nsel + 63) / 64;
		KeyFill		f = {ex, ki};

		/* the range of the non-null values */
		fill_ones(ex->tmpm, nsel);
		if (v->isnull)
		{
			for (int i = 0; i < nsel; i++)
				ex->nb[i] = v->isnull[idx[i]];
			sift_aggk->bytes_mask(ex->tmpm2, ex->nb, nsel);
			sift_aggk->andnot_w(ex->tmpm, ex->tmpm2, nw);
		}
		bool		any = sift_aggk->minmax_i32(&mn, &mx, (const int32 *) vals_out, ex->tmpm, nsel);

		if (k->ident && (k->wcap || (any && rt_span(&k->rt, mn, mx) > MAX_RANGE)))
		{
			if (!k->wcap)
				ident_go_wide(ex, k);
			for (int i = 0; i < nsel; i++)
				vals_out[i] = (v->isnull && v->isnull[idx[i]]) ? 0 : ident_wide_id(ex, k, (int32) vals_out[i]);
			*map_out = NULL;		/* the values are the ids */
			*base_out = 0;
			return;
		}
		if (any)
		{
			MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

			rt_cover(&k->rt, mn, mx, key_range_fill, &f);
			MemoryContextSwitchTo(old);
		}
		else
		{
			/* only NULLs: any covered value will do for the kernel, the rows are corrected below */
			MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

			rt_cover(&k->rt, 0, 0, key_range_fill, &f);
			MemoryContextSwitchTo(old);
		}
		if (v->isnull)
			for (int i = 0; i < nsel; i++)
				if (v->isnull[idx[i]])
					vals_out[i] = (uint32) k->rt.lo;
		*map_out = (uint32 *) k->rt.tab;
		*base_out = k->rt.lo;
	}
}

/* ids of the key for the tuples whose value was NULL are 0 (or the id of expr(NULL)): fixes gid after the kernels */
static void
key_null_fix(AggExec *ex, int ki, int nsel, const uint32 *idx, const uint32 *vals, uint32 *gid, uint32 mult, uint32 *ids)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcVec *v = get_vec(ex, sk->leaf);
	uint32		nullid = 0;

	if (v->isnull == NULL || ex->lay->leaf[sk->leaf].vtype == SCV_CODE)
		return;
	if (sk->kind == SAK_LUT)
	{
		if (k->nullid == UINT32_MAX)
		{
			MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

			k->nullid = key_value_id(ex, ki, (Datum) 0, true);
			MemoryContextSwitchTo(old);
		}
		nullid = k->nullid;
	}
	for (int i = 0; i < nsel; i++)
		if (v->isnull[idx[i]])
		{
			uint32		cur = ((uint32 *) k->rt.tab)[(int32) vals[i] - k->rt.lo];

			if (gid)
				gid[i] += (nullid - cur) * mult;
			else
				ids[i] = nullid;
		}
}

/* ---------- aggregation of a chunk ---------- */

static void
gather_idx(AggExec *ex, int leaf, int nsel, const uint32 **idx_out)
{
	*idx_out = (leaf_lvl(ex, leaf) == ex->qlevel) ? ex->tidx : ex->ridx;
}

/* the leaf a key reads; NULL for the keys of a combo, a numeric expression and the grouping set number */
static const SiftSrcLeaf *
key_lf(AggExec *ex, int j)
{
	const SiftAggKey *k = &ex->spec->key[j];

	return k->kind == SAK_COMBO || k->kind == SAK_SET || k->kind == SAK_NUM ? NULL : &ex->lay->leaf[k->leaf];
}

static void
compute_gids(AggExec *ex, int nsel)
{
	int			nk = ex->spec->nkeys;

	if (nk == 0)
	{
		memset(ex->gid, 0, nsel * sizeof(uint32));
		return;
	}
	if (!ex->hash)
	{
		uint32	   *maps[SIFT_AGG_MAX_KEYS];
		int32		bases[SIFT_AGG_MAX_KEYS];
		uint32	   *vals[SIFT_AGG_MAX_KEYS];
		bool		grow = false;

		/* make every id first: capacity checks need the final counts */
		for (int j = 0; j < nk; j++)
		{
			const uint32 *idx = NULL;

			if (key_lf(ex, j))
				gather_idx(ex, ex->spec->key[j].leaf, nsel, &idx);
			vals[j] = ex->kids + (Size) j * (VEC + PADW);
			key_prepare(ex, j, nsel, idx, &maps[j], &bases[j], vals[j]);
			if (key_used(&ex->ks[j]) > ex->ks[j].cap)
				grow = true;
		}
		if (grow)
			direct_grow(ex);
		if (!ex->hash)
		{
			memset(ex->gid, 0, nsel * sizeof(uint32));
			for (int j = 0; j < nk; j++)
			{
				const uint32 *idx = NULL;
				const SiftSrcLeaf *lf = key_lf(ex, j);

				if (lf)
					gather_idx(ex, ex->spec->key[j].leaf, nsel, &idx);
				if (maps[j] == NULL)
					for (int i = 0; i < nsel; i++)
						ex->gid[i] += vals[j][i] * ex->ks[j].mult;
				else if (lf->vtype == SCV_CODE)
					sift_aggk->gid_code(ex->gid, vals[j], maps[j], nsel, ex->ks[j].mult);
				else
				{
					sift_aggk->gid_i32(ex->gid, (const int32 *) vals[j], bases[j], maps[j], nsel, ex->ks[j].mult);
					key_null_fix(ex, j, nsel, idx, vals[j], ex->gid, ex->ks[j].mult, NULL);
				}
			}
			return;
		}
		/* the table just replaced the arrays: recompute below, in hash mode */
	}
	{
		uint32	   *maps[SIFT_AGG_MAX_KEYS];
		int32		bases[SIFT_AGG_MAX_KEYS];

		for (int j = 0; j < nk; j++)
		{
			const uint32 *idx = NULL;
			const SiftSrcLeaf *lf = key_lf(ex, j);
			uint32	   *vals = ex->kids + (Size) j * (VEC + PADW);

			if (lf)
				gather_idx(ex, ex->spec->key[j].leaf, nsel, &idx);
			key_prepare(ex, j, nsel, idx, &maps[j], &bases[j], vals);
			/* the ids themselves: gid = 0 + map[..] * 1 into a per key array */
			{
				uint32	   *kid = ex->tmpu32 + (Size) j * (VEC + PADW);

				memset(kid, 0, nsel * sizeof(uint32));
				if (maps[j] == NULL)
					memcpy(kid, vals, nsel * sizeof(uint32));
				else if (lf->vtype == SCV_CODE)
					sift_aggk->gid_code(kid, vals, maps[j], nsel, 1);
				else
				{
					sift_aggk->gid_i32(kid, (const int32 *) vals, bases[j], maps[j], nsel, 1);
					key_null_fix(ex, j, nsel, idx, vals, NULL, 1, kid);
				}
			}
		}
		for (int i = 0; i < nsel; i++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			for (int j = 0; j < nk; j++)
				ids[j] = ex->tmpu32[(Size) j * (VEC + PADW) + i];
			ex->gid[i] = hash_group(ex, ids);
		}
		if (ex->ngroups > ex->gcap)
			groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
	}
}

/* okb[j] = 0 for the tuples whose value of the leaf is NULL; true when the leaf has NULLs */
static bool
clear_null_rows(AggExec *ex, int leaf, int nsel)
{
	const SiftSrcVec *v = get_vec(ex, leaf);
	const uint32 *idx;

	if (v->isnull == NULL)
		return false;
	gather_idx(ex, leaf, nsel, &idx);
	for (int j = 0; j < nsel; j++)
		ex->okb[j] &= !v->isnull[idx[j]];
	return true;
}

/* the aggregate's selection among the nsel tuples: its FILTER, a non-null argument; the bitmap goes to ex->okm */
static void
agg_selection(AggExec *ex, SiftAggAgg *a, int start, int n, int nsel, bool *all)
{
	bool		any = false;

	memset(ex->okb, 1, nsel);
	if (a->filter)
	{
		uint64		fm[VECW + PADW];

		eval_pred(ex, a->filter, ex->qlevel, start, n, fm);
		for (int j = 0; j < nsel; j++)
			ex->okb[j] = sift_colsrc_bit(fm, ex->sel[j]);
		any = true;
	}
	if (a->leaf >= 0)
		any |= clear_null_rows(ex, a->leaf, nsel);
	for (int k = 0; k < a->nvx; k++)
		if (a->vx[k].op == SVX_LEAF)
			any |= clear_null_rows(ex, a->vx[k].leaf, nsel);
	for (int k = 0; k < a->num.n; k++)
		if (a->num.step[k].op == SNX_LEAF)
			any |= clear_null_rows(ex, a->num.step[k].leaf, nsel);
	*all = !any;
	if (any)
		sift_aggk->bytes_mask(ex->okm, ex->okb, nsel);
}

/* The value of the float8 argument program of aggregate a for the nsel tuples (in double, NULL operands are not looked at) */
static const double *
eval_vx(AggExec *ex, SiftAggAgg *a, int nsel)
{
	int			sp = 0;
	const int	stride = SIFT_AGG_VEC + PADW;

#define VS(i)	(ex->vstk + (Size) (i) * stride)
	for (int k = 0; k < a->nvx; k++)
	{
		const SiftAggVx *x = &a->vx[k];
		double	   *top = VS(sp);

		switch (x->op)
		{
			case SVX_LEAF:
				{
					const SiftSrcVec *v = get_vec(ex, x->leaf);
					const uint32 *idx;

					gather_idx(ex, x->leaf, nsel, &idx);
					if (ex->lay->leaf[x->leaf].vtype == SCV_I32)
						for (int j = 0; j < nsel; j++)
							top[j] = ((const int32 *) v->data)[idx[j]];
					else if (ex->lay->leaf[x->leaf].vtype == SCV_I64)	/* numeric: the integer over 10^scale, in c */
						for (int j = 0; j < nsel; j++)
							top[j] = (double) ((const int64 *) v->data)[idx[j]] / x->c;
					else
						for (int j = 0; j < nsel; j++)
							top[j] = ((const double *) v->data)[idx[j]];
					sp++;
				}
				break;
			case SVX_CONST:
				for (int j = 0; j < nsel; j++)
					top[j] = x->c;
				sp++;
				break;
			case SVX_NEG:
				for (int j = 0; j < nsel; j++)
					VS(sp - 1)[j] = -VS(sp - 1)[j];
				break;
			case SVX_ADD:
				for (int j = 0; j < nsel; j++)
					VS(sp - 2)[j] += VS(sp - 1)[j];
				sp--;
				break;
			case SVX_SUB:
				for (int j = 0; j < nsel; j++)
					VS(sp - 2)[j] -= VS(sp - 1)[j];
				sp--;
				break;
			case SVX_MUL:
				for (int j = 0; j < nsel; j++)
					VS(sp - 2)[j] *= VS(sp - 1)[j];
				sp--;
				break;
		}
	}
#undef VS
	return ex->vstk;
}

/*
 * PostgreSQL's float8 aggregates raise errors on overflow that a plain double sum would hide as Infinity. sum(): the error is
 * raised at the end, when a group's sum is infinite and none of its inputs was (nf marks those). avg() keeps a sum of squares
 * (float8_accum) that overflows first: it is not tracked, the error is raised when the inputs are so large that it could have.
 */
static void
float_guard(AggExec *ex, int ai, const double *x, int nsel)
{
	AggAcc	   *acc = &ex->acc[ai];
	double		m = 0;
	bool		bad = false;

	for (int j = 0; j < nsel; j++)
	{
		bad |= ex->okb[j] && !isfinite(x[j]);
		m = Max(m, ex->okb[j] ? fabs(x[j]) : 0);
	}
	if (bad)
	{
		for (int j = 0; j < nsel; j++)
			if (ex->okb[j] && !isfinite(x[j]))
				acc->nf[ex->gid[j]] = 1;
		return;
	}
	ex->amax[ai] = Max(ex->amax[ai], m);
	if (ex->spec->agg[ai].kind == SAA_AVG_F64 && 8.0 * ex->n_tuples * ex->amax[ai] * ex->amax[ai] >= DBL_MAX)
		float_overflow_error();
}

/* ---------- ordered-set aggregates ---------- */

#define OSA_SIGN	(UINT64CONST(1) << 63)

/* the key of a float8 that sorts as float8_cmp does: unsigned order, -0 and 0 one key, every NaN the last */
static inline uint64
f8_key(double x)
{
	uint64		b;

	if (isnan(x))
		return UINT64_MAX;
	x += 0.0;					/* -0 -> 0 */
	memcpy(&b, &x, sizeof(b));
	return (b & OSA_SIGN) ? ~b : (b | OSA_SIGN);
}

static inline double
key_f8(uint64 k)
{
	uint64		b = (k & OSA_SIGN) ? (k & ~OSA_SIGN) : ~k;
	double		x;

	if (k == UINT64_MAX)
		return get_float8_nan();
	memcpy(&x, &b, sizeof(x));
	return x;
}

static inline uint64
osa_hash(uint32 sid, uint64 key)
{
	return dset_hash(key ^ ((uint64) sid * UINT64CONST(0xC2B2AE3D27D4EB4F)));
}

static Osa *
osa_new(AggExec *ex)
{
	Osa		   *o = MemoryContextAllocZero(ex->tcxt, sizeof(Osa));

	o->cap = 1024;
	o->slot = MemoryContextAllocZero(ex->tcxt, o->cap * sizeof(OsaSlot));
	o->maxn = Max((uint64) work_mem * 1024 / (4 * sizeof(OsaSlot)), 256);	/* the table has at most 4 slots per pair */
	return o;
}

static void
osa_put(Osa *o, uint32 sid, uint64 key, uint64 cnt)
{
	TupleTableSlot *t = o->put;

	for (; cnt > 0; cnt--)
	{
		ExecClearTuple(t);
		t->tts_values[0] = Int32GetDatum((int32) sid);
		t->tts_values[1] = Int64GetDatum((int64) (key ^ OSA_SIGN));
		t->tts_isnull[0] = t->tts_isnull[1] = false;
		ExecStoreVirtualTuple(t);
		tuplesort_puttupleslot(o->sort, t);
	}
}

/* the histogram does not fit work_mem: its pairs, and from now on every value, go through a tuplesort */
static void
osa_spill(AggExec *ex, Osa *o)
{
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	TupleDesc	desc = CreateTemplateTupleDesc(2);
	AttrNumber	att[2] = {1, 2};
	Oid			ops[2] = {Int4LessOperator, Int8LessOperator},
				coll[2] = {InvalidOid, InvalidOid};
	bool		nulls[2] = {false, false};

	TupleDescInitEntry(desc, 1, "sid", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "key", INT8OID, -1, 0);
	o->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	o->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	o->sort = tuplesort_begin_heap(desc, 2, att, ops, coll, nulls, work_mem, NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(old);
	for (uint64 i = 0; i < o->cap; i++)
		if (o->slot[i].sid1)
			osa_put(o, o->slot[i].sid1 - 1, o->slot[i].key, o->slot[i].cnt);
	pfree(o->slot);
	o->slot = NULL;
	ex->n_osa_spilled++;
}

static void
osa_grow(AggExec *ex, Osa *o)
{
	OsaSlot    *old = o->slot;
	uint64		ocap = o->cap;

	o->cap *= 2;
	o->slot = MemoryContextAllocZero(ex->tcxt, o->cap * sizeof(OsaSlot));
	for (uint64 i = 0; i < ocap; i++)
		if (old[i].sid1)
		{
			uint64		h = osa_hash(old[i].sid1 - 1, old[i].key) & (o->cap - 1);

			while (o->slot[h].sid1)
				h = (h + 1) & (o->cap - 1);
			o->slot[h] = old[i];
		}
	pfree(old);
}

static void
osa_add(AggExec *ex, Osa *o, uint32 sid, uint64 key)
{
	uint64		h;

	if (o->sort)
	{
		osa_put(o, sid, key, 1);
		return;
	}
	h = osa_hash(sid, key) & (o->cap - 1);
	for (; o->slot[h].sid1; h = (h + 1) & (o->cap - 1))
		if (o->slot[h].sid1 == sid + 1 && o->slot[h].key == key && o->slot[h].cnt != UINT32_MAX)
		{
			o->slot[h].cnt++;
			return;
		}
	if (o->n >= o->maxn)
	{
		osa_spill(ex, o);
		osa_put(o, sid, key, 1);
		return;
	}
	o->slot[h] = (OsaSlot) {key, sid + 1, 1};
	if (++o->n * 2 > o->cap)
		osa_grow(ex, o);
}

/* the argument of ordered-set aggregate ai for the nsel tuples, in double */
static const double *
osa_values(AggExec *ex, SiftAggAgg *a, const uint32 *idx, int nsel)
{
	const SiftSrcVec *v;

	if (a->nvx)
		return eval_vx(ex, a, nsel);
	v = get_vec(ex, a->leaf);
	if (ex->lay->leaf[a->leaf].vtype == SCV_F64)
	{
		sift_aggk->gather_u64(ex->c64, (const uint64 *) v->data, idx, nsel);
		return (const double *) ex->c64;
	}
	sift_aggk->gather_u32(ex->cu32, (const uint32 *) v->data, idx, nsel);
	for (int j = 0; j < nsel; j++)
		ex->vstk[j] = ((const int32 *) ex->cu32)[j];
	return ex->vstk;
}

/* adds the selected values to the values of the aggregate, which owns them (the other aggregates on the argument share them) */
static void
osa_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *own = &ex->acc[ai];
	Osa		   *o = ex->osa[ai];
	const uint32 *idx = NULL;
	const double *x;

	if (a->leaf >= 0)
		gather_idx(ex, a->leaf, nsel, &idx);
	x = osa_values(ex, a, idx, nsel);
	for (int j = 0; j < nsel; j++)
	{
		uint32		g = ex->gid[j];

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		if (own->i[g] == 0)
			own->i[g] = ++o->nsid;
		osa_add(ex, o, own->i[g] - 1, f8_key(x[j]));
	}
}

typedef struct OsaRun
{
	uint32		sid;
	uint64		key;
	uint64		cnt;
} OsaRun;

static int
osa_cmp(const void *pa, const void *pb)
{
	const OsaSlot *a = pa,
			   *b = pb;

	if (a->sid1 != b->sid1)
		return a->sid1 < b->sid1 ? -1 : 1;
	return a->key < b->key ? -1 : a->key > b->key;
}

/* the runs of equal values, by group id then value */
static void
osa_sort(Osa *o)
{
	uint64		w = 0;

	if (o->sort)
	{
		tuplesort_performsort(o->sort);
		return;
	}
	for (uint64 i = 0; i < o->cap; i++)
		if (o->slot[i].sid1)
			o->slot[w++] = o->slot[i];
	qsort(o->slot, w, sizeof(OsaSlot), osa_cmp);
	o->n = w;
	o->next = 0;
}

static bool
osa_next(Osa *o, OsaRun *r)
{
	bool		isnull;

	if (o->sort)
	{
		if (!tuplesort_gettupleslot(o->sort, true, false, o->get, NULL))
			return false;
		r->sid = DatumGetInt32(slot_getattr(o->get, 1, &isnull));
		r->key = (uint64) DatumGetInt64(slot_getattr(o->get, 2, &isnull)) ^ OSA_SIGN;
		r->cnt = 1;
		return true;
	}
	if (o->next >= o->n)
		return false;
	r->sid = o->slot[o->next].sid1 - 1;
	r->key = o->slot[o->next].key;
	r->cnt = o->slot[o->next].cnt;
	o->next++;
	return true;
}

/* where an aggregate's one or two values sit in the ascending values of a group of n, as the final functions of PostgreSQL pick them */
typedef struct OsaPick
{
	int64		pos[2];
	uint64		key[2];
	double		prop;
} OsaPick;

static void
osa_pick(const SiftAggAgg *a, int64 n, OsaPick *p)
{
	int64		lo,
				hi;

	if (a->kind == SAA_PCT_DISC)
	{
		lo = hi = Max((int64) ceil(a->frac * n), 1) - 1;
		p->prop = 0;
		if (a->desc_order)
			lo = hi = n - 1 - lo;
	}
	else
	{
		double		at = a->frac * (n - 1);

		lo = (int64) floor(at);
		hi = (int64) ceil(at);
		p->prop = at - lo;
		if (a->desc_order)
		{
			lo = n - 1 - lo;
			hi = n - 1 - hi;
		}
	}
	p->pos[0] = lo;
	p->pos[1] = hi;
}

static double
osa_result(const SiftAggAgg *a, const OsaPick *p)
{
	double		first = key_f8(p->key[0]),
				second = key_f8(p->key[1]);

	if (a->kind == SAA_PCT_DISC || p->pos[0] == p->pos[1])
		return first;
	return first + (p->prop * (second - first));
}

/* walks the sorted values of the aggregate that owns them once, taking the values of every aggregate on them */
static void
osa_finish(AggExec *ex, int owner)
{
	SiftAggSpec *s = ex->spec;
	Osa		   *o = ex->osa[owner];
	AggAcc	   *own = &ex->acc[owner];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap,
				cur = UINT32_MAX;
	int64	   *nrows = palloc0(Max(o->nsid, 1) * sizeof(int64));
	OsaPick    *pick = palloc0(s->naggs * sizeof(OsaPick));
	OsaRun		r;
	int64		pos = 0;

	for (uint32 g = 0; g < ngr; g++)
		if (own->i[g])
			nrows[own->i[g] - 1] = own->n[g];
	for (int ai = 0; ai < s->naggs; ai++)
		if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
			ex->oval[ai] = MemoryContextAlloc(ex->tcxt, Max(o->nsid, 1) * sizeof(double));
	osa_sort(o);
	for (bool more = osa_next(o, &r);; more = osa_next(o, &r))
	{
		if (!more || r.sid != cur)
		{
			for (int ai = 0; cur != UINT32_MAX && ai < s->naggs; ai++)
				if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
					ex->oval[ai][cur] = osa_result(&s->agg[ai], &pick[ai]);
			if (cur != UINT32_MAX && pos != nrows[cur])
				elog(ERROR, "sift: ordered-set values of a group are %lld, expected %lld", (long long) pos, (long long) nrows[cur]);
			if (!more)
				break;
			cur = r.sid;
			pos = 0;
			for (int ai = 0; ai < s->naggs; ai++)
				if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
					osa_pick(&s->agg[ai], nrows[cur], &pick[ai]);
		}
		for (int ai = 0; ai < s->naggs; ai++)
			if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
				for (int k = 0; k < 2; k++)
					if (pick[ai].pos[k] >= pos && pick[ai].pos[k] < pos + (int64) r.cnt)
						pick[ai].key[k] = r.key;
		pos += r.cnt;
	}
	if (o->sort)
	{
		tuplesort_end(o->sort);
		o->sort = NULL;
	}
}

static void
osa_finish_all(AggExec *ex)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		if (is_ordered_agg(&ex->spec->agg[ai]) && ex->spec->agg[ai].share == ai)
			osa_finish(ex, ai);
}

/*
 * An aggregate of an expression over a column of a joined table: the combo gives the id (0: NULL) of the value of each tuple, in the
 * table of the distinct values of the aggregate. count(x) counts the ids; the others add the (group, id) pairs to a set (with the
 * times each was added, for sum and avg) that mapped_finish turns into the result with PostgreSQL's own comparison and arithmetic.
 */
static void
mapped_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	const uint32 *out = combo_outs(ex, a->combo),
			   *val = ex->cs[a->combo].val;
	int			ns = ex->spec->combo[a->combo].nslots;

	for (int j = 0; j < nsel; j++)
	{
		uint32		id = val[(size_t) out[j] * ns + a->slot];

		if (id == 0 || (ok && !sift_colsrc_bit(ok, j)))
			continue;
		if (a->kind == SAA_M_COUNT || (dset_add(ex, &ex->dset[ai], ex->gid[j], id) && a->kind == SAA_M_DISTINCT))
			acc->n[ex->gid[j]]++;
	}
}

/* `times` rows of group g had the value id: min, max, sum or avg of the aggregate ai */
static void
mapped_add(AggExec *ex, int ai, uint32 g, uint32 id, int64 times)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	Datum		val = ex->aot[ai].val[id - 1];

	switch (a->kind)
	{
		case SAA_M_MIN:
		case SAA_M_MAX:
			{
				FmgrInfo   *cmp = &lookup_type_cache(a->argtype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
				int32		c = acc->n[g] ? DatumGetInt32(FunctionCall2Coll(cmp, a->collid, val, acc->v[g])) : 0;

				if (acc->n[g] == 0 || (a->kind == SAA_M_MIN ? c < 0 : c > 0))
					acc->v[g] = val;
				acc->n[g] = 1;
			}
			break;
		default:
			if (a->restype == NUMERICOID)
			{
				Datum		sum = DirectFunctionCall2(numeric_mul, DirectFunctionCall1(int8_numeric, Int64GetDatum(times)), val);

				acc->v[g] = acc->n[g] ? DirectFunctionCall2(numeric_add, acc->v[g], sum) : sum;
			}
			else
			{
				double		x = DatumGetFloat8(val);

				acc->d[g] += times * x;
				if (!isfinite(x))
					acc->nf[g] = 1;
				else
					ex->amax[ai] = Max(ex->amax[ai], fabs(x));
			}
			acc->n[g] += times;
	}
}

/* the results of the aggregates over distinct values, from the pairs of their sets */
static void
mapped_finish(AggExec *ex)
{
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		const DSet *d = &ex->dset[ai];

		if (a->kind < SAA_M_MIN || a->kind > SAA_M_AVG)
			continue;
		for (uint64 i = 0; i < d->cap; i++)
			if (d->slot[i])
				mapped_add(ex, ai, (d->slot[i] >> 32) & 0x7fffffff, (uint32) d->slot[i], d->counted ? d->cnt[i] : 1);
		if (a->kind == SAA_M_AVG && a->restype == FLOAT8OID && 8.0 * ex->n_tuples * ex->amax[ai] * ex->amax[ai] >= DBL_MAX)
			float_overflow_error();
	}
	MemoryContextSwitchTo(old);
}

static Datum
mapped_sum_datum(AggExec *ex, const SiftAggAgg *a, int ai, uint32 g, bool *isnull)
{
	AggAcc	   *acc = &ex->acc[ai];

	*isnull = acc->n[g] == 0;
	if (*isnull)
		return (Datum) 0;
	if (a->restype == NUMERICOID)
		return a->kind == SAA_M_SUM ? acc->v[g] :
			DirectFunctionCall2(numeric_div, acc->v[g], DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
	if (isinf(acc->d[g]) && !acc->nf[g])
		float_overflow_error();
	return Float8GetDatum(a->kind == SAA_M_SUM ? acc->d[g] : acc->d[g] / (double) acc->n[g]);
}

/* GROUPING(args) in a group of grouping set s: bit i (the first argument is the highest) is 1 when argument i is not grouped by the set */
static int32
grouping_bits(AggExec *ex, const SiftAggAgg *a, uint32 g)
{
	uint32		mask = ex->spec->setmask[key_id(ex, ex->spec->nkeys - 1, g) - 1];
	int32		bits = 0;

	for (int i = 0; i < a->nref; i++)
		bits = bits << 1 | !(mask >> a->ref[i] & 1);
	return bits;
}

/* true when the number is new in the group: distinct numbers have one id, a (group, id) pair is counted once */
static bool
num_first(AggExec *ex, int ai, uint32 g, sift_i128 v)
{
	return dset_add(ex, &ex->dset[ai], g, idmap_id(&ex->amap[ai], v));
}

static void
agg_update(AggExec *ex, int ai, int start, int n, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	const uint64 *ok;
	bool		all;
	const uint32 *idx = NULL;
	const SiftSrcVec *v = NULL;
	const SiftSrcLeaf *lf = NULL;
	const double *x;
	const sift_i128 *nx;

	agg_selection(ex, a, start, n, nsel, &all);
	ok = all ? NULL : ex->okm;
	if (a->leaf >= 0)
	{
		v = get_vec(ex, a->leaf);
		lf = &ex->lay->leaf[a->leaf];
		gather_idx(ex, a->leaf, nsel, &idx);
	}
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
			sift_aggk->count_gid(acc->n, ex->gid, ok, nsel);
			break;
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			sift_aggk->gather_u32(ex->cu32, (const uint32 *) v->data, idx, nsel);
			sift_aggk->sum_gid_i32(acc->i, ex->gid, (const int32 *) ex->cu32, ok, nsel);
			sift_aggk->count_gid(acc->n, ex->gid, ok, nsel);
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			if (a->nvx)
				x = eval_vx(ex, a, nsel);
			else
			{
				sift_aggk->gather_u64(ex->c64, (const uint64 *) v->data, idx, nsel);
				x = (const double *) ex->c64;
			}
			float_guard(ex, ai, x, nsel);
			sift_aggk->sum_gid_f64(acc->d, ex->gid, x, ok, nsel);
			sift_aggk->count_gid(acc->n, ex->gid, ok, nsel);
			break;
		case SAA_MIN:
		case SAA_MAX:
			{
				bool		ismin = a->kind == SAA_MIN;

				if (a->num.n)
				{
					nx = eval_nx(ex, &a->num, nsel, ex->tidx, ex->ridx, NULL);
					for (int j = 0; j < nsel; j++)
					{
						uint32		g = ex->gid[j];

						if ((ok && !sift_colsrc_bit(ok, j)))
							continue;
						if (acc->n[g] == 0 || (ismin ? nx[j] < acc->w[g] : nx[j] > acc->w[g]))
							acc->w[g] = nx[j];
						acc->n[g]++;
					}
					break;
				}
				if (lf->vtype == SCV_I32)
					sift_aggk->gather_u32(ex->cu32, (const uint32 *) v->data, idx, nsel);
				else
					sift_aggk->gather_u64(ex->c64, (const uint64 *) v->data, idx, nsel);
				for (int j = 0; j < nsel; j++)
				{
					uint32		g = ex->gid[j];

					if (ok && !sift_colsrc_bit(ok, j))
						continue;
					if (lf->vtype == SCV_F64)
					{
						double		x = ((const double *) ex->c64)[j];

						if (acc->n[g] == 0 || (ismin ? float8_cmp_internal(x, acc->d[g]) < 0 : float8_cmp_internal(x, acc->d[g]) > 0))
							acc->d[g] = x;
					}
					else
					{
						int64		x = lf->vtype == SCV_I32 ? ((const int32 *) ex->cu32)[j] : ((const int64 *) ex->c64)[j];

						if (acc->n[g] == 0 || (ismin ? x < acc->i[g] : x > acc->i[g]))
							acc->i[g] = x;
					}
					acc->n[g]++;
				}
			}
			break;
		case SAA_COUNT_ROWS:
			for (int j = 0; j < nsel; j++)
			{
				uint32		g = ex->gid[j];
				uint64		row = ex->row_base + ex->ridx[j];

				if (ok && !sift_colsrc_bit(ok, j))
					continue;
				if (acc->last[g] != row)
				{
					acc->last[g] = row;
					acc->n[g]++;
				}
			}
			break;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			nx = eval_nx(ex, &a->num, nsel, ex->tidx, ex->ridx, NULL);
			for (int j = 0; j < nsel; j++)
				if ((!ok || sift_colsrc_bit(ok, j)) && (!a->distinct || num_first(ex, ai, ex->gid[j], nx[j])))
				{
					acc->w[ex->gid[j]] += nx[j];
					acc->n[ex->gid[j]]++;
				}
			break;
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
			if (a->share == ai)
				osa_update(ex, ai, ok, nsel);
			sift_aggk->count_gid(acc->n, ex->gid, ok, nsel);
			break;
		case SAA_COUNT_DISTINCT:
			if (a->num.n)
			{
				nx = eval_nx(ex, &a->num, nsel, ex->tidx, ex->ridx, NULL);
				for (int j = 0; j < nsel; j++)
					if ((!ok || sift_colsrc_bit(ok, j)) && num_first(ex, ai, ex->gid[j], nx[j]))
						acc->n[ex->gid[j]]++;
				break;
			}
			sift_aggk->gather_u32(ex->cu32, (const uint32 *) v->data, idx, nsel);
			if (lf->vtype == SCV_CODE)
			{
				const uint32 *map = dict_amap(ex, dict_slot(ex, v->dict), v->dict, ai);

				for (int j = 0; j < nsel; j++)
					ex->cu32[j] = map[v->isnull && v->isnull[idx[j]] ? v->dict->n : ex->cu32[j]];
			}
			for (int j = 0; j < nsel; j++)
				if ((!ok || sift_colsrc_bit(ok, j)) && dset_add(ex, &ex->dset[ai], ex->gid[j], ex->cu32[j]))
					acc->n[ex->gid[j]]++;
			break;
		case SAA_GROUPING:
			break;
		case SAA_M_COUNT:
		case SAA_M_DISTINCT:
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_M_SUM:
		case SAA_M_AVG:
			mapped_update(ex, ai, ok, nsel);
			break;
		default:
			elog(ERROR, "sift: aggregate kind %d is not a scan aggregate", a->kind);
	}
}

/* the conjuncts over the joined rows (boolean slots of the join combos): the tuples that pass, moved to the front; the new count */
static int
post_filter(AggExec *ex, Tup *t)
{
	int			k = 0;

	for (int j = 0; j < t->n; j++)
	{
		bool		keep = true;

		for (int i = 0; keep && i < ex->spec->npost; i++)
		{
			const SiftAggPred *p = ex->spec->post[i];

			keep = ex->cs[p->leaf].val[(size_t) t->cout[p->leaf][j] * ex->spec->combo[p->leaf].nslots + p->op] != 0;
		}
		if (!keep)
			continue;
		t->sel[k] = t->sel[j];
		t->tidx[k] = t->tidx[j];
		t->ridx[k] = t->ridx[j];
		for (int x = 0; x < ex->nj; x++)
			t->cout[ex->jcombo[x]][k] = t->cout[ex->jcombo[x]][j];
		k++;
	}
	return k;
}

static void arm_chunk(AggExec *ex, Tup *t);

/* the tuples that passed the joins and the filters over the joined rows: their groups and aggregates */
static void
agg_tuples(AggExec *ex, Tup *t)
{
	SiftAggSpec *s = ex->spec;

	ex->cur = t;
	ex->sel = t->sel;
	ex->tidx = t->tidx;
	ex->ridx = t->ridx;
	memset(ex->cfree_ok, 0, sizeof(ex->cfree_ok));
	if (s->npost > 0)
		t->n = post_filter(ex, t);
	if (t->n == 0)
		return;
	if (s->narms > 0)
	{
		arm_chunk(ex, t);
		return;
	}
	ex->n_tuples += t->n;
	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		ex->curset = set;
		ex->arm_mask = s->nsets ? s->setmask[set] : ~(uint32) 0;
		compute_gids(ex, t->n);
		sift_aggk->count_gid(ex->seen, ex->gid, NULL, t->n);
		for (int i = 0; i < s->naggs; i++)
			agg_update(ex, i, ex->cstart, ex->cn, t->n);
	}
	ex->arm_mask = ~(uint32) 0;
}

/* applies the lookup joins one at a time: a tuple is repeated per match of its key, and dropped (INNER) or kept once (LEFT) without one */
static void
join_stage(AggExec *ex, int s, Tup *in)
{
	int			ci,
				n = in->n;
	ComboSt    *cs;
	Tup		   *own;
	uint32	   *eid;
	bool		plain = true;

	if (s == ex->nj)
	{
		agg_tuples(ex, in);
		return;
	}
	ci = ex->jcombo[s];
	cs = &ex->cs[ci];
	own = &ex->stage[s];
	eid = ex->peid;
	combo_resolve(ex, ci, ex->qlevel, n, in->tidx, in->ridx, eid);
	for (int i = 0; i < n; i++)
		plain &= cs->ent[eid[i]].nout == 1;
	if (plain)
	{
		Tup			al = *in;

		for (int i = 0; i < n; i++)
			own->cout[ci][i] = cs->ent[eid[i]].base;
		al.cout[ci] = own->cout[ci];
		join_stage(ex, s + 1, &al);
		return;
	}
	/* the entries of this stage's tuples stay in eid only until the next stage resolves its own: keep a copy */
	eid = memcpy(ex->seid[s], eid, n * sizeof(uint32));
	for (int j = 0, e = 0; j < n;)
	{
		int			k = 0;

		for (; j < n && k < VEC; k++)
		{
			const ComboEnt *en = &cs->ent[eid[j]];

			if (e >= en->nout)
			{
				j++;
				e = 0;
				k--;
				continue;
			}
			own->sel[k] = in->sel[j];
			own->tidx[k] = in->tidx[j];
			own->ridx[k] = in->ridx[j];
			for (int x = 0; x < s; x++)
				own->cout[ex->jcombo[x]][k] = in->cout[ex->jcombo[x]][j];
			own->cout[ci][k] = en->base + e;
			if (++e >= en->nout)
			{
				j++;
				e = 0;
			}
		}
		own->n = k;
		if (k > 0)
			join_stage(ex, s + 1, own);
	}
}

/* one chunk of tuples [start, start + n) selected by mask */
static void
agg_chunk(AggExec *ex, int start, int n, const uint64 *mask)
{
	Tup		   *t = &ex->t0;
	int			nw = (n + 63) / 64;

	t->n = sift_aggk->mask_sel(t->sel, mask, nw);
	if (t->n == 0)
		return;
	ex->cstart = start;
	ex->cn = n;
	for (int j = 0; j < t->n; j++)
		t->tidx[j] = start + t->sel[j];
	if (ex->spec->group >= 0)
		for (int j = 0; j < t->n; j++)
			t->ridx[j] = ex->parent[t->tidx[j]];
	else
		memcpy(t->ridx, t->tidx, t->n * sizeof(uint32));
	join_stage(ex, 0, t);
}

/* ---------- two level: per-row aggregates under an outer aggregate ---------- */

/* the cells of every arm to newcap groups, renumbered through remap (NULL: as they are); the touched list follows */
static void
arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	SiftAggSpec *s = ex->spec;

	for (int k = 0; k < s->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];
		SiftAggArm *sa = &s->arm[k];
		uint64	   *nord = MemoryContextAlloc(ex->tcxt, newcap * sizeof(uint64));

		memset(nord, 0xff, newcap * sizeof(uint64));
		for (int ai = 0; ai < sa->ninner; ai++)
		{
			int64	   *nn = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64)),
					   *ni = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64));
			double	   *nd = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
			uint8	   *nnf = MemoryContextAllocZero(ex->tcxt, newcap);

			for (uint32 g = 0; g < oldcap && a->ord; g++)
			{
				uint32		t = remap ? remap[g] : g;

				if (t == UINT32_MAX || a->ord[g] == UINT64_MAX)
					continue;
				nn[t] = a->n[ai][g];
				ni[t] = a->i[ai][g];
				nd[t] = a->d[ai][g];
				nnf[t] = a->nf[ai][g];
			}
			a->n[ai] = nn;
			a->i[ai] = ni;
			a->d[ai] = nd;
			a->nf[ai] = nnf;
		}
		for (uint32 g = 0; g < oldcap && a->ord; g++)
		{
			uint32		t = remap ? remap[g] : g;

			if (t != UINT32_MAX && a->ord[g] != UINT64_MAX)
				nord[t] = a->ord[g];
		}
		a->ord = nord;
	}
	if (remap)
		for (uint32 t = 0; t < ex->ntouch; t++)
			ex->touched[t].g = remap[ex->touched[t].g];
}

static void
arms_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	ex->arm = MemoryContextAllocZero(ex->cxt, Max(s->narms, 1) * sizeof(ArmSt));
	ex->arm_mask = ~(uint32) 0;
	ex->cur_order = UINT64_MAX;
	for (int k = 0; k < s->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];
		int			ni = s->arm[k].ninner;

		a->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		a->ok = MemoryContextAlloc(ex->cxt, maxv);
		a->iok = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(uint8 *));
		a->dx = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(double *));
		a->ix = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->n = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->i = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->d = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(double *));
		a->nf = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(uint8 *));
		a->amax = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(double));
		for (int ai = 0; ai < ni; ai++)
		{
			a->iok[ai] = MemoryContextAlloc(ex->cxt, maxv);
			a->dx[ai] = MemoryContextAlloc(ex->cxt, maxv * sizeof(double));
			a->ix[ai] = MemoryContextAlloc(ex->cxt, maxv * sizeof(int64));
		}
	}
	ex->captouch = 1024;
	ex->touched = MemoryContextAlloc(ex->cxt, ex->captouch * sizeof(Touch));
}

/* the inner aggregate ai of arm k for the tuples of the chunk: the ones it takes and their values */
static void
arm_inner_prepare(AggExec *ex, ArmSt *a, SiftAggAgg *ia, int ai, int n)
{
	bool		all;
	const uint32 *idx;
	const SiftSrcVec *v;

	agg_selection(ex, ia, ex->cstart, ex->cn, n, &all);
	for (int j = 0; j < n; j++)
		a->iok[ai][j] = all || sift_colsrc_bit(ex->okm, j);
	switch (ia->kind)
	{
		case SAA_SUM_INT:
			v = get_vec(ex, ia->leaf);
			gather_idx(ex, ia->leaf, n, &idx);
			for (int j = 0; j < n; j++)
				a->ix[ai][j] = ((const int32 *) v->data)[idx[j]];
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			if (ia->nvx)
				memcpy(a->dx[ai], eval_vx(ex, ia, n), n * sizeof(double));
			else
			{
				v = get_vec(ex, ia->leaf);
				gather_idx(ex, ia->leaf, n, &idx);
				for (int j = 0; j < n; j++)
					a->dx[ai][j] = ((const double *) v->data)[idx[j]];
			}
			break;
		default:
			break;
	}
}

static void
cell_start(AggExec *ex, int k, ArmSt *a, uint32 g, uint64 row)
{
	a->ord[g] = row;
	for (int ai = 0; ai < ex->spec->arm[k].ninner; ai++)
	{
		a->n[ai][g] = 0;
		a->i[ai][g] = 0;
		a->d[ai][g] = 0;
		a->nf[ai][g] = 0;
	}
	if (ex->ntouch == ex->captouch)
	{
		ex->captouch *= 2;
		ex->touched = repalloc(ex->touched, ex->captouch * sizeof(Touch));
	}
	ex->touched[ex->ntouch].arm = k;
	ex->touched[ex->ntouch++].g = g;
	ex->seen[g]++;
}

static void
cell_add(ArmSt *a, SiftAggAgg *ia, int ai, uint32 g, int j)
{
	switch (ia->kind)
	{
		case SAA_SUM_INT:
			a->i[ai][g] += a->ix[ai][j];
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			{
				double		x = a->dx[ai][j];

				a->d[ai][g] += x;
				if (!isfinite(x))
					a->nf[ai][g] = 1;
				else
					a->amax[ai] = Max(a->amax[ai], fabs(x));
			}
			break;
		default:
			break;
	}
	a->n[ai][g]++;
}

/* the value of inner aggregate ai in a cell: false for NULL */
static bool
cell_value(ArmSt *a, SiftAggAgg *ia, int ai, uint32 g, double *dv, int64 *iv)
{
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
			*iv = a->n[ai][g];
			return true;
		case SAA_SUM_INT:
			*iv = a->i[ai][g];
			return a->n[ai][g] > 0;
		case SAA_SUM_F64:
			if (isinf(a->d[ai][g]) && !a->nf[ai][g])
				float_overflow_error();
			*dv = a->d[ai][g];
			return a->n[ai][g] > 0;
		case SAA_AVG_F64:
			if (a->n[ai][g] == 0)
				return false;
			*dv = a->d[ai][g] / (double) a->n[ai][g];
			return true;
		default:
			elog(ERROR, "sift: unexpected inner aggregate");
	}
	return false;
}

static bool
cond_holds(const SiftAggCond *c, double dv, int64 iv)
{
	int			cmp = c->isint ? pg_cmp_s64(iv, c->ci) : float8_cmp_internal(dv, c->cd);

	switch ((SiftCmpOp) c->op)
	{
		case SCMP_EQ:
			return cmp == 0;
		case SCMP_NE:
			return cmp != 0;
		case SCMP_LT:
			return cmp < 0;
		case SCMP_LE:
			return cmp <= 0;
		case SCMP_GT:
			return cmp > 0;
		case SCMP_GE:
			return cmp >= 0;
	}
	return false;
}

/* the cell (arm k, group g) of the row `row` into the outer aggregates */
static void
flush_cell(AggExec *ex, int k, uint32 g, uint64 row)
{
	SiftAggArm *sa = &ex->spec->arm[k];
	ArmSt	   *a = &ex->arm[k];

	for (int h = 0; h < sa->nhaving; h++)
	{
		double		dv = 0;
		int64		iv = 0;

		if (!cell_value(a, &sa->inner[sa->having[h].col], sa->having[h].col, g, &dv, &iv) || !cond_holds(&sa->having[h], dv, iv))
		{
			ex->seen[g]--;		/* the cell is no group of the arm */
			return;
		}
	}
	for (int oi = 0; oi < ex->spec->naggs; oi++)
	{
		SiftAggAgg *o = &ex->spec->agg[oi];
		AggAcc	   *acc = &ex->acc[oi];
		bool		pass = true;
		double		dv = 0;
		int64		iv = 0;

		for (int c = 0; pass && c < o->ncond; c++)
		{
			int			src = sa->colsrc[o->cond[c].col];

			pass = cell_value(a, &sa->inner[src], src, g, &dv, &iv) && cond_holds(&o->cond[c], dv, iv);
		}
		if (!pass)
			continue;
		if (o->kind == SAA_O_COUNT_STAR)
		{
			acc->n[g]++;
			continue;
		}
		if (o->kind == SAA_O_COUNT_ROWS)
		{
			if (acc->last[g] != row)
			{
				acc->last[g] = row;
				acc->n[g]++;
			}
			continue;
		}
		if (!cell_value(a, &sa->inner[sa->colsrc[o->col]], sa->colsrc[o->col], g, &dv, &iv))
			continue;
		acc->n[g]++;
		if (o->kind == SAA_O_SUM_I64)
		{
			if (pg_add_s64_overflow(acc->i[g], iv, &acc->i[g]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("sift: a sum of the aggregate scan exceeds bigint"),
						 errhint("Set sift.enable_agg to off for this query.")));
		}
		else
		{
			acc->d[g] += dv;
			if (!isfinite(dv))
				acc->nf[g] = 1;
		}
	}
}

static void
arms_flush(AggExec *ex)
{
	for (uint32 t = 0; t < ex->ntouch; t++)
		flush_cell(ex, ex->touched[t].arm, ex->touched[t].g, ex->cur_order);
	ex->ntouch = 0;
}

/* the tuples of the chunk through every arm: the tuples of a row come together, so a cell lives for one row */
static void
arm_chunk(AggExec *ex, Tup *t)
{
	SiftAggSpec *s = ex->spec;
	int			n = t->n;
	uint32		gen;

	ex->n_tuples += n;
	do
	{
		gen = ex->gen;
		for (int k = 0; k < s->narms && gen == ex->gen; k++)
		{
			ArmSt	   *a = &ex->arm[k];

			ex->arm_mask = s->arm[k].keymask;
			compute_gids(ex, n);
			memcpy(a->gid, ex->gid, n * sizeof(uint32));
		}
	} while (gen != ex->gen);
	ex->arm_mask = ~(uint32) 0;
	for (int k = 0; k < s->narms; k++)
	{
		SiftAggArm *sa = &s->arm[k];
		ArmSt	   *a = &ex->arm[k];

		memset(a->ok, 1, n);
		for (int f = 0; f < sa->nfilter; f++)
		{
			const SiftAggPred *p = sa->filter[f];
			const uint32 *out = combo_outs(ex, p->leaf);
			const uint32 *val = ex->cs[p->leaf].val;
			int			ns = s->combo[p->leaf].nslots;

			for (int j = 0; j < n; j++)
				a->ok[j] &= val[(size_t) out[j] * ns + p->op] != 0;
		}
		for (int ai = 0; ai < sa->ninner; ai++)
			arm_inner_prepare(ex, a, &sa->inner[ai], ai, n);
	}
	for (int j = 0; j < n; j++)
	{
		uint64		row = ex->row_base + ex->ridx[j];

		if (row != ex->cur_order)
		{
			arms_flush(ex);
			ex->cur_order = row;
		}
		for (int k = 0; k < s->narms; k++)
		{
			ArmSt	   *a = &ex->arm[k];
			SiftAggArm *sa = &s->arm[k];
			uint32		g = a->gid[j];

			if (!a->ok[j])
				continue;
			if (a->ord[g] != row)
				cell_start(ex, k, a, g, row);
			for (int ai = 0; ai < sa->ninner; ai++)
				if (a->iok[ai][j])
					cell_add(a, &sa->inner[ai], ai, g, j);
		}
	}
	for (int k = 0; k < s->narms; k++)
		for (int ai = 0; ai < s->arm[k].ninner; ai++)
			if (s->arm[k].inner[ai].kind == SAA_AVG_F64 &&
				8.0 * ex->n_tuples * ex->arm[k].amax[ai] * ex->arm[k].amax[ai] >= DBL_MAX)
				float_overflow_error();
}

/* ---------- visibility ---------- */

/* ---------- a unit ---------- */

static void
build_parents(AggExec *ex)
{
	SiftSrcUnit *u = &ex->unit;
	uint32		m;

	ex->offs = elem_offs(ex, ex->spec->group);
	ex->nelem = m = ex->offs[u->nrow];
	ex->parent = MemoryContextAllocZero(ex->ucxt, (Size) m * sizeof(uint32) + SIFT_AGG_PAD);
	/* the row is written at the offset of its first element (an empty row is overwritten by the next one that has any), then carried forward */
	for (int r = 0; r < u->nrow; r++)
		ex->parent[ex->offs[r]] = r;
	for (uint32 e = 1, p = ex->parent[0]; e < m; e++)
		ex->parent[e] = p = Max(p, ex->parent[e]);
}

static void
process_unit(AggExec *ex)
{
	SiftSrcUnit *u = &ex->unit;
	SiftAggSpec *s = ex->spec;
	int			nrow = u->nrow,
				nrw = (nrow + 63) / 64;
	uint64	   *rmask;
	bool		elem = s->group >= 0;

	MemoryContextReset(ex->ucxt);
	memset(ex->uvec_ok, 0, ex->lay->nleaves * sizeof(bool));
	memset(ex->gofs, 0, ex->lay->nleaves * sizeof(uint32 *));
	for (int i = 0; i < s->npreds_total; i++)
		ex->ps[i].rowmask_ok = false;
	ex->n_units++;
	ex->n_rows += nrow;
	if (nrow == 0)
		return;
	rmask = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));
	sift_colsrc_live(ex->src, u, rmask);
	/* row level conjuncts */
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == 0)
			sift_aggk->and_w(rmask, row_bitmap(ex, s->pred[i]), nrw);
	ex->qlevel = elem;
	if (!elem)
	{
		sift_colsrc_vet(ex->src, u, rmask);
		for (int st = 0; st < nrow; st += VEC)
			agg_chunk(ex, st, Min(VEC, nrow - st), rmask + st / 64);
	}
	else
	{
		uint64	   *em;
		int			m;
		uint64		cm[VECW + PADW];

		build_parents(ex);
		m = ex->nelem;
		em = MemoryContextAlloc(ex->ucxt, ((m + 63) / 64 + PADW) * sizeof(uint64));
		for (int st = 0; st < m; st += VEC)
		{
			int			n = Min(VEC, m - st),
						nw = (n + 63) / 64;

			sift_aggk->gather_bits(cm, rmask, ex->parent + st, n);
			for (int i = 0; i < s->npred; i++)
				if (s->pred[i]->level == 1)
				{
					uint64		t[VECW + PADW];

					eval_pred(ex, s->pred[i], 1, st, n, t);
					sift_aggk->and_w(cm, t, nw);
				}
			memcpy(em + st / 64, cm, nw * sizeof(uint64));
		}
		{
			/* the rows that keep an element, checked against the snapshot */
			uint64	   *rv = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64));

			for (int w = 0; w < (m + 63) / 64; w++)
				for (uint64 b = em[w]; b; b &= b - 1)
				{
					uint32		r = ex->parent[w * 64 + pg_rightmost_one_pos64(b)];

					rv[r >> 6] |= UINT64CONST(1) << (r & 63);
				}
			sift_colsrc_vet(ex->src, u, rv);
			for (int st = 0; st < m; st += VEC)
			{
				int			n = Min(VEC, m - st),
							nw = (n + 63) / 64;

				sift_aggk->gather_bits(cm, rv, ex->parent + st, n);
				sift_aggk->and_w(em + st / 64, cm, nw);
			}
		}
		for (int st = 0; st < m; st += VEC)
			agg_chunk(ex, st, Min(VEC, m - st), em + st / 64);
	}
	ex->row_base += nrow;
	CHECK_FOR_INTERRUPTS();
}

/* ---------- the scan ---------- */

static void
init_states(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->ps = MemoryContextAllocZero(ex->cxt, Max(s->npreds_total, 1) * sizeof(PredState));
	ex->ks = MemoryContextAllocZero(ex->cxt, Max(s->nkeys, 1) * sizeof(KeyState));
}

static void
register_pred(AggExec *ex, SiftAggPred *p)
{
	PredState  *st;

	if (p == NULL)
		return;
	st = &ex->ps[p->id];
	if (p->kind == SAP_LUT)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		st->expr = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
		st->param = find_param(p->expr);
		rt_init(&st->rt, 1);
		if (ex->lay->leaf[p->leaf].vtype == SCV_I32)
			st->null_result = eval_bool(ex, st->expr, (Datum) 0, true);
		MemoryContextSwitchTo(old);
	}
	for (int i = 0; i < p->nchild; i++)
		register_pred(ex, p->child[i]);
}

/* The interval each leaf's comparisons leave: a unit whose zone misses it holds no row that passes (docs/COL-PLAN.md, "Zone pruning") */
static void
prune_init(AggExec *ex)
{
	List	   *cmps = sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase);

	ex->prune = MemoryContextAlloc(ex->cxt, Max(list_length(cmps), 1) * sizeof(SiftSrcPrune));
	foreach_ptr(SiftAggPred, p, cmps)
	{
		PredState  *st = &ex->ps[p->id];
		SiftSrcPrune *z = NULL;
		int64		c;

		if (!st->ready)
			eval_operand(ex, p, st);
		if (st->cnull)
			continue;
		for (int k = 0; k < ex->nprune; k++)
			if (ex->prune[k].leaf == p->leaf)
				z = &ex->prune[k];
		if (z == NULL)
		{
			z = &ex->prune[ex->nprune++];
			*z = (SiftSrcPrune) {p->leaf, PG_INT64_MIN, PG_INT64_MAX};
		}
		c = st->ci;
		if (p->op != SCMP_GT && p->op != SCMP_GE)
			z->hi = Min(z->hi, p->op == SCMP_LT && c > PG_INT64_MIN ? c - 1 : c);
		if (p->op != SCMP_LT && p->op != SCMP_LE)
			z->lo = Max(z->lo, p->op == SCMP_GT && c < PG_INT64_MAX ? c + 1 : c);
	}
}

/* what a key keeps for the whole scan: its expression and the type of its input; the ids are the table's (table_init) */
static void
register_key(AggExec *ex, int ki)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcLeaf *lf = key_lf(ex, ki);
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	if (sk->kind == SAK_SET)
		k->ident = true;
	else if (sk->kind == SAK_LUT)
	{
		k->expr = ExecInitExpr((Expr *) sk->expr, EXPR_PARENT(ex));
		k->param = find_param(sk->expr);
	}
	else if (sk->kind == SAK_NUM)
	{
		k->numk = true;
		idmap_init(&k->im);
	}
	else if (sk->kind == SAK_LEAF)
	{
		/* the leaf itself: the placeholder has the key's type */
		k->param = makeNode(Param);
		k->param->paramkind = PARAM_EXEC;
		k->param->paramtype = sk->typid;
		k->param->paramtypmod = sk->typmod;
		k->param->paramcollid = sk->collid;
		k->ident = lf->vtype == SCV_I32 || lf->vtype == SCV_F64;
		if (lf->vtype == SCV_F64)
			ident_go_wide(ex, k);
	}
	MemoryContextSwitchTo(old);
}

static void
register_combos(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->comcxt = AllocSetContextCreate(ex->cxt, "sift agg combo", ALLOCSET_SMALL_SIZES);
	ex->cs = palloc0(Max(s->ncombo, 1) * sizeof(ComboSt));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		SiftAggCombo *cb = &s->combo[ci];
		ComboSt    *cs = &ex->cs[ci];

		cs->nht = 64;
		cs->ht = palloc0(cs->nht * sizeof(uint32));
		cs->slot = palloc(Max(cb->nslots, 1) * sizeof(ExprState *));
		for (int sl = 0; sl < cb->nslots; sl++)
			cs->slot[sl] = ExecInitExpr((Expr *) cb->slot[sl].expr, EXPR_PARENT(ex));
		ex->clbase[ci] = ex->ncl;
		ex->ncl += cb->nleaves;
		if (cb->join >= 0)
			ex->jcombo[ex->nj++] = ci;
	}
	MemoryContextSwitchTo(old);
}

/* An empty group table, all in tcxt: key ids, group arrays, count(DISTINCT) sets */
static void
table_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
	uint32		total = 1;

	ex->hash = false;
	ex->direct_max = ex->lanes ? LANE_DIRECT_MAX : DIRECT_MAX;
	ex->ngroups = 0;
	ex->hkey = NULL;
	ex->hkcap = 0;
	ex->hslot = NULL;
	ex->nhslot = 0;
	ex->row_base = 0;
	for (int j = 0; j < s->nkeys; j++)
	{
		KeyState   *k = &ex->ks[j];
		KeyState	keep = {.expr = k->expr, .param = k->param, .ident = k->ident, .numk = k->numk};

		*k = keep;
		ot_init(&k->ot, s->key[j].eqop, s->key[j].typid, s->key[j].collid);
		rt_init(&k->rt, sizeof(uint32));
		k->cap = 4;
		k->nullid = UINT32_MAX;
		if (s->key[j].kind == SAK_SET)
			for (int i = 0; i < s->nsets; i++)
				ident_add(ex, k, i);
		else if (k->numk)
			idmap_init(&k->im);
		else if (s->key[j].kind == SAK_LEAF && key_lf(ex, j)->vtype == SCV_F64)
			ident_go_wide(ex, k);
	}
	/* room for the groups the planner expects of one key, instead of doubling the arrays as the ids come */
	if (s->nkeys == 1 && !ex->lanes && s->est_groups > PRESIZE_MIN && s->est_groups <= ex->direct_max)
		ex->ks[0].cap = pg_nextpower2_32((uint32) s->est_groups);
	direct_layout(ex);
	for (int j = 0; j < s->nkeys; j++)
		total *= ex->ks[j].cap;
	ex->gcap = total;
	ex->seen = palloc0(total * sizeof(int64));
	ex->acc = acc_alloc(ex, total);
	arms_resize(ex, total, NULL, 0);
	ex->dset = palloc0(Max(s->naggs, 1) * sizeof(DSet));
	ex->aot = palloc0(Max(s->naggs, 1) * sizeof(OutTable));
	ex->amax = palloc0(Max(s->naggs, 1) * sizeof(double));
	ex->amap = palloc0(Max(s->naggs, 1) * sizeof(IdMap));
	ex->osa = palloc0(Max(s->naggs, 1) * sizeof(Osa *));
	ex->oval = palloc0(Max(s->naggs, 1) * sizeof(double *));
	for (int i = 0; i < s->naggs; i++)
	{
		SiftAggAgg *a = &s->agg[i];
		TypeCacheEntry *tc;

		if (is_ordered_agg(a))
			ex->osa[i] = a->share == i ? osa_new(ex) : ex->osa[a->share];
		if (a->num.n && (a->kind == SAA_COUNT_DISTINCT || a->distinct))
			idmap_init(&ex->amap[i]);
		if (a->kind >= SAA_M_COUNT && a->kind <= SAA_M_AVG)
			ex->dset[i].counted = a->kind == SAA_M_SUM || a->kind == SAA_M_AVG;
		else if (a->kind != SAA_COUNT_DISTINCT || a->leaf < 0 || ex->lay->leaf[a->leaf].vtype != SCV_CODE)
			continue;
		tc = lookup_type_cache(a->argtype, TYPECACHE_EQ_OPR);
		ot_init(&ex->aot[i], tc->eq_opr, a->argtype, a->collid);
	}
	MemoryContextSwitchTo(old);
}

/* forgets every group (the dictionary maps hold key ids of the table) */
static void
table_reset(AggExec *ex)
{
	int			nk = Max(ex->spec->nkeys + ex->spec->naggs, 1);

	for (int i = 0; i < NDICT_SLOTS; i++)
		if (ex->dslot[i].kmap)
			memset(ex->dslot[i].kmap, 0, nk * sizeof(uint32 *));
	MemoryContextReset(ex->tcxt);
	table_init(ex);
}

static void
scan_buffers(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	arms_init(ex);

	ex->t0.sel = MemoryContextAlloc(ex->cxt, (maxv + 16) * sizeof(uint32));
	ex->t0.tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->t0.ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	for (int x = 0; x < ex->nj; x++)
	{
		Tup		   *st = &ex->stage[x];

		st->sel = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		for (int y = 0; y <= x; y++)
			st->cout[ex->jcombo[y]] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		ex->seid[x] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	}
	for (int p = 0; p < SIFT_AGG_MAX_CLEAVES; p++)
		ex->cvid[p] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		ex->cfree[ci] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		if (s->combo[ci].join >= 0)
			ex->cs[ci].map = map_load(ex, &s->join[s->combo[ci].join]);
	}
	ex->peid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pti = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pri = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->kids = MemoryContextAlloc(ex->cxt, (Size) maxv * SIFT_AGG_MAX_KEYS * sizeof(uint32));
	ex->cu32 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->c64 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	ex->okb = MemoryContextAlloc(ex->cxt, maxv);
	ex->nb = MemoryContextAlloc(ex->cxt, maxv);
	ex->vstk = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * maxv * sizeof(double));
	ex->pix = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->okm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm2 = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpu32 = MemoryContextAlloc(ex->cxt, (Size) maxv * SIFT_AGG_MAX_KEYS * sizeof(uint32));
}

/* ---------- partial group tables ---------- */

/*
 * A group table leaves a process as a blob: the key values its groups use, then per group the key ids (numbered from 1 in the
 * blob) and the states of the aggregates, in 8-byte aligned sections. blob_merge adds a blob to a table by value: the keys through
 * the table's own ids, the count(DISTINCT) sets by union, the sums in the order the blobs are merged (which the caller fixes).
 */
#define BLOB_MAGIC	0x53414731

typedef struct BlobHdr
{
	uint32		magic;
	int32		lane;
	uint32		ngroups;
	uint32		pad;
} BlobHdr;

static void
bw_put(StringInfo b, const void *p, Size n)
{
	appendBinaryStringInfo(b, p, n);
	while (b->len % 8)
		appendStringInfoChar(b, 0);
}

static const void *
br_get(const char **p, Size n)
{
	const void *r = *p;

	*p += TYPEALIGN(8, n);
	return r;
}

/* the aggregate arrays a state holds per group, besides the count: what acc_alloc made */
static void
bw_acc(StringInfo b, const AggAcc *acc, const uint32 *gl, uint32 ng)
{
	char	   *tmp = palloc(Max(ng, 1) * sizeof(int64));
	struct
	{
		const void *src;
		int			esize;
	}			col[] = {{acc->n, 8}, {acc->i, 8}, {acc->d, 8}, {acc->nf, 1}};

	for (int c = 0; c < lengthof(col); c++)
	{
		if (col[c].src == NULL)
			continue;
		for (uint32 r = 0; r < ng; r++)
			memcpy(tmp + (Size) r * col[c].esize, (const char *) col[c].src + (Size) gl[r] * col[c].esize, col[c].esize);
		bw_put(b, tmp, (Size) ng * col[c].esize);
	}
	pfree(tmp);
}

/* the values of an output table whose ids are listed in old[0..nused), as datums */
static void
bw_values(StringInfo b, const OutTable *t, const uint32 *old, uint32 nused)
{
	StringInfoData vb;
	uint32		hd[2];

	initStringInfo(&vb);
	for (uint32 i = 0; i < nused; i++)
	{
		Datum		d = t->val[old[i] - 1];
		Size		sz = datumEstimateSpace(d, false, t->byval, t->len);
		char	   *at;

		enlargeStringInfo(&vb, sz);
		at = vb.data + vb.len;
		datumSerialize(d, false, t->byval, t->len, &at);
		vb.len += sz;
	}
	hd[0] = nused;
	hd[1] = vb.len;
	bw_put(b, hd, sizeof(hd));
	if (vb.len)
		bw_put(b, vb.data, vb.len);
	pfree(vb.data);
}

/* renumbers the ids ids[0..n) from 1 in order of use (0 stays 0); returns the old ids by new id */
static uint32 *
renumber(uint32 *ids, uint32 n, uint32 nold, uint32 *nused)
{
	uint32	   *rm = palloc0((nold + 1) * sizeof(uint32)),
			   *old;

	*nused = 0;
	for (uint32 r = 0; r < n; r++)
		if (ids[r] && !rm[ids[r]])
			rm[ids[r]] = ++*nused;
	old = palloc(Max(*nused, 1) * sizeof(uint32));
	for (uint32 id = 1; id <= nold; id++)
		if (rm[id])
			old[rm[id] - 1] = id;
	for (uint32 r = 0; r < n; r++)
		ids[r] = ids[r] ? rm[ids[r]] : 0;
	pfree(rm);
	return old;
}

static uint32
group_key_id(AggExec *ex, int j, uint32 g)
{
	return ex->hash ? ex->hkey[(Size) g * SIFT_AGG_MAX_KEYS + j] : (g / ex->ks[j].mult) % ex->ks[j].cap;
}

static void
blob_key(AggExec *ex, int j, const uint32 *gl, uint32 ng, StringInfo b)
{
	KeyState   *k = &ex->ks[j];
	uint32	   *ids = palloc(Max(ng, 1) * sizeof(uint32)),
			   *old,
				nused;

	for (uint32 r = 0; r < ng; r++)
		ids[r] = group_key_id(ex, j, gl[r]);
	old = renumber(ids, ng, k->ident ? k->nident : k->ot.n, &nused);
	if (k->ident)
	{
		uint32		hd[2] = {nused, nused * sizeof(int32)};
		int32	   *vals = palloc(Max(nused, 1) * sizeof(int32));

		for (uint32 i = 0; i < nused; i++)
			vals[i] = k->idval[old[i] - 1];
		bw_put(b, hd, sizeof(hd));
		bw_put(b, vals, nused * sizeof(int32));
	}
	else
		bw_values(b, &k->ot, old, nused);
	bw_put(b, ids, (Size) ng * sizeof(uint32));
}

/* the (group, value) pairs of a count(DISTINCT): values of a dictionary leaf by value, the others as they are */
static void
blob_set(AggExec *ex, int ai, const uint32 *cidx, StringInfo b)
{
	DSet	   *ds = &ex->dset[ai];
	bool		dict = ex->lay->leaf[ex->spec->agg[ai].leaf].vtype == SCV_CODE;
	uint32		np = 0,
			   *pg = palloc(Max(ds->n, 1) * sizeof(uint32)),
			   *pv = palloc(Max(ds->n, 1) * sizeof(uint32)),
				hd[2],
			   *old = NULL,
				nused = 0;

	for (uint64 i = 0; i < ds->cap; i++)
		if (ds->slot[i])
		{
			pg[np] = cidx[(ds->slot[i] >> 32) & 0x7fffffff];
			pv[np++] = (uint32) ds->slot[i];
		}
	if (dict)
		old = renumber(pv, np, ex->aot[ai].n, &nused);
	hd[0] = np;
	hd[1] = 0;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, pg, (Size) np * sizeof(uint32));
	bw_put(b, pv, (Size) np * sizeof(uint32));
	if (dict)
		bw_values(b, &ex->aot[ai], old, nused);
}

static void
blob_dump(AggExec *ex, int lane, StringInfo b)
{
	SiftAggSpec *s = ex->spec;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				ng = 0,
			   *gl = palloc(Max(n, 1) * sizeof(uint32)),
			   *cidx = palloc(Max(n, 1) * sizeof(uint32));
	BlobHdr		h = {BLOB_MAGIC, lane, 0, 0};

	for (uint32 g = 0; g < n; g++)
	{
		cidx[g] = UINT32_MAX;
		if (ex->seen[g] > 0)
		{
			cidx[g] = ng;
			gl[ng++] = g;
		}
	}
	h.ngroups = ng;
	bw_put(b, &h, sizeof(h));
	for (int j = 0; j < s->nkeys; j++)
		blob_key(ex, j, gl, ng, b);
	{
		AggAcc		seen = {.n = ex->seen};

		bw_acc(b, &seen, gl, ng);
	}
	for (int i = 0; i < s->naggs; i++)
	{
		bw_acc(b, &ex->acc[i], gl, ng);
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			blob_set(ex, i, cidx, b);
	}
	bw_put(b, ex->amax, Max(s->naggs, 1) * sizeof(double));
	pfree(gl);
	pfree(cidx);
}

/* ids of the values of an integer key in the table (the ids of the range table, or of the wide one) */
static void
merge_ident(AggExec *g, int j, const int32 *vals, uint32 nv, uint32 *xid)
{
	KeyState   *k = &g->ks[j];
	int32		mn = PG_INT32_MAX,
				mx = PG_INT32_MIN;

	for (uint32 i = 0; i < nv; i++)
	{
		mn = Min(mn, vals[i]);
		mx = Max(mx, vals[i]);
	}
	if (nv == 0)
		return;
	if (k->wcap || rt_span(&k->rt, mn, mx) > MAX_RANGE)
	{
		if (!k->wcap)
			ident_go_wide(g, k);
		for (uint32 i = 0; i < nv; i++)
			xid[i + 1] = ident_wide_id(g, k, vals[i]);
	}
	else
	{
		KeyFill		f = {g, j};
		MemoryContext old = MemoryContextSwitchTo(g->tcxt);

		rt_cover(&k->rt, mn, mx, key_range_fill, &f);
		MemoryContextSwitchTo(old);
		for (uint32 i = 0; i < nv; i++)
			xid[i + 1] = ((uint32 *) k->rt.tab)[vals[i] - k->rt.lo];
	}
}

/* xid[1..] = the ids in table t of the values a blob lists; the reader is left after them */
static void
merge_values(AggExec *g, OutTable *t, const char **p, uint32 *xid)
{
	const uint32 *hd = br_get(p, 8);
	const char *at = *p;

	for (uint32 i = 0; i < hd[0]; i++)
	{
		bool		isnull;
		Datum		d = datumRestore((char **) &at, &isnull);
		MemoryContext old = MemoryContextSwitchTo(g->tcxt);

		xid[i + 1] = ot_id(t, d);
		MemoryContextSwitchTo(old);
	}
	*p += TYPEALIGN(8, hd[1]);
}

typedef struct BlobAgg
{
	AggAcc		a;				/* the arrays of the blob, read only */
	uint32		npairs;
	const uint32 *pg;
	const uint32 *pv;
	uint32	   *xv;				/* dictionary values: the ids in the table */
} BlobAgg;

static void
merge_minmax(AggExec *g, int ai, const BlobAgg *ba, uint32 r, uint32 t)
{
	SiftAggAgg *a = &g->spec->agg[ai];
	AggAcc	   *acc = &g->acc[ai];
	bool		ismin = a->kind == SAA_MIN,
				f64 = g->lay->leaf[a->leaf].vtype == SCV_F64;

	if (ba->a.n[r] == 0)
		return;
	if (acc->n[t] == 0)
	{
		acc->i[t] = ba->a.i[r];
		acc->d[t] = ba->a.d[r];
	}
	else if (f64 ? (ismin ? float8_cmp_internal(ba->a.d[r], acc->d[t]) < 0 : float8_cmp_internal(ba->a.d[r], acc->d[t]) > 0) :
			 (ismin ? ba->a.i[r] < acc->i[t] : ba->a.i[r] > acc->i[t]))
	{
		acc->i[t] = ba->a.i[r];
		acc->d[t] = ba->a.d[r];
	}
	acc->n[t] += ba->a.n[r];
}

static void
merge_agg(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids, uint32 r0, uint32 n)
{
	SiftAggAgg *a = &g->spec->agg[ai];
	AggAcc	   *acc = &g->acc[ai];

	for (uint32 r = r0; r < r0 + n; r++)
	{
		uint32		t = gids[r];

		switch (a->kind)
		{
			case SAA_COUNT_STAR:
			case SAA_COUNT:
			case SAA_COUNT_ROWS:
				acc->n[t] += ba->a.n[r];
				break;
			case SAA_SUM_INT:
			case SAA_AVG_INT:
				acc->n[t] += ba->a.n[r];
				acc->i[t] += ba->a.i[r];
				break;
			case SAA_SUM_F64:
			case SAA_AVG_F64:
				acc->n[t] += ba->a.n[r];
				acc->d[t] += ba->a.d[r];
				acc->nf[t] |= ba->a.nf[r];
				break;
			case SAA_MIN:
			case SAA_MAX:
				merge_minmax(g, ai, ba, r, t);
				break;
			case SAA_COUNT_DISTINCT:
				break;
			default:
				elog(ERROR, "sift: aggregate kind %d has no partial table", a->kind);
		}
	}
}

/* the arrays of a blob's aggregate (the layout of bw_acc), with the pairs of its set */
static void
merge_read_agg(AggExec *g, int ai, const char **p, uint32 ng, BlobAgg *ba)
{
	const AggAcc *acc = &g->acc[ai];

	ba->a.n = (int64 *) br_get(p, (Size) ng * 8);
	if (acc->i)
		ba->a.i = (int64 *) br_get(p, (Size) ng * 8);
	if (acc->d)
		ba->a.d = (double *) br_get(p, (Size) ng * 8);
	if (acc->nf)
		ba->a.nf = (uint8 *) br_get(p, ng);
	if (g->spec->agg[ai].kind == SAA_COUNT_DISTINCT)
	{
		const uint32 *hd = br_get(p, 8);

		ba->npairs = hd[0];
		ba->pg = br_get(p, (Size) ba->npairs * 4);
		ba->pv = br_get(p, (Size) ba->npairs * 4);
		if (g->lay->leaf[g->spec->agg[ai].leaf].vtype == SCV_CODE)
		{
			const uint32 *vh = (const uint32 *) *p;

			ba->xv = palloc0((vh[0] + 1) * sizeof(uint32));
			merge_values(g, &g->aot[ai], p, ba->xv);
		}
	}
}

static void
merge_set(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids)
{
	for (uint32 i = 0; i < ba->npairs; i++)
	{
		uint32		t = gids[ba->pg[i]];

		if (dset_add(g, &g->dset[ai], t, ba->xv ? ba->xv[ba->pv[i]] : ba->pv[i]))
			g->acc[ai].n[t]++;
	}
}

static void
blob_merge(AggExec *g, const char *p)
{
	SiftAggSpec *s = g->spec;
	const BlobHdr *h = br_get(&p, sizeof(BlobHdr));
	uint32		ng = h->ngroups,
			   *xid[SIFT_AGG_MAX_KEYS],
			   *gids = palloc(Max(ng, 1) * sizeof(uint32));
	const uint32 *lid[SIFT_AGG_MAX_KEYS];
	const int64 *seen;
	BlobAgg    *ba = palloc0(Max(s->naggs, 1) * sizeof(BlobAgg));
	bool		grow = false;

	if (h->magic != BLOB_MAGIC)
		elog(ERROR, "sift: bad partial aggregate");
	for (int j = 0; j < s->nkeys; j++)
	{
		KeyState   *k = &g->ks[j];
		const uint32 *hd = (const uint32 *) p;

		xid[j] = palloc0((hd[0] + 1) * sizeof(uint32));
		if (k->ident)
		{
			br_get(&p, 8);
			merge_ident(g, j, br_get(&p, (Size) hd[0] * sizeof(int32)), hd[0], xid[j]);
		}
		else
			merge_values(g, &k->ot, &p, xid[j]);
		lid[j] = br_get(&p, (Size) ng * sizeof(uint32));
	}
	seen = br_get(&p, (Size) ng * sizeof(int64));
	for (int i = 0; i < s->naggs; i++)
		merge_read_agg(g, i, &p, ng, &ba[i]);
	if (s->naggs)
		for (int i = 0; i < s->naggs; i++)
			g->amax[i] = Max(g->amax[i], ((const double *) p)[i]);
	if (!g->hash)
		for (int j = 0; j < s->nkeys; j++)
			grow |= key_used(&g->ks[j]) > g->ks[j].cap;
	if (grow)
		direct_grow(g);
	for (uint32 r0 = 0; r0 < ng; r0 += VEC)
	{
		uint32		n = Min(VEC, ng - r0);

		for (uint32 r = r0; r < r0 + n; r++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			for (int j = 0; j < s->nkeys; j++)
				ids[j] = xid[j][lid[j][r]];
			if (g->hash)
				gids[r] = hash_group(g, ids);
			else
			{
				gids[r] = 0;
				for (int j = 0; j < s->nkeys; j++)
					gids[r] += ids[j] * g->ks[j].mult;
			}
		}
		if (g->ngroups > g->gcap)
			groups_resize(g, Max(g->gcap * 2, g->ngroups), NULL, g->gcap);
		for (uint32 r = r0; r < r0 + n; r++)
			g->seen[gids[r]] += seen[r];
		for (int i = 0; i < s->naggs; i++)
			merge_agg(g, i, &ba[i], gids, r0, n);
	}
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			merge_set(g, i, &ba[i], gids);
	pfree(gids);
	pfree(ba);
}

/* ---------- the scan: items of work, lanes ---------- */

/*
 * The superblocks are split in items that the participants take from a counter. Float sums are order dependent, so that a result
 * does not depend on the number of workers: such a scan has SIFT_AGG_LANES items, lane l being the superblocks l, l + SIFT_AGG_LANES, ..., and the
 * tables of the lanes are combined in lane order, whoever scanned them. The other aggregates are exact in any order: items are
 * MORSEL_SBS superblocks, and a participant keeps one table for all its items.
 */
#define MORSEL_SBS	2

typedef struct ScanShared
{
	pg_atomic_uint32 next;		/* the next item to take */
	uint32		nitems;
	uint32		nsb;
	uint32		morsel;			/* superblocks of an item (the scans that keep their state in the leader: all of them) */
	bool		ordered;
} ScanShared;

static void
shared_init(ScanShared *sh, AggExec *ex)
{
	sh->nsb = sift_colsrc_nsb(ex->src);
	sh->ordered = sift_aggspec_ordered(ex->spec);
	sh->morsel = sift_aggspec_parallel_ok(ex->spec) ? MORSEL_SBS : Max(sh->nsb, 1);
	sh->nitems = sh->ordered ? SIFT_AGG_LANES : (sh->nsb + sh->morsel - 1) / sh->morsel;
	pg_atomic_init_u32(&sh->next, 0);
}

static void
scan_range(AggExec *ex, uint32 lo, uint32 hi)
{
	sift_colsrc_begin(ex->src, lo, hi, ex->prune, ex->nprune);
	while (sift_colsrc_next(ex->src, &ex->unit))
		process_unit(ex);
}

/* a grouping set without columns has its group even when no row reached it, as the plain aggregate has its row */
static void
empty_set_groups(AggExec *ex)
{
	for (int set = 0; set < ex->spec->nsets; set++)
		if (ex->spec->setmask[set] == (uint32) 1 << (ex->spec->nkeys - 1))
		{
			ex->curset = set;
			ex->arm_mask = ex->spec->setmask[set];
			compute_gids(ex, 1);
			ex->seen[ex->gid[0]] = Max(ex->seen[ex->gid[0]], 1);
		}
	ex->arm_mask = ~(uint32) 0;
}

static void
scan_item(AggExec *ex, uint32 item)
{
	ScanShared *sh = ex->shared;

	if (sh->ordered)
		for (uint32 sb = item; sb < sh->nsb; sb += SIFT_AGG_LANES)
			scan_range(ex, sb, sb + 1);
	else
		scan_range(ex, item * sh->morsel, Min((item + 1) * sh->morsel, sh->nsb));
}

static bool
take_item(AggExec *ex, uint32 *item)
{
	*item = pg_atomic_fetch_add_u32(&ex->shared->next, 1);
	return *item < ex->shared->nitems;
}

/* the lanes that are next in order go into the result */
static void
lanes_combine(AggExec *ex)
{
	while (ex->next_lane < SIFT_AGG_LANES && ex->pending[ex->next_lane])
	{
		blob_merge(ex->gtab, ex->pending[ex->next_lane]);
		pfree(ex->pending[ex->next_lane]);
		ex->pending[ex->next_lane++] = NULL;
	}
}

static void
lanes_put(AggExec *ex, int lane, const char *data, Size len)
{
	ex->pending[lane] = MemoryContextAlloc(ex->cxt, len);
	memcpy(ex->pending[lane], data, len);
	lanes_combine(ex);
}

static AggExec *
result_view(AggExec *ex)
{
	AggExec    *g = MemoryContextAlloc(ex->cxt, sizeof(AggExec));

	*g = *ex;
	g->lanes = false;
	g->tcxt = AllocSetContextCreate(ex->cxt, "sift agg result", ALLOCSET_DEFAULT_SIZES);
	g->ks = MemoryContextAlloc(ex->cxt, Max(ex->spec->nkeys, 1) * sizeof(KeyState));
	memcpy(g->ks, ex->ks, Max(ex->spec->nkeys, 1) * sizeof(KeyState));
	table_init(g);
	return g;
}

/* the check that avg(float8) owes (float_guard): the inputs of the whole scan could have overflowed the sum of squares */
static void
final_checks(AggExec *g, int64 ntuples)
{
	for (int i = 0; i < g->spec->naggs; i++)
		if (g->spec->agg[i].kind == SAA_AVG_F64 && 8.0 * ntuples * g->amax[i] * g->amax[i] >= DBL_MAX)
			float_overflow_error();
}

/* ---------- workers ---------- */

/*
 * A scan whose plan asks for workers runs them under a parallel context of its own (not under a Gather: the partial tables are
 * not tuples). The leader and the workers take items from the counter in the shared memory; a worker sends its tables to the
 * leader through a shm_mq, as blobs, and the leader merges them. Lanes wait for the lanes before them.
 */
#define KEY_SHARED	UINT64CONST(0x53494654A001)
#define KEY_SPEC	UINT64CONST(0x53494654A002)
#define KEY_PARAMS	UINT64CONST(0x53494654A003)
#define KEY_SNAP	UINT64CONST(0x53494654A004)
#define KEY_MQ		UINT64CONST(0x53494654A005)
#define MQ_SIZE		(1 << 20)
#define DONE_MAGIC	0x53414732

typedef struct WorkerDone
{
	uint32		magic;
	uint32		pad;
	int64		n_units;
	int64		n_rows;
	int64		n_tuples;
	SiftSrcCounters cc;
} WorkerDone;

typedef struct AggPar
{
	ParallelContext *pcxt;
	int			nlaunched;
	int			ndone;
	shm_mq_handle **mqh;
	bool	   *done;
	SiftSrcCounters cc;			/* of the workers, added */
} AggPar;

static void
send_blob(shm_mq_handle *mqh, const void *data, Size len)
{
	if (shm_mq_send(mqh, len, data, false, true) != SHM_MQ_SUCCESS)
		ereport(ERROR, (errmsg("sift: the leader of a parallel aggregate went away")));
}

/* the table of the lane just scanned, as a blob: kept in order here, sent by a worker */
static void
lane_done(AggExec *ex, uint32 lane)
{
	StringInfoData b;

	initStringInfo(&b);
	blob_dump(ex, lane, &b);
	if (ex->mqh)
		send_blob(ex->mqh, b.data, b.len);
	else
		lanes_put(ex, lane, b.data, b.len);
	pfree(b.data);
	table_reset(ex);
}

static void par_poll(AggExec *ex);
static void engine_contexts(AggExec *ex, MemoryContext parent);
static void engine_rels(AggExec *ex);
static void engine_open(AggExec *ex, Snapshot snapshot, ParamListInfo params);
static void engine_close(AggExec *ex);

static void
scan_items(AggExec *ex)
{
	uint32		item;

	while (take_item(ex, &item))
	{
		scan_item(ex, item);
		if (ex->shared->ordered)
			lane_done(ex, item);
		if (ex->par)
			par_poll(ex);
	}
}

void
sift_agg_worker_main(dsm_segment *seg, shm_toc *toc)
{
	ScanShared *sh = shm_toc_lookup(toc, KEY_SHARED, false);
	char	   *params = shm_toc_lookup(toc, KEY_PARAMS, false);
	shm_mq	   *mq = (shm_mq *) ((char *) shm_toc_lookup(toc, KEY_MQ, false) + (Size) ParallelWorkerNumber * MQ_SIZE);
	AggExec    *ex = palloc0(sizeof(AggExec));
	WorkerDone	done = {DONE_MAGIC};

	shm_mq_set_sender(mq, MyProc);
	ex->mqh = shm_mq_attach(mq, seg, NULL);
	ex->spec = sift_aggspec_unpack((List *) stringToNode((char *) shm_toc_lookup(toc, KEY_SPEC, false)));
	engine_contexts(ex, CurrentMemoryContext);
	engine_rels(ex);
	engine_open(ex, RestoreSnapshot(shm_toc_lookup(toc, KEY_SNAP, false)), RestoreParamList(&params));
	ex->shared = sh;
	ex->lanes = sh->ordered;
	scan_buffers(ex);
	table_init(ex);
	scan_items(ex);
	if (!sh->ordered)
	{
		StringInfoData b;

		initStringInfo(&b);
		blob_dump(ex, -1, &b);
		send_blob(ex->mqh, b.data, b.len);
	}
	done.n_units = ex->n_units;
	done.n_rows = ex->n_rows;
	done.n_tuples = ex->n_tuples;
	sift_colsrc_counters(ex->src, &done.cc);
	send_blob(ex->mqh, &done, sizeof(done));
	shm_mq_detach(ex->mqh);
	engine_close(ex);
}

/* Starts the workers of the plan; the leader scans alone when none could be launched */
static void
par_launch(AggExec *ex)
{
	ParallelContext *pcxt;
	char	   *spec_txt = nodeToString(ex->pack),
			   *p;
	Size		plen = EstimateParamListSpace(ex->params),
				slen = EstimateSnapshotSpace(ex->snap);
	int			nw = ex->spec->nworkers;
	ScanShared  *ps;
	AggPar	   *par;

	if (nw <= 0 || IsParallelWorker())
		return;
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_sift", "sift_agg_worker_main", nw);
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(ScanShared));
	shm_toc_estimate_chunk(&pcxt->estimator, strlen(spec_txt) + 1);
	shm_toc_estimate_chunk(&pcxt->estimator, plen);
	shm_toc_estimate_chunk(&pcxt->estimator, slen);
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(MQ_SIZE, nw));
	shm_toc_estimate_keys(&pcxt->estimator, 5);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	ps = shm_toc_allocate(pcxt->toc, sizeof(ScanShared));
	shared_init(ps, ex);
	shm_toc_insert(pcxt->toc, KEY_SHARED, ps);
	p = shm_toc_allocate(pcxt->toc, strlen(spec_txt) + 1);
	strcpy(p, spec_txt);
	shm_toc_insert(pcxt->toc, KEY_SPEC, p);
	p = shm_toc_allocate(pcxt->toc, plen);
	shm_toc_insert(pcxt->toc, KEY_PARAMS, p);
	SerializeParamList(ex->params, &p);
	p = shm_toc_allocate(pcxt->toc, slen);
	SerializeSnapshot(ex->snap, p);
	shm_toc_insert(pcxt->toc, KEY_SNAP, p);
	p = shm_toc_allocate(pcxt->toc, mul_size(MQ_SIZE, nw));
	for (int i = 0; i < nw; i++)
		shm_mq_set_receiver(shm_mq_create(p + (Size) i * MQ_SIZE, MQ_SIZE), MyProc);
	shm_toc_insert(pcxt->toc, KEY_MQ, p);
	LaunchParallelWorkers(pcxt);
	if (pcxt->nworkers_launched == 0)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	par = MemoryContextAllocZero(ex->cxt, sizeof(AggPar));
	par->pcxt = pcxt;
	par->nlaunched = pcxt->nworkers_launched;
	par->mqh = MemoryContextAllocZero(ex->cxt, par->nlaunched * sizeof(shm_mq_handle *));
	par->done = MemoryContextAllocZero(ex->cxt, par->nlaunched * sizeof(bool));
	for (int i = 0; i < par->nlaunched; i++)
	{
		par->mqh[i] = shm_mq_attach((shm_mq *) (p + (Size) i * MQ_SIZE), pcxt->seg, NULL);
		shm_mq_set_handle(par->mqh[i], pcxt->worker[i].bgwhandle);
	}
	ex->par = par;
	ex->shared = ps;
}

static void
par_message(AggExec *ex, int w, const char *data, Size len)
{
	AggPar	   *par = ex->par;

	if (*(const uint32 *) data == DONE_MAGIC)
	{
		const WorkerDone *d = (const WorkerDone *) data;

		ex->n_units += d->n_units;
		ex->n_rows += d->n_rows;
		ex->n_tuples += d->n_tuples;
		par->cc.units_pruned += d->cc.units_pruned;
		par->cc.rows_vetted += d->cc.rows_vetted;
		par->cc.heap_pages += d->cc.heap_pages;
		par->done[w] = true;
		par->ndone++;
		shm_mq_detach(par->mqh[w]);
	}
	else if (ex->shared->ordered)
	{
		const BlobHdr *h = (const BlobHdr *) data;

		if (h->magic != BLOB_MAGIC)
			elog(ERROR, "sift: bad message of a parallel aggregate");
		lanes_put(ex, h->lane, data, len);
	}
	else
		blob_merge(ex->gtab, data);
}

/* the messages that are there; whether there were any */
static bool
par_receive(AggExec *ex)
{
	AggPar	   *par = ex->par;
	bool		any = false;

	for (int w = 0; w < par->nlaunched; w++)
		while (!par->done[w])
		{
			Size		len;
			void	   *data;
			shm_mq_result r = shm_mq_receive(par->mqh[w], &len, &data, true);

			if (r == SHM_MQ_WOULD_BLOCK)
				break;
			if (r == SHM_MQ_DETACHED)
				ereport(ERROR, (errmsg("sift: a worker of a parallel aggregate stopped without its result")));
			par_message(ex, w, data, len);
			any = true;
		}
	return any;
}

static void
par_poll(AggExec *ex)
{
	par_receive(ex);
	CHECK_FOR_INTERRUPTS();
}

/* waits for the workers' tables, then for the workers */
static void
par_finish(AggExec *ex)
{
	AggPar	   *par = ex->par;

	while (par->ndone < par->nlaunched)
	{
		if (!par_receive(ex))
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}
		CHECK_FOR_INTERRUPTS();
	}
	WaitForParallelWorkersToFinish(par->pcxt);
	DestroyParallelContext(par->pcxt);
	par->pcxt = NULL;
	ExitParallelMode();
}

static void
run_scan(AggExec *ex)
{
	bool		ordered = sift_aggspec_ordered(ex->spec);

	ex->started = true;
	ex->glist = NULL;
	engine_open(ex, ex->snap, ex->params);
	scan_buffers(ex);
	par_launch(ex);
	if (ex->shared == NULL)
	{
		ex->shared = MemoryContextAlloc(ex->cxt, sizeof(ScanShared));
		shared_init(ex->shared, ex);
	}
	ex->lanes = ordered;
	table_init(ex);
	ex->gtab = ordered ? result_view(ex) : ex;
	ex->pending = MemoryContextAllocZero(ex->cxt, SIFT_AGG_LANES * sizeof(char *));
	if (ex->par == NULL || parallel_leader_participation)
		scan_items(ex);
	arms_flush(ex);
	osa_finish_all(ex);
	if (ex->par)
		par_finish(ex);
	if (ordered && ex->next_lane != SIFT_AGG_LANES)
		elog(ERROR, "sift: a lane of the aggregate scan is missing");
	final_checks(ex->gtab, ex->n_tuples);
}

/* the groups that exist, in index order */
static void
list_groups(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				k = 0;

	ex->glist = MemoryContextAlloc(ex->cxt, Max(n, 1) * sizeof(uint32));
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0)
			ex->glist[k++] = g;
	if (ex->spec->nkeys == 0 && k == 0)
		ex->glist[k++] = 0;
	ex->nglist = k;
	ex->n_groups = k;
}

static Datum scaled_numeric(__int128 v, int scale);

static Datum
key_datum(AggExec *ex, int j, uint32 g, bool *isnull)
{
	KeyState   *k = &ex->ks[j];
	uint32		id = key_id(ex, j, g);

	*isnull = id == 0;
	if (id == 0)
		return (Datum) 0;
	if (k->numk)
		return ex->spec->key[j].typid == NUMERICOID ? scaled_numeric(k->im.val[id - 1], ex->spec->key[j].num.scale) :
			leaf_datum(ex->spec->key[j].typid, (int64) k->im.val[id - 1]);
	if (k->ident)
		return leaf_datum(ex->spec->key[j].typid, k->idval[id - 1]);
	return k->ot.val[id - 1];
}

/* v / 10^scale as a numeric of that scale: the text carries all the digits, so 1.50 keeps its display scale */
static Datum
scaled_numeric(__int128 v, int scale)
{
	char		tmp[48],
				buf[64];
	char	   *o = buf;
	int			n = 0;
	unsigned __int128 u = v < 0 ? -(unsigned __int128) v : (unsigned __int128) v;

	do
	{
		tmp[n++] = '0' + (int) (u % 10);
		u /= 10;
	} while (u > 0 || n <= scale);
	if (v < 0)
		*o++ = '-';
	for (int i = n - 1; i >= 0; i--)
	{
		*o++ = tmp[i];
		if (i == scale && scale > 0)
			*o++ = '.';
	}
	*o = '\0';
	return DirectFunctionCall3(numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static Datum
agg_datum(AggExec *ex, int ai, uint32 g, bool *isnull)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];

	*isnull = false;
	switch (a->kind)
	{
			case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_COUNT_ROWS:
		case SAA_COUNT_DISTINCT:
		case SAA_O_COUNT_STAR:
		case SAA_O_COUNT_ROWS:
		case SAA_M_COUNT:
		case SAA_M_DISTINCT:
			return Int64GetDatum(acc->n[g]);
		case SAA_GROUPING:
			return Int32GetDatum(grouping_bits(ex, a, g));
		case SAA_M_MIN:
		case SAA_M_MAX:
			*isnull = acc->n[g] == 0;
			return acc->v[g];
		case SAA_M_SUM:
		case SAA_M_AVG:
			return mapped_sum_datum(ex, a, ai, g, isnull);
		case SAA_O_SUM_I64:
			*isnull = acc->n[g] == 0;
			return DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->i[g]));
		case SAA_O_SUM_F64:
			*isnull = acc->n[g] == 0;
			if (isinf(acc->d[g]) && !acc->nf[g])
				float_overflow_error();
			return Float8GetDatum(acc->d[g]);
		case SAA_O_AVG_F64:
			*isnull = acc->n[g] == 0;
			return Float8GetDatum(acc->d[g] / (double) acc->n[g]);
		case SAA_SUM_INT:
			*isnull = acc->n[g] == 0;
			return Int64GetDatum(acc->i[g]);
		case SAA_SUM_F64:
			*isnull = acc->n[g] == 0;
			if (isinf(acc->d[g]) && !acc->nf[g])
				float_overflow_error();
			return Float8GetDatum(acc->d[g]);
		case SAA_AVG_INT:
			if (acc->n[g] == 0)
			{
				*isnull = true;
				return (Datum) 0;
			}
			return DirectFunctionCall2(numeric_div, DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->i[g])),
									   DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
		case SAA_AVG_F64:
			*isnull = acc->n[g] == 0;
			return Float8GetDatum(acc->d[g] / (double) acc->n[g]);
		case SAA_SUM_NUM:
			*isnull = acc->n[g] == 0;
			return *isnull ? (Datum) 0 : scaled_numeric(acc->w[g], a->num.scale);
		case SAA_AVG_NUM:
			*isnull = acc->n[g] == 0;
			if (*isnull)
				return (Datum) 0;
			return DirectFunctionCall2(numeric_div, scaled_numeric(acc->w[g], a->num.scale),
									   DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
			{
				double		v;

				*isnull = acc->n[g] == 0;
				if (*isnull)
					return (Datum) 0;
				v = ex->oval[ai][ex->acc[a->share].i[g] - 1];
				if (a->restype == FLOAT4OID)
					return Float4GetDatum((float4) v);
				return a->restype == FLOAT8OID ? Float8GetDatum(v) : leaf_datum(a->restype, (int64) v);
			}
		case SAA_MIN:
		case SAA_MAX:
			*isnull = acc->n[g] == 0;
			if (!a->num.n && ex->lay->leaf[a->leaf].vtype == SCV_F64)
				return a->restype == FLOAT4OID ? Float4GetDatum((float4) acc->d[g]) : Float8GetDatum(acc->d[g]);
			if (a->num.n)
				return scaled_numeric(acc->w[g], a->num.scale);
			return leaf_datum(a->restype, acc->i[g]);
	}
	return (Datum) 0;
}

static TupleTableSlot *
agg_next(ScanState *node)
{
	AggExec    *ex = (AggExec *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	AggExec    *t;
	uint32		g;

	if (!ex->started)
	{
		run_scan(ex);
		empty_set_groups(ex->gtab);
		mapped_finish(ex->gtab);
		list_groups(ex->gtab);
		ex->n_groups = ex->gtab->nglist;
	}
	t = ex->gtab;
	if (ex->gpos >= t->nglist)
		return NULL;
	g = t->glist[ex->gpos++];
	ExecClearTuple(slot);
	for (int j = 0; j < t->spec->nkeys; j++)
		slot->tts_values[j] = key_datum(t, j, g, &slot->tts_isnull[j]);
	for (int i = 0; i < t->spec->naggs; i++)
	{
		int			c = t->spec->nkeys + i;

		slot->tts_values[c] = agg_datum(t, i, g, &slot->tts_isnull[c]);
	}
	return ExecStoreVirtualTuple(slot);
}

static bool
agg_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

/* ---------- executor methods ---------- */

static void
engine_contexts(AggExec *ex, MemoryContext parent)
{
	ex->cxt = AllocSetContextCreate(parent, "sift agg", ALLOCSET_DEFAULT_SIZES);
	ex->tcxt = AllocSetContextCreate(ex->cxt, "sift agg table", ALLOCSET_DEFAULT_SIZES);
	ex->ucxt = AllocSetContextCreate(ex->cxt, "sift agg unit", ALLOCSET_DEFAULT_SIZES);
}

static void
engine_rels(AggExec *ex)
{
	if (OidIsValid(ex->spec->indexoid))
		ex->index = index_open(ex->spec->indexoid, AccessShareLock);
	ex->heap = table_open(ex->spec->heapoid, AccessShareLock);
	ex->heap_opened = true;
}

/* The scan engine on the relations of the spec: shared by the node and by a parallel worker */
static void
engine_open(AggExec *ex, Snapshot snapshot, ParamListInfo params)
{
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->src = sift_colsrc_open(ex->heap, ex->index, snapshot, ex->cxt);
	ex->lay = sift_colsrc_source_layout(ex->src);
	extend_layout(ex);
	ex->uvec = palloc0(ex->lay->nleaves * sizeof(SiftSrcVec));
	ex->uvec_ok = palloc0(ex->lay->nleaves * sizeof(bool));
	ex->gofs = palloc0(ex->lay->nleaves * sizeof(uint32 *));
	ex->lutcx = CreateStandaloneExprContext();
	ex->lutcx->ecxt_param_list_info = params;
	ex->lutcx->ecxt_param_exec_vals = ex->lutparams;
	init_states(ex);
	for (int i = 0; i < ex->spec->npred; i++)
		register_pred(ex, ex->spec->pred[i]);
	for (int i = 0; i < ex->spec->naggs; i++)
		register_pred(ex, ex->spec->agg[i].filter);
	for (int j = 0; j < ex->spec->nkeys; j++)
		register_key(ex, j);
	register_combos(ex);
	prune_init(ex);
	MemoryContextSwitchTo(old);
}

static void
engine_close(AggExec *ex)
{
	if (ex->src)
		sift_colsrc_close(ex->src);
	if (ex->lutcx)
		FreeExprContext(ex->lutcx, true);
	if (ex->index)
		index_close(ex->index, NoLock);
	if (ex->heap_opened)
		table_close(ex->heap, NoLock);
}

static void
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	AggExec    *ex = (AggExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	ex->spec = sift_aggspec_unpack(cs->custom_private);
	ex->pack = cs->custom_private;
	ex->params = estate->es_param_list_info;
	ex->snap = estate->es_snapshot;
	engine_contexts(ex, estate->es_query_cxt);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	engine_rels(ex);
	PredicateLockRelation(ex->heap, estate->es_snapshot);
}

static TupleTableSlot *
agg_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, agg_next, agg_recheck);
}

static void
agg_end(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	for (int i = 0; ex->osa && i < ex->spec->naggs; i++)
		if (ex->osa[i] && ex->spec->agg[i].share == i && ex->osa[i]->sort)
			tuplesort_end(ex->osa[i]->sort);
	engine_close(ex);
	if (ex->cxt)
		MemoryContextDelete(ex->cxt);
}

static void
agg_rescan(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	/* the plan has no parameter the scan reads (unsupported_clause), so the groups are the same: replay them */
	ex->gpos = 0;
}

static void
agg_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	AggExec    *ex = (AggExec *) node;
	SiftAggSpec *s = ex->spec;
	StringInfoData buf;

	ExplainPropertyText("Index", s->indexoid ? get_rel_name(s->indexoid) : "(table)", es);
	if (s->desc_quals && s->desc_quals[0])
		ExplainPropertyText("Filters", s->desc_quals, es);
	initStringInfo(&buf);
	for (int j = 0; j < s->nkeys; j++)
		appendStringInfo(&buf, "%s%s", j ? ", " : "", s->key[j].desc);
	if (s->nkeys)
		ExplainPropertyText("Group Keys", buf.data, es);
	resetStringInfo(&buf);
	for (int i = 0; i < s->naggs; i++)
		appendStringInfo(&buf, "%s%s", i ? ", " : "", s->agg[i].desc);
	if (s->naggs)
		ExplainPropertyText("Pushed Aggregates", buf.data, es);
	if (s->group >= 0)
		ExplainPropertyText("Unnests", "yes", es);
	if (s->njoin)
	{
		resetStringInfo(&buf);
		for (int i = 0; i < s->njoin; i++)
			appendStringInfo(&buf, "%s%s", i ? ", " : "", s->join[i].desc);
		ExplainPropertyText("Lookup Joins", buf.data, es);
	}
	if (s->nworkers > 0)
		ExplainPropertyInteger("Workers Planned", NULL, s->nworkers, es);
	if (es->analyze && ex->started)
	{
		SiftSrcCounters cc;

		sift_colsrc_counters(ex->src, &cc);
		ExplainPropertyText("SIMD", sift_simd_level_name(sift_cpu.active), es);
		if (s->nworkers > 0)
			ExplainPropertyInteger("Workers Launched", NULL, ex->par ? ex->par->nlaunched : 0, es);
		if (ex->par)
		{
			cc.units_pruned += ex->par->cc.units_pruned;
			cc.rows_vetted += ex->par->cc.rows_vetted;
			cc.heap_pages += ex->par->cc.heap_pages;
		}
		ExplainPropertyInteger("Units", NULL, ex->n_units, es);
		ExplainPropertyInteger("Units Pruned", NULL, cc.units_pruned, es);
		ExplainPropertyInteger("Rows Scanned", NULL, ex->n_rows, es);
		ExplainPropertyInteger("Tuples Aggregated", NULL, ex->n_tuples, es);
		ExplainPropertyInteger("Rows Vetted By Heap", NULL, cc.rows_vetted, es);
		ExplainPropertyInteger("Heap Pages", NULL, cc.heap_pages, es);
		ExplainPropertyInteger("Groups", NULL, ex->n_groups, es);
		if (ex->n_osa_spilled)
			ExplainPropertyInteger("Ordered Sets Spilled", NULL, ex->n_osa_spilled, es);
	}
}

static const CustomExecMethods agg_exec_methods = {
	.CustomName = "SiftAgg",
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

Node *
sift_agg_create_state(CustomScan *cscan)
{
	AggExec    *ex = (AggExec *) newNode(sizeof(AggExec), T_CustomScanState);

	ex->css.methods = &agg_exec_methods;
	ex->css.flags = cscan->flags;
	return (Node *) ex;
}
