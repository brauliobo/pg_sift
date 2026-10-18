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

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "storage/predicate.h"
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
	void	   *tab;			/* esize bytes per value */
	int			esize;
} RangeTab;

static void
rt_init(RangeTab *r, int esize)
{
	r->lo = 1;
	r->hi = 0;
	r->tab = NULL;
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
	char	   *nt;

	if (had && lo >= old_lo && hi <= old_hi)
		return;
	if (span > MAX_RANGE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: a column range of %lld values is too wide for the aggregate scan", (long long) span),
				 errhint("Set sift.enable_agg to off for this query.")));
	nt = MemoryContextAllocZero(CurrentMemoryContext, span * r->esize + SIFT_AGG_PAD);
	if (had)
	{
		memcpy(nt + (int64) (old_lo - nlo) * r->esize, r->tab, ((int64) old_hi - old_lo + 1) * r->esize);
		pfree(r->tab);
	}
	r->tab = nt;
	r->lo = nlo;
	r->hi = nhi;
	for (int64 v = nlo; v <= nhi; v++)
		if (!had || v < old_lo || v > old_hi)
			fill(arg, (int32) v, nt + (v - nlo) * r->esize);
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
} PredState;

typedef struct KeyState
{
	OutTable	ot;				/* distinct output values (ids 1..) */
	ExprState  *expr;
	Param	   *param;
	RangeTab	rt;				/* integer leaves: value -> id */
	bool		ident;			/* integer leaf as the key: ids are given to the values in order of appearance */
	int32	   *idval;			/* ident: the value of id i + 1 */
	uint32		nident;
	uint32		capident;
	uint32		wcap;			/* ident, values too far apart for a table: open addressing value -> id (0 = free), power of 2 */
	int32	   *wval;
	uint32	   *wid;
	uint32		nullid;			/* LUT keys on integer leaves: the id of expr(NULL) */
	uint32		cap;			/* direct mode: ids 0..cap-1 */
	uint32		mult;
} KeyState;

/* open addressing set of (group, value id) pairs; a slot holds the pair with bit 63 set, 0 is empty */
typedef struct DSet
{
	uint64	   *slot;
	uint64		n;
	uint64		cap;
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
	__int128   *w;				/* numeric sums: the exact sum of the scaled values */
	double	   *d;
	uint64	   *last;			/* count distinct of the rows: the last row counted */
	uint8	   *nf;				/* float sums: a non-finite input was added */
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
	MemoryContext ucxt;			/* one unit */
	ExprContext *lutcx;
	ParamExecData lutparams[SIFT_AGG_MAX_PARAMS];
	ComboSt    *cs;
	int			clbase[SIFT_AGG_MAX_COMBOS];	/* per combo: its first dictionary map slot (after the keys and aggregates) */
	int			ncl;
	int			nj;				/* join combos, in the order the stages apply them */
	int			jcombo[SIFT_AGG_MAX_JOINS];
	MemoryContext tcxt;			/* temporaries of one combo entry */
	ArmSt	   *arm;
	uint32		arm_mask;		/* the keys the tuples are grouped by: all, or an arm's */
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
	Osa		  **osa;			/* ordered-set aggregates: the values of the aggregate (shared by those on one argument) */
	double	  **oval;			/* ... and their results by sorted-values id */
	int			n_osa_spilled;
	/* output */
	uint32	   *glist;
	uint32		nglist;
	uint32		gpos;
	/* the unit */
	SiftSrcUnit unit;
	SiftSrcVec *uvec;
	bool	   *uvec_ok;
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

/* ---------- value conversions ---------- */

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
		MemoryContextSwitchTo(ex->cxt);
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
	map = MemoryContextAlloc(ex->cxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
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
	map = MemoryContextAlloc(ex->cxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	old = MemoryContextSwitchTo(ex->cxt);
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

/* ---------- predicates ---------- */

static int64
operand_int(Oid t, Datum d)
{
	switch (t)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return DatumGetInt64(d);
		case DATEOID:
			return DatumGetDateADT(d);
		case BOOLOID:
			return DatumGetBool(d);
		default:
			elog(ERROR, "sift: unexpected operand type %u", t);
	}
	return 0;
}

static int
int32_cmp_asc(const void *a, const void *b)
{
	return pg_cmp_s32(*(const int32 *) a, *(const int32 *) b);
}

static void
eval_operand(AggExec *ex, SiftAggPred *p, PredState *st)
{
	bool		isnull;
	Datum		d;
	ExprState  *e;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	e = ExecInitExpr((Expr *) p->expr, &ex->css.ss.ps);
	d = ExecEvalExprSwitchContext(e, ex->css.ss.ps.ps_ExprContext, &isnull);
	st->cnull = isnull;
	if (p->kind == SAP_CMP && !isnull)
	{
		if (p->argtype == FLOAT4OID)
			st->cd = DatumGetFloat4(d);
		else if (p->argtype == FLOAT8OID)
			st->cd = DatumGetFloat8(d);
		else
			st->ci = operand_int(p->argtype, d);
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
			v = operand_int(p->argtype, el[i]);
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
			sift_aggk->cmp_i64(out, (const int64 *) v->data + start, n, op, st->ci);
			break;
		case SCV_F64:
			sift_aggk->cmp_f64(out, (const double *) v->data + start, n, op, st->cd);
			break;
		default:
			elog(ERROR, "sift: comparison on a dictionary leaf");
	}
	clear_nulls(ex, v, start, n, out);
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
			m->kint[kb + k] = operand_int(j->keytype[k], kd[k]);
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

/* evaluates the slots of a new entry for every output row */
static void
combo_make(AggExec *ex, int ci, const uint64 *v, ComboEnt *ent)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = cb->join >= 0 ? &ex->spec->join[cb->join] : NULL;
	ComboSt    *cs = &ex->cs[ci];
	MapTab	   *m = cs->map;
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
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
				r = ot_id(&ex->ks[cb->slot[sl].key].ot, d);
				MemoryContextSwitchTo(ex->tcxt);
			}
			cs->val[(size_t) cs->nout * cb->nslots + sl] = r;
		}
		cs->nout++;
		ResetExprContext(ex->lutcx);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->tcxt);
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

/* the bitmap of a row level predicate over the unit's rows, computed once */
static uint64 *
row_bitmap(AggExec *ex, SiftAggPred *p)
{
	PredState  *st = &ex->ps[p->id];

	if (!st->rowmask_ok)
	{
		int			nrow = ex->unit.nrow;

		st->rowmask = MemoryContextAlloc(ex->ucxt, ((nrow + 63) / 64 + PADW) * sizeof(uint64));
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
	if (!st->ready && (p->kind == SAP_CMP || p->kind == SAP_IN))
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

static void
dset_insert_raw(DSet *s, uint64 pair)
{
	uint64		h = dset_hash(pair) & (s->cap - 1);

	while (s->slot[h])
		h = (h + 1) & (s->cap - 1);
	s->slot[h] = pair;
	s->n++;
}

/* twice the slots, the groups renumbered through remap (NULL: as they are) */
static void
dset_grow(AggExec *ex, DSet *s, const uint32 *remap)
{
	uint64	   *old = s->slot;
	uint64		ocap = s->cap;

	s->cap = Max(ocap * 2, 1024);
	s->slot = MemoryContextAllocZero(ex->cxt, s->cap * sizeof(uint64));
	s->n = 0;
	for (uint64 i = 0; i < ocap; i++)
		if (old[i])
			dset_insert_raw(s, remap ? DSET_PAIR(remap[(old[i] >> 32) & 0x7fffffff], old[i]) : old[i]);
	if (old)
		pfree(old);
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
	while (s->slot[h])
	{
		if (s->slot[h] == pair)
			return false;
		h = (h + 1) & (s->cap - 1);
	}
	s->slot[h] = pair;
	s->n++;
	return true;
}

/* ---------- groups ---------- */

static AggAcc *
acc_alloc(AggExec *ex, uint32 cap)
{
	AggAcc	   *a = MemoryContextAllocZero(ex->cxt, Max(ex->spec->naggs, 1) * sizeof(AggAcc));

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		a[i].n = MemoryContextAllocZero(ex->cxt, cap * sizeof(int64));
		switch (ex->spec->agg[i].kind)
		{
			case SAA_SUM_INT:
			case SAA_AVG_INT:
			case SAA_PCT_CONT:
			case SAA_PCT_DISC:
				a[i].i = MemoryContextAllocZero(ex->cxt, cap * sizeof(int64));
				break;
			case SAA_SUM_NUM:
			case SAA_AVG_NUM:
				a[i].w = MemoryContextAllocZero(ex->cxt, cap * sizeof(__int128));
				break;
			case SAA_MIN:
			case SAA_MAX:
				a[i].i = MemoryContextAllocZero(ex->cxt, cap * sizeof(int64));
				a[i].d = MemoryContextAllocZero(ex->cxt, cap * sizeof(double));
				break;
			case SAA_SUM_F64:
			case SAA_AVG_F64:
			case SAA_O_SUM_F64:
			case SAA_O_AVG_F64:
				a[i].d = MemoryContextAllocZero(ex->cxt, cap * sizeof(double));
				a[i].nf = MemoryContextAllocZero(ex->cxt, cap);
				break;
			case SAA_O_SUM_I64:
				a[i].i = MemoryContextAllocZero(ex->cxt, cap * sizeof(int64));
				break;
			case SAA_O_COUNT_ROWS:
			case SAA_COUNT_ROWS:
				a[i].last = MemoryContextAlloc(ex->cxt, cap * sizeof(uint64));
				memset(a[i].last, 0xff, cap * sizeof(uint64));
				break;
			default:
				break;
		}
	}
	return a;
}

static void arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap);

/* every per-group array to newcap groups; remap[old] = new index (UINT32_MAX: drop), NULL: identity */
static void
groups_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	AggAcc	   *na = acc_alloc(ex, newcap);
	int64	   *nseen = MemoryContextAllocZero(ex->cxt, newcap * sizeof(int64));

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

/* the group of an id tuple in hash mode */
static uint32
hash_group(AggExec *ex, const uint32 *ids)
{
	uint64		a = ids[0] | ((uint64) ids[1] << 32),
				b = ids[2] | ((uint64) ids[3] << 32),
				h = (a * UINT64CONST(0x9E3779B97F4A7C15)) ^ (b * UINT64CONST(0xC2B2AE3D27D4EB4F));
	uint32		pos,
				g;

	h ^= h >> 29;
	pos = (uint32) h & (ex->nhslot - 1);
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
			MemoryContextAlloc(ex->cxt, (Size) ex->hkcap * SIFT_AGG_MAX_KEYS * sizeof(uint32));
	}
	memcpy(&ex->hkey[(Size) g * SIFT_AGG_MAX_KEYS], ids, SIFT_AGG_MAX_KEYS * sizeof(uint32));
	ex->hslot[pos] = g + 1;
	if ((uint64) ex->ngroups * 2 > ex->nhslot)
	{
		uint32		nn = ex->nhslot * 2;
		uint32	   *ns = MemoryContextAllocZero(ex->cxt, nn * sizeof(uint32));

		for (uint32 x = 0; x < ex->ngroups; x++)
		{
			const uint32 *k = &ex->hkey[(Size) x * SIFT_AGG_MAX_KEYS];
			uint64		ka = k[0] | ((uint64) k[1] << 32),
						kb = k[2] | ((uint64) k[3] << 32),
						hh = (ka * UINT64CONST(0x9E3779B97F4A7C15)) ^ (kb * UINT64CONST(0xC2B2AE3D27D4EB4F));
			uint32		p;

			hh ^= hh >> 29;
			p = (uint32) hh & (nn - 1);
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

static uint32
key_used(KeyState *k)
{
	return (k->ident ? k->nident : k->ot.n) + 1;
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
	if (total > DIRECT_MAX)
	{
		/* the existing groups keep their ids in the hash table */
		ex->hash = true;
		ex->nhslot = 1024;
		ex->hslot = MemoryContextAllocZero(ex->cxt, ex->nhslot * sizeof(uint32));
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

/* the id of an ident key's value when the range table is not used: ids are given in order of appearance, as the table does */
static uint32
ident_wide_id(AggExec *ex, KeyState *k, int32 v)
{
	uint32		h = (uint32) ((uint32) v * 0x9E3779B1u) & (k->wcap - 1);

	while (k->wid[h])
	{
		if (k->wval[h] == v)
			return k->wid[h];
		h = (h + 1) & (k->wcap - 1);
	}
	if (k->nident == k->capident)
	{
		k->capident = Max(k->capident * 2, 64);
		k->idval = k->idval ? repalloc(k->idval, k->capident * sizeof(int32)) :
			MemoryContextAlloc(ex->cxt, k->capident * sizeof(int32));
	}
	k->idval[k->nident++] = v;
	k->wval[h] = v;
	k->wid[h] = k->nident;
	if ((uint64) k->nident * 2 > k->wcap)
	{
		uint32		ocap = k->wcap;
		int32	   *ov = k->wval;
		uint32	   *oi = k->wid;

		k->wcap *= 2;
		k->wval = MemoryContextAlloc(ex->cxt, k->wcap * sizeof(int32));
		k->wid = MemoryContextAllocZero(ex->cxt, k->wcap * sizeof(uint32));
		for (uint32 s = 0; s < ocap; s++)
			if (oi[s])
			{
				uint32		p = (uint32) ((uint32) ov[s] * 0x9E3779B1u) & (k->wcap - 1);

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
	k->wval = MemoryContextAlloc(ex->cxt, k->wcap * sizeof(int32));
	k->wid = MemoryContextAllocZero(ex->cxt, k->wcap * sizeof(uint32));
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
	{
		if (k->nident == k->capident)
		{
			k->capident = Max(k->capident * 2, 64);
			k->idval = k->idval ? repalloc(k->idval, k->capident * sizeof(int32)) :
				MemoryContextAlloc(f->ex->cxt, k->capident * sizeof(int32));
		}
		k->idval[k->nident++] = v;
		*(uint32 *) entry = k->nident;
	}
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

	if (!(ex->arm_mask >> ki & 1))
	{
		memset(vals_out, 0, nsel * sizeof(uint32));	/* NULL in this arm */
		*map_out = NULL;
		*base_out = 0;
		return;
	}
	if (sk->kind == SAK_COMBO)
	{
		const uint32 *out = combo_outs(ex, sk->leaf);
		const uint32 *val = ex->cs[sk->leaf].val;
		int			ns = ex->spec->combo[sk->leaf].nslots;

		for (int i = 0; i < nsel; i++)
			vals_out[i] = val[(size_t) out[i] * ns + sk->slot];
		*map_out = NULL;			/* the values are the ids */
		*base_out = 0;
		return;
	}
	v = get_vec(ex, sk->leaf);
	lf = &ex->lay->leaf[sk->leaf];
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
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

			rt_cover(&k->rt, mn, mx, key_range_fill, &f);
			MemoryContextSwitchTo(old);
		}
		else
		{
			/* only NULLs: any covered value will do for the kernel, the rows are corrected below */
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

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
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

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

			if (ex->spec->key[j].kind != SAK_COMBO)
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
				const SiftSrcLeaf *lf = ex->spec->key[j].kind == SAK_COMBO ? NULL : &ex->lay->leaf[ex->spec->key[j].leaf];

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
			const SiftSrcLeaf *lf = ex->spec->key[j].kind == SAK_COMBO ? NULL : &ex->lay->leaf[ex->spec->key[j].leaf];
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
	Osa		   *o = MemoryContextAllocZero(ex->cxt, sizeof(Osa));

	o->cap = 1024;
	o->slot = MemoryContextAllocZero(ex->cxt, o->cap * sizeof(OsaSlot));
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
	o->slot = MemoryContextAllocZero(ex->cxt, o->cap * sizeof(OsaSlot));
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
			ex->oval[ai] = MemoryContextAlloc(ex->cxt, Max(o->nsid, 1) * sizeof(double));
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
			sift_aggk->gather_u64(ex->c64, (const uint64 *) v->data, idx, nsel);
			for (int j = 0; j < nsel; j++)
				if (!ok || sift_colsrc_bit(ok, j))
				{
					acc->w[ex->gid[j]] += ((const int64 *) ex->c64)[j];
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
	compute_gids(ex, t->n);
	sift_aggk->count_gid(ex->seen, ex->gid, NULL, t->n);
	for (int i = 0; i < s->naggs; i++)
		agg_update(ex, i, ex->cstart, ex->cn, t->n);
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
		uint64	   *nord = MemoryContextAlloc(ex->cxt, newcap * sizeof(uint64));

		memset(nord, 0xff, newcap * sizeof(uint64));
		for (int ai = 0; ai < sa->ninner; ai++)
		{
			int64	   *nn = MemoryContextAllocZero(ex->cxt, newcap * sizeof(int64)),
					   *ni = MemoryContextAllocZero(ex->cxt, newcap * sizeof(int64));
			double	   *nd = MemoryContextAllocZero(ex->cxt, newcap * sizeof(double));
			uint8	   *nnf = MemoryContextAllocZero(ex->cxt, newcap);

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
	const SiftSrcVec *cnt = get_vec(ex, ex->spec->group);
	const int32 *c = cnt->data;
	uint32		m = 0;

	ex->offs = MemoryContextAlloc(ex->ucxt, (u->nrow + 1) * sizeof(uint32));
	for (int r = 0; r < u->nrow; r++)
	{
		ex->offs[r] = m;
		if (!(cnt->isnull && cnt->isnull[r]) && c[r] > 0)
			m += c[r];
	}
	ex->offs[u->nrow] = m;
	ex->nelem = m;
	ex->parent = MemoryContextAlloc(ex->ucxt, (Size) m * sizeof(uint32) + SIFT_AGG_PAD);
	for (int r = 0; r < u->nrow; r++)
		for (uint32 e = ex->offs[r]; e < ex->offs[r + 1]; e++)
			ex->parent[e] = r;
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

		st->expr = ExecInitExpr((Expr *) p->expr, &ex->css.ss.ps);
		st->param = find_param(p->expr);
		rt_init(&st->rt, 1);
		if (ex->lay->leaf[p->leaf].vtype == SCV_I32)
			st->null_result = eval_bool(ex, st->expr, (Datum) 0, true);
		MemoryContextSwitchTo(old);
	}
	for (int i = 0; i < p->nchild; i++)
		register_pred(ex, p->child[i]);
}

static void
register_key(AggExec *ex, int ki)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	SiftSrcLeaf *lf = sk->kind == SAK_COMBO ? NULL : &ex->lay->leaf[sk->leaf];
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ot_init(&k->ot, sk->eqop, sk->typid, sk->collid);
	rt_init(&k->rt, sizeof(uint32));
	k->cap = 4;
	k->nullid = UINT32_MAX;
	if (sk->kind == SAK_LUT)
	{
		k->expr = ExecInitExpr((Expr *) sk->expr, &ex->css.ss.ps);
		k->param = find_param(sk->expr);
	}
	else if (sk->kind != SAK_COMBO)
	{
		/* the leaf itself: the placeholder has the key's type */
		k->param = makeNode(Param);
		k->param->paramkind = PARAM_EXEC;
		k->param->paramtype = sk->typid;
		k->param->paramtypmod = sk->typmod;
		k->param->paramcollid = sk->collid;
		k->ident = lf->vtype == SCV_I32;
	}
	MemoryContextSwitchTo(old);
}

static void
register_combos(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->tcxt = AllocSetContextCreate(ex->cxt, "sift agg combo", ALLOCSET_SMALL_SIZES);
	ex->cs = palloc0(Max(s->ncombo, 1) * sizeof(ComboSt));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		SiftAggCombo *cb = &s->combo[ci];
		ComboSt    *cs = &ex->cs[ci];

		cs->nht = 64;
		cs->ht = palloc0(cs->nht * sizeof(uint32));
		cs->slot = palloc(Max(cb->nslots, 1) * sizeof(ExprState *));
		for (int sl = 0; sl < cb->nslots; sl++)
			cs->slot[sl] = ExecInitExpr((Expr *) cb->slot[sl].expr, &ex->css.ss.ps);
		ex->clbase[ci] = ex->ncl;
		ex->ncl += cb->nleaves;
		if (cb->join >= 0)
			ex->jcombo[ex->nj++] = ci;
	}
	MemoryContextSwitchTo(old);
}

static void
register_aggs(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->dset = palloc0(Max(s->naggs, 1) * sizeof(DSet));
	ex->aot = palloc0(Max(s->naggs, 1) * sizeof(OutTable));
	ex->amax = palloc0(Max(s->naggs, 1) * sizeof(double));
	ex->osa = palloc0(Max(s->naggs, 1) * sizeof(Osa *));
	ex->oval = palloc0(Max(s->naggs, 1) * sizeof(double *));
	for (int i = 0; i < s->naggs; i++)
	{
		SiftAggAgg *a = &s->agg[i];
		TypeCacheEntry *tc;

		if (is_ordered_agg(a))
			ex->osa[i] = a->share == i ? osa_new(ex) : ex->osa[a->share];
		if (a->kind != SAA_COUNT_DISTINCT || ex->lay->leaf[a->leaf].vtype != SCV_CODE)
			continue;
		tc = lookup_type_cache(a->argtype, TYPECACHE_EQ_OPR);
		ot_init(&ex->aot[i], tc->eq_opr, a->argtype, a->collid);
	}
	MemoryContextSwitchTo(old);
}

static void
start_scan(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	uint32		total;
	int			maxv = VEC + PADW;

	ex->started = true;
	ex->glist = NULL;
	direct_layout(ex);
	total = 1;
	for (int j = 0; j < s->nkeys; j++)
		total *= ex->ks[j].cap;
	ex->gcap = total;
	ex->seen = MemoryContextAllocZero(ex->cxt, total * sizeof(int64));
	ex->acc = acc_alloc(ex, total);
	arms_init(ex);
	arms_resize(ex, total, NULL, 0);
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
	ex->okm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm2 = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpu32 = MemoryContextAlloc(ex->cxt, (Size) maxv * SIFT_AGG_MAX_KEYS * sizeof(uint32));
	sift_colsrc_begin(ex->src, 0, sift_colsrc_nsb(ex->src), NULL, 0);
	while (sift_colsrc_next(ex->src, &ex->unit))
		process_unit(ex);
	arms_flush(ex);
	osa_finish_all(ex);
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

static Datum
key_datum(AggExec *ex, int j, uint32 g, bool *isnull)
{
	KeyState   *k = &ex->ks[j];
	uint32		id = ex->hash ? ex->hkey[(Size) g * SIFT_AGG_MAX_KEYS + j] : (g / k->mult) % k->cap;

	*isnull = id == 0;
	if (id == 0)
		return (Datum) 0;
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
			return Int64GetDatum(acc->n[g]);
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
			return *isnull ? (Datum) 0 : scaled_numeric(acc->w[g], ex->lay->leaf[a->leaf].scale);
		case SAA_AVG_NUM:
			*isnull = acc->n[g] == 0;
			if (*isnull)
				return (Datum) 0;
			return DirectFunctionCall2(numeric_div, scaled_numeric(acc->w[g], ex->lay->leaf[a->leaf].scale),
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
			if (ex->lay->leaf[a->leaf].vtype == SCV_F64)
				return a->restype == FLOAT4OID ? Float4GetDatum((float4) acc->d[g]) : Float8GetDatum(acc->d[g]);
			if (a->restype == NUMERICOID)
				return scaled_numeric(acc->i[g], ex->lay->leaf[a->leaf].scale);
			return leaf_datum(a->restype, acc->i[g]);
	}
	return (Datum) 0;
}

static TupleTableSlot *
agg_next(ScanState *node)
{
	AggExec    *ex = (AggExec *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	uint32		g;

	if (!ex->started)
	{
		start_scan(ex);
		list_groups(ex);
	}
	if (ex->gpos >= ex->nglist)
		return NULL;
	g = ex->glist[ex->gpos++];
	ExecClearTuple(slot);
	for (int j = 0; j < ex->spec->nkeys; j++)
		slot->tts_values[j] = key_datum(ex, j, g, &slot->tts_isnull[j]);
	for (int i = 0; i < ex->spec->naggs; i++)
	{
		int			c = ex->spec->nkeys + i;

		slot->tts_values[c] = agg_datum(ex, i, g, &slot->tts_isnull[c]);
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
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	AggExec    *ex = (AggExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	ex->spec = sift_aggspec_unpack(cs->custom_private);
	ex->cxt = AllocSetContextCreate(estate->es_query_cxt, "sift agg", ALLOCSET_DEFAULT_SIZES);
	ex->ucxt = AllocSetContextCreate(ex->cxt, "sift agg unit", ALLOCSET_DEFAULT_SIZES);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	if (OidIsValid(ex->spec->indexoid))
		ex->index = index_open(ex->spec->indexoid, AccessShareLock);
	ex->heap = table_open(ex->spec->heapoid, AccessShareLock);
	ex->heap_opened = true;
	ex->src = sift_colsrc_open(ex->heap, ex->index, estate->es_snapshot, ex->cxt);
	ex->lay = sift_colsrc_source_layout(ex->src);
	extend_layout(ex);
	ex->uvec = MemoryContextAllocZero(ex->cxt, ex->lay->nleaves * sizeof(SiftSrcVec));
	ex->uvec_ok = MemoryContextAllocZero(ex->cxt, ex->lay->nleaves * sizeof(bool));
	ex->lutcx = CreateStandaloneExprContext();
	ex->lutcx->ecxt_param_list_info = estate->es_param_list_info;
	ex->lutcx->ecxt_param_exec_vals = ex->lutparams;
	init_states(ex);
	for (int i = 0; i < ex->spec->npred; i++)
		register_pred(ex, ex->spec->pred[i]);
	for (int i = 0; i < ex->spec->naggs; i++)
		register_pred(ex, ex->spec->agg[i].filter);
	for (int j = 0; j < ex->spec->nkeys; j++)
		register_key(ex, j);
	register_combos(ex);
	register_aggs(ex);
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
	if (ex->src)
		sift_colsrc_close(ex->src);
	if (ex->lutcx)
		FreeExprContext(ex->lutcx, true);
	if (ex->index)
		index_close(ex->index, NoLock);
	if (ex->heap_opened)
		table_close(ex->heap, NoLock);
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
	if (es->analyze && ex->started)
	{
		ExplainPropertyText("SIMD", sift_simd_level_name(sift_cpu.active), es);
		SiftSrcCounters cc;

		sift_colsrc_counters(ex->src, &cc);
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
