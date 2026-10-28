/*
 * agg/init_filter.c - the source's filters from the spec: zone pruning, text, integer, type, wide and TID conjuncts
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "port/pg_bitutils.h"

#include "agg/part.h"
#include "agg/wide.h"
#include "core/cmp.h"
#include "agg/agg_int.h"

/* the values of an IN set that has gaps in [in_lo, in_hi] (a bit table, or a hash table of values far apart), sorted */
static int64 *
set_values(const PredState *st, int *n)
{
	int64	   *vals = palloc(st->in_nv * sizeof(int64));

	*n = 0;
	if (st->in_hset.slot)
	{
		/* check-memory: allow in_hset.mask is the capacity of a hash table minus one (core/i64set.h) */
		for (uint64 h = 0; h <= st->in_hset.mask; h++)
			if (st->in_hset.slot[h] != st->in_hset.empty)
				vals[(*n)++] = st->in_hset.slot[h];
		qsort(vals, *n, sizeof(int64), sift_cmp_i64);
	}
	else
		for (uint64 w = 0; w < (st->in_nbits + 63) / 64; w++)
			for (uint64 b = st->in_bits[w]; b != 0; b &= b - 1)
				vals[(*n)++] = (int64) ((uint64) st->in_lo + w * 64 + pg_rightmost_one_pos64(b));
	return vals;
}

/* the zone test of a comparison or IN list, false when it refuses no unit (a NULL constant) */
static bool
cmp_zone(AggExec *ex, SiftAggPred *p, SiftSrcPrune *z)
{
	PredState  *st = &ex->ps[p->id];
	int64		lo = PG_INT64_MIN,
				hi = PG_INT64_MAX;

	*z = (SiftSrcPrune) {.leaf = p->leaf};
	if (!st->ready)
		eval_operand(ex, p, st);
	if (st->cnull)
		return false;
	if (p->kind == SAP_IN)
	{
		/* the NaNs of a numeric leaf are in no zone */
		if (st->in_hasnan)
			return false;
		/* no value in the set: a unit's zone holds none */
		if (st->in_nv == 0)
		{
			lo = PG_INT64_MAX;
			hi = PG_INT64_MIN;
		}
		else
		{
			lo = st->in_lo;
			hi = st->in_hi;
			/* values with gaps: a unit whose zone holds none of them is not read, which their range cannot tell */
			if (!st->in_dense && st->in_nv <= ZONE_SET_CAP)
			{
				if (st->in_vals)
				{
					z->vals = st->in_vals;
					z->nvals = st->in_nvals;
				}
				else
					z->vals = set_values(st, &z->nvals);
			}
		}
	}
	else
	{
		bool		num = ex->lay->leaf[p->leaf].typid == NUMERICOID;
		SiftCmpOp	op = num ? (SiftCmpOp) st->nop : (SiftCmpOp) p->op;
		int64		c = num ? (int64) st->cn : st->ci;

		if (num && st->konst >= 0)
		{
			/* one result for every number (a constant no scaled integer equals, or beyond all of them): the whole range passes, or no value does */
			if (st->konst == 0)
			{
				lo = PG_INT64_MAX;
				hi = PG_INT64_MIN;
			}
		}
		else
		{
			if (op != SCMP_GT && op != SCMP_GE)
				hi = op == SCMP_LT && c > PG_INT64_MIN ? c - 1 : c;
			if (op != SCMP_LT && op != SCMP_LE)
				lo = op == SCMP_GT && c < PG_INT64_MAX ? c + 1 : c;
		}
	}
	z->lo = lo;
	z->hi = hi;
	return true;
}

/* the values both sorted sets hold (none: the range of the test is empty, which prunes every unit) */
static const int64 *
meet_values(const int64 *a, int na, const int64 *b, int nb, int *n)
{
	int64	   *out = palloc(Max(Min(na, nb), 1) * sizeof(int64));

	*n = 0;
	for (int i = 0, j = 0; i < na && j < nb;)
		if (a[i] < b[j])
			i++;
		else if (a[i] > b[j])
			j++;
		else
			out[(*n)++] = a[i++], j++;
	return out;
}

/* z, ANDed with the test of its leaf in tests (a leaf is one entry: its range narrows, its sets meet) */
static List *
and_zone(List *tests, const SiftSrcPrune *z)
{
	SiftSrcPrune *t = NULL;

	foreach_ptr(SiftSrcPrune, e, tests)
		if (e->nkids == 0 && !e->nulltest && e->leaf == z->leaf)
			t = e;
	if (t == NULL)
		return lappend(tests, memcpy(palloc(sizeof(SiftSrcPrune)), z, sizeof(SiftSrcPrune)));
	t->lo = Max(t->lo, z->lo);
	t->hi = Min(t->hi, z->hi);
	if (z->nvals > 0 && t->nvals == 0)
	{
		t->nvals = z->nvals;
		t->vals = z->vals;
	}
	else if (z->nvals > 0)
	{
		/* two sets of keys on one leaf (a dimension filter and a lookup's keys): a unit has to hold a value of both */
		t->vals = meet_values(t->vals, t->nvals, z->vals, z->nvals, &t->nvals);
		if (t->nvals == 0)
		{
			t->lo = PG_INT64_MAX;
			t->hi = PG_INT64_MIN;
		}
	}
	return tests;
}

static List *zone_tests(AggExec *ex, SiftAggPred *p, List *tests);

/* the zone test of x IS [NOT] NULL (sift_aggspec_null_zone): a unit whose zone counts no NULL, or only NULLs, holds no row the test keeps */
static bool
null_zone(AggExec *ex, const SiftAggPred *p, SiftSrcPrune *z)
{
	if (!sift_aggspec_null_zone(p, ex->lay, ex->nbase))
		return false;
	*z = (SiftSrcPrune) {.leaf = p->leaf, .nulltest = p->op ? SSN_NOTNULL : SSN_ISNULL};
	return true;
}

/* the entry of tests (one, or an AND of them) that holds for p, NULL when p refuses no unit */
static SiftSrcPrune *
disjunct_zone(AggExec *ex, SiftAggPred *p)
{
	List	   *tests = zone_tests(ex, p, NIL);
	SiftSrcPrune *z;

	if (tests == NIL)
		return NULL;
	if (list_length(tests) == 1)
		return linitial(tests);
	z = palloc0(sizeof(SiftSrcPrune));
	z->nkids = list_length(tests);
	z->kids = palloc(z->nkids * sizeof(SiftSrcPrune));
	for (int i = 0; i < z->nkids; i++)
		((SiftSrcPrune *) z->kids)[i] = *(SiftSrcPrune *) list_nth(tests, i);
	return z;
}

/* an OR refuses a unit only when each disjunct does: one that refuses none leaves the OR without a test */
static List *
or_zone(AggExec *ex, SiftAggPred *p, List *tests)
{
	SiftSrcPrune *z = palloc0(sizeof(SiftSrcPrune));
	SiftSrcPrune *kids = palloc(p->nchild * sizeof(SiftSrcPrune));

	for (int i = 0; i < p->nchild; i++)
	{
		SiftSrcPrune *k = disjunct_zone(ex, p->child[i]);

		if (k == NULL)
			return tests;
		kids[i] = *k;
	}
	z->any = true;
	z->nkids = p->nchild;
	z->kids = kids;
	return lappend(tests, z);
}

/* the zone tests p implies, appended to tests: those of an AND's conjuncts, a comparison's own, an OR's as one entry */
static List *
zone_tests(AggExec *ex, SiftAggPred *p, List *tests)
{
	SiftSrcPrune z;

	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			tests = zone_tests(ex, p->child[i], tests);
	else if (p->kind == SAP_OR)
		tests = or_zone(ex, p, tests);
	else if (sift_aggspec_zone_cmp(p, ex->lay, ex->nbase) && cmp_zone(ex, p, &z))
		tests = and_zone(tests, &z);
	else if (null_zone(ex, p, &z))
		tests = lappend(tests, memcpy(palloc(sizeof(SiftSrcPrune)), &z, sizeof(SiftSrcPrune)));
	return tests;
}

/* the operands of a condition that need no row (a parameter, a stable function) as the constants they have for this statement */
static Node *
part_fold(Node *n, AggExec *ex)
{
	ExprState  *e;
	Datum		d;
	bool		isnull;
	int16		len;
	bool		byval;

	if (n == NULL || IsA(n, Const) || IsA(n, Var))
		return n;
	if (contain_var_clause(n))
		return expression_tree_mutator(n, part_fold, ex);
	e = ExecInitExpr((Expr *) n, EXPR_PARENT(ex));
	d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
	get_typlenbyval(exprType(n), &len, &byval);
	return (Node *) makeConst(exprType(n), exprTypmod(n), exprCollation(n), len, isnull ? (Datum) 0 : datumCopy(d, byval, len), isnull, byval);
}

/* the partitions a parameter or a stable function prunes when the plan starts are not read (the leader decides, the workers take its offsets) */
void
part_prune_init(AggExec *ex)
{
	List	   *conds = NIL;
	MemoryContext old;

	if (ex->spec->nparts == 0 || ex->spec->partprune == NIL || IsParallelWorker())
		return;
	old = MemoryContextSwitchTo(ex->cxt);
	foreach_ptr(Node, cl, ex->spec->partprune)
		conds = lappend(conds, part_fold(cl, ex));
	ex->parts_pruned = sift_aggpart_prune(ex->src, ex->spec->heapoid, conds);
	MemoryContextSwitchTo(old);
}

/*
 * Is the WHERE of the statement only comparisons (<, <=, =, >=, >) of one integer, date or timestamp leaf with constants of its own type, and which leaf: a count(*) of
 * it is the number of rows whose key lies in the range they make, which a segment's global order counts (sift_colsrc_count_cmp)
 */
static bool
cmp_only(AggExec *ex, SiftAggPred *p, int *leaf)
{
	SiftSrcPrune z;
	Oid			typid;

	if (p->kind == SAP_AND)
	{
		for (int i = 0; i < p->nchild; i++)
			if (!cmp_only(ex, p->child[i], leaf))
				return false;
		return true;
	}
	if (p->kind != SAP_CMP || p->op == SCMP_NE || !sift_aggspec_zone_cmp(p, ex->lay, ex->nbase) || !cmp_zone(ex, p, &z) || z.nkids > 0 || z.any || z.nvals > 0 || z.nulltest)
		return false;
	typid = ex->lay->leaf[p->leaf].typid;
	/* integers compare with integers of any width (the constant is the integer it is); a date or timestamp with its own type only */
	if (!((typid == INT2OID || typid == INT4OID || typid == INT8OID) ? (p->argtype == INT2OID || p->argtype == INT4OID || p->argtype == INT8OID) :
		  ((typid == DATEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID) && p->argtype == typid)))
		return false;
	if (*leaf >= 0 && *leaf != p->leaf)
		return false;
	*leaf = p->leaf;
	return true;
}

/* the leaf of the only comparisons of the statement (cmp_only), -1 when the conditions are anything else */
int
sift_agg_cmp_only_leaf(AggExec *ex)
{
	int			leaf = -1;

	if (ex->spec->npred == 0)
		return -1;
	for (int i = 0; i < ex->spec->npred; i++)
		if (!cmp_only(ex, ex->spec->pred[i], &leaf))
			return -1;
	return leaf;
}

/* the entries of the leaves the conjuncts compare (preagg_zones), then those of the ORs: all of them prune the scan */
void
prune_init(AggExec *ex)
{
	List	   *tests = NIL;

	ex->skip = NULL;
	for (int i = 0; i < ex->spec->npred; i++)
		tests = zone_tests(ex, ex->spec->pred[i], tests);
	ex->prune = MemoryContextAlloc(ex->cxt, Max(list_length(tests), 1) * sizeof(SiftSrcPrune));
	foreach_ptr(SiftSrcPrune, z, tests)
		if (z->nkids == 0)
			ex->prune[ex->nprune++] = *z;
	ex->nzone = ex->nprune;
	foreach_ptr(SiftSrcPrune, z, tests)
		if (z->nkids > 0)
			ex->prune[ex->nzone++] = *z;
}

/* the element tests of the conjuncts p and of the ANDs below it: they only prune, the executor tests the elements */
static int
elem_tests(AggExec *ex, const SiftAggPred *p, SiftSrcStrTest *all, int n)
{
	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			n = elem_tests(ex, p->child[i], all, n);
	else if (sift_aggspec_elem_test(p, ex->lay, ex->nbase, &all[n]))
	{
		ex->ps[p->id].elem = ++n;
		ex->npoint_elem++;
	}
	return n;
}

static int
pred_nodes(const SiftAggPred *p)
{
	int			n = 1;

	for (int i = 0; i < p->nchild; i++)
		n += pred_nodes(p->child[i]);
	return n;
}

/* The text tests of the query a source can run on the encoded form: they prune its units and narrow the rows of each (process_unit) */
void
str_init(AggExec *ex)
{
	int			cap = 0,
				n = 0;
	SiftSrcStrTest *all;

	for (int i = 0; i < ex->spec->npred; i++)
		cap += pred_nodes(ex->spec->pred[i]);
	all = palloc0(Max(cap, 1) * sizeof(SiftSrcStrTest));
	for (int i = 0; i < ex->spec->npred; i++)
	{
		if (sift_aggspec_str_test(ex->spec->pred[i], ex->lay, ex->nbase, &all[n]))
			ex->ps[ex->spec->pred[i]->id].str = &all[n++];
		else
			n = elem_tests(ex, ex->spec->pred[i], all, n);
	}
	if (n > 0)
		sift_colsrc_prune_str(ex->src, all, n);
}

/*
 * The conjuncts the source can apply on its stored integer values, as filters: a comparison of an integer-valued leaf with a constant (a
 * numeric as its scaled integer) and a short IN list. The text tests have their own path (str_init).
 */
#define FILTER_MAX_SET	8

/* a comparison or short IN list of an integer-valued leaf */
static bool
int_filter(AggExec *ex, SiftAggPred *p, SiftSrcFilter *f)
{
	PredState  *st = &ex->ps[p->id];
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];

	if (lf->vtype != SCV_I32 && lf->vtype != SCV_I64)
		return false;
	if (!st->ready)
		eval_operand(ex, p, st);
	if (st->cnull)
		return false;
	f->leaf = p->leaf;
	if (p->kind == SAP_CMP)
	{
		bool		num = lf->typid == NUMERICOID;

		if (num && st->konst >= 0)
		{
			/* every value passes (a NaN too: not null) or none does; the NaN rows of the others are the executor's */
			if (st->konst == 0 || !st->nan_res)
				return false;
			f->op = SSF_NOTNULL;
		}
		else
		{
			f->op = (SiftSrcFilterOp) (num ? st->nop : p->op);
			f->val = num ? (int64) st->cn : st->ci;
		}
		return true;
	}
	{
		int64	   *set = palloc(FILTER_MAX_SET * sizeof(int64));
		int			n = 0;

		/* the NaN rows of a numeric leaf are the executor's */
		if ((p->op && st->in_hasnull) || st->in_hset.slot || st->in_hasnan)
			return false;
		if (st->in_vals == NULL || st->in_nvals == 0 || st->in_nvals > FILTER_MAX_SET)
			return false;
		n = st->in_nvals;
		memcpy(set, st->in_vals, n * sizeof(int64));
		f->op = p->op ? SSF_NOTIN : SSF_IN;
		f->nset = n;
		f->set = set;
		return true;
	}
}

/*
 * The range [*lo, *hi] of one integer-valued leaf that a comparison, or an AND of them, keeps (an empty range has *lo > *hi); false for anything else: a <>, another
 * leaf, a numeric (its NaN rows are the executor's), or a comparison the source cannot test
 */
static bool
arm_range(AggExec *ex, SiftAggPred *p, int leaf, int64 *lo, int64 *hi)
{
	SiftSrcFilter f = {0};

	if (p->level != 0 || p->late)
		return false;
	if (p->kind == SAP_AND)
	{
		for (int i = 0; i < p->nchild; i++)
			if (!arm_range(ex, p->child[i], leaf, lo, hi))
				return false;
		return true;
	}
	if (p->kind != SAP_CMP || p->leaf != leaf || ex->lay->leaf[leaf].typid == NUMERICOID || !int_filter(ex, p, &f))
		return false;
	switch (f.op)
	{
		case SSF_EQ:
			*lo = Max(*lo, f.val);
			*hi = Min(*hi, f.val);
			return true;
		case SSF_LT:
		case SSF_LE:
			if (f.op == SSF_LT && f.val == PG_INT64_MIN)
				*hi = PG_INT64_MIN, *lo = PG_INT64_MAX;
			else
				*hi = Min(*hi, f.op == SSF_LT ? f.val - 1 : f.val);
			return true;
		case SSF_GT:
		case SSF_GE:
			if (f.op == SSF_GT && f.val == PG_INT64_MAX)
				*lo = PG_INT64_MAX, *hi = PG_INT64_MIN;
			else
				*lo = Max(*lo, f.op == SSF_GT ? f.val + 1 : f.val);
			return true;
		default:
			return false;
	}
}

#define RANGES_SHARE	8
#define RANGES_SAMPLE	16

/* The smallest and largest value of an integer leaf over RANGES_SAMPLE superblocks spread over the source (all of them when fewer): false when the source has no zones or the sample holds no value */
static bool
leaf_sample_range(AggExec *ex, int leaf, int64 *min, int64 *max)
{
	uint32		nsb = ex->src ? sift_colsrc_nsb(ex->src) : 0;
	bool		any = false;

	if (nsb == 0 || !sift_colsrc_has_zone_range(ex->src))
		return false;
	for (uint32 i = 0; i < Min(nsb, RANGES_SAMPLE); i++)
	{
		SiftSrcZone z;
		uint32		sb = (uint64) i * nsb / Min(nsb, RANGES_SAMPLE);

		if (sift_colsrc_zone_range(ex->src, sb, sb + 1, leaf, &z) <= 0 || z.nnull >= z.nvals)
			continue;
		*min = any ? Min(*min, z.min) : z.min;
		*max = any ? Max(*max, z.max) : z.max;
		any = true;
	}
	return any;
}

/* An OR whose arms are comparisons (or ANDs of them) of one integer-valued leaf with constants, as the union of the ranges they keep (the empty ones dropped) */
static bool
ranges_filter(AggExec *ex, SiftAggPred *p, SiftSrcFilter *f)
{
	SiftAggPred *c = p;
	int64	   *set,
				min,
				max;
	double		covered = 0;
	int			n = 0;

	while (c->kind == SAP_AND || c->kind == SAP_OR)
		c = c->child[0];
	if (c->kind != SAP_CMP || c->leaf >= ex->nbase || (ex->lay->leaf[c->leaf].vtype != SCV_I32 && ex->lay->leaf[c->leaf].vtype != SCV_I64))
		return false;
	set = palloc(2 * p->nchild * sizeof(int64));
	for (int i = 0; i < p->nchild; i++)
	{
		int64		lo = PG_INT64_MIN,
					hi = PG_INT64_MAX;

		if (!arm_range(ex, p->child[i], c->leaf, &lo, &hi))
			return false;
		if (lo <= hi)
		{
			set[n++] = lo;
			set[n++] = hi;
		}
	}
	/* the source tests an arm by the sorted run of a unit, which answers a range of an eighth of the rows or less: a union that covers more of the values of the index (taken as even, from a sample of its superblocks) is cheaper in the executor, on the decoded leaf */
	if (!leaf_sample_range(ex, c->leaf, &min, &max))
		return false;
	for (int a = 0; a < n; a += 2)
		covered += (double) Min(set[a + 1], max) - (double) Max(set[a], min) + 1;
	if (covered * RANGES_SHARE > (double) max - (double) min + 1)
		return false;
	f->leaf = c->leaf;
	f->op = SSF_RANGES;
	f->nset = n;
	f->set = set;
	return true;
}

void
filter_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcFilter f[SIFT_COLSRC_MAX_FILTERS];
	int			at[SIFT_COLSRC_MAX_FILTERS],
				n = 0;
	uint64		ok;

	ex->pushed = MemoryContextAllocZero(ex->cxt, Max(s->npred, 1) * sizeof(bool));
	ex->pflt = MemoryContextAllocZero(ex->cxt, Max(s->npred, 1) * sizeof(SiftSrcFilter));
	for (int i = 0; i < s->npred && n < SIFT_COLSRC_MAX_FILTERS; i++)
	{
		SiftAggPred *p = s->pred[i];

		if (p->level != 0 || p->late || (p->kind != SAP_OR && (p->leaf >= ex->nbase || (p->kind != SAP_CMP && p->kind != SAP_IN))))
			continue;
		memset(&f[n], 0, sizeof(SiftSrcFilter));
		if (p->kind == SAP_OR ? ranges_filter(ex, p, &f[n]) : int_filter(ex, p, &f[n]))
			at[n++] = i;
	}
	/* the source tests the stored rows of a unit, the rows of a match set may be renumbered: with a search the executor tests them */
	ok = n > 0 && s->search == NULL ? sift_colsrc_set_filters(ex->src, f, n) : 0;
	for (int k = 0; k < n; k++)
		if (ok >> k & 1)
		{
			ex->pushed[at[k]] = true;
			ex->pflt[at[k]] = f[k];
			ex->nfilter++;
		}
}

/* the leaves the source reads again from the heap for the rows that hold a value they cannot store */
void
recheck_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE)
		{
			sift_colsrc_set_recheck(ex->src, s->leaves[i], s->recheck[i] == SRK_RECORDSET_LAZY ? SRK_RECORDSET : s->recheck[i] == SRK_CAST_TESTED ? SRK_CAST : s->recheck[i] == SRK_IOCAST ? SRK_TEXT : s->recheck[i], NULL, InvalidOid);
			ex->recheck = true;
			ex->recheck_late |= s->recheck[i] == SRK_CAST || s->recheck[i] == SRK_IOCAST;
			if (s->recheck[i] == SRK_RECORDSET)
				for (int k = 0; k < s->npred; k++)
					ex->recheck_after_late |= s->pred[k]->late && s->pred[k]->level == 0;
		}
	/* the members of one key read through a cast: the value column of their object is read again for the member of the key only */
	for (int j = 0; j < s->nvleaf; j++)
		if (s->vleaf[j].recheck != SRK_NONE)
		{
			sift_colsrc_set_recheck(ex->src, ex->vl[j].val, SRK_CAST, s->vleaf[j].key, s->vleaf[j].cast);
			ex->recheck = true;
			ex->recheck_late = true;
		}
}

/*
 * The conjuncts on the JSON type of a leaf's path (SAP_TYPE) are the source's: it keeps the rows of the types and raises for one whose value
 * it cannot give. The other rows may hold such a value in this leaf, which its reads then give as NULL.
 */
void
type_init(AggExec *ex)
{
	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i]->kind == SAP_TYPE)
		{
			sift_colsrc_tolerate_ovf(ex->src, ex->spec->pred[i]->leaf);
			ex->pushed[i] = true;
			ex->ntype++;
		}
}

/*
 * The conjuncts on wide leaves (SAP_WIDE) are the source's: their operands are evaluated once, as Datums of the leaf's type (an IN list as the
 * elements of its array, a NULL operand as isnull), and the source returns the rows that pass test k of wide_filters.
 */
static void
wide_operand(AggExec *ex, const SiftAggPred *p, SiftSrcWideTest *t)
{
	bool		isnull;
	ExprState  *e;
	Datum		d;
	Datum	   *vals = palloc(sizeof(Datum));
	bool	   *nulls = palloc(sizeof(bool));
	int16		len;
	bool		byval;
	char		align;

	if (p->op == SWT_ISNULL || p->op == SWT_NOTNULL)
		return;
	e = operand_state(ex, p->expr);
	d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
	get_typlenbyvalalign(p->argtype, &len, &byval, &align);
	t->nvals = 1;
	nulls[0] = isnull;
	if (isnull)
		vals[0] = (Datum) 0;
	else if (p->op == SWT_IN || p->op == SWT_NOTIN)
		deconstruct_array(DatumGetArrayTypePCopy(d), p->argtype, len, byval, align, &vals, &nulls, &t->nvals);
	else
		vals[0] = datumCopy(d, byval, len);
	t->vals = vals;
	t->isnull = nulls;
}

void
wide_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcWideTest t[SIFT_AGG_MAX_WIDE];
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	int			n = 0;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE)
		{
			t[n] = (SiftSrcWideTest) {.leaf = s->pred[i]->leaf, .op = s->pred[i]->op};
			wide_operand(ex, s->pred[i], &t[n++]);
			ex->pushed[i] = true;
		}
	SIFT_INVARIANT(n == 0 || sift_colsrc_set_wide(ex->src, t, n) == n, "sift: the source did not take the conjuncts on wide leaves");
	ex->nwide = n;
	MemoryContextSwitchTo(old);
}

/* The TID range of the SAP_TID conjuncts: the scan reads the superblocks it touches, and a heap's edge rows are looked up for their visible version */
void
tid_init(AggExec *ex, Snapshot snapshot)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->sb_heap = sift_colsrc_sb_heap(ex->src);
	ex->tid_pred = -1;
	ex->tid_sb_hi = PG_UINT32_MAX;
	for (int i = 0; i < s->npred && ex->tid_pred < 0; i++)
		if (s->pred[i]->kind == SAP_TID)
			ex->tid_pred = s->pred[i]->id;
	if (ex->tid_pred >= 0)
	{
		sift_aggspec_tid_range(s, &ex->tid_lo, &ex->tid_hi);
		ex->tid_sb_hi = 0;
		ex->tid_rows = palloc(VEC * sizeof(uint32));
		ex->tid_tids = palloc(VEC * sizeof(ItemPointerData));
		if (ex->tid_lo < ex->tid_hi)
		{
			ex->tid_blo = ex->tid_lo >> 16;
			ex->tid_bhi = (ex->tid_hi - 1) >> 16;
			ex->tid_sb_lo = ex->tid_blo / SIFT_SB_PAGES;
			ex->tid_sb_hi = ex->tid_bhi / SIFT_SB_PAGES + 1;
		}
		/* units cut by TID range hold rows of any block: they are all read, and the rows tested */
		if (!ex->sb_heap)
		{
			ex->tid_sb_lo = 0;
			ex->tid_sb_hi = PG_UINT32_MAX;
		}
		if (!ex->lay->is_table)
		{
			ex->tid_vis = palloc(sizeof(SiftVis));
			sift_vis_begin(ex->tid_vis, ex->heap, snapshot, NULL, 0);
			ex->tid_slot = table_slot_create(ex->heap, NULL);
		}
	}
	MemoryContextSwitchTo(old);
}
