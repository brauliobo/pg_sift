/*
 * agg/init_filter.c - the source's filters from the spec: zone pruning, text, integer, type, wide and TID conjuncts
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/part.h"
#include "agg/wide.h"
#include "core/cmp.h"
#include "agg/agg_int.h"

/* the values of an IN set that has gaps in [in_lo, in_hi] (a byte table, or a hash table of values far apart), sorted */
static int64 *
set_values(const PredState *st, int *n)
{
	int64	   *vals = palloc(st->in_nv * sizeof(int64));

	*n = 0;
	if (st->in_htab)
	{
		for (uint64 h = 0; h <= st->in_hmask; h++)
			if (st->in_hused[h])
				vals[(*n)++] = st->in_htab[h];
		qsort(vals, *n, sizeof(int64), sift_cmp_i64);
	}
	else
		for (uint32 i = 0; i < st->in_nlut; i++)
			if (st->in_lut[i])
				vals[(*n)++] = st->in_lo + i;
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
			/* values with gaps: a unit whose zone holds none of them is not read, which their range cannot tell. A byte table longer than
			 * ZONE_SET_MAX is left to its range: the values are sorted again at each begin of a range of units, about 1 us each, and a
			 * set that long meets the zone of almost every unit */
			if (!st->in_dense && (st->in_htab || st->in_nv <= ZONE_SET_MAX))
				z->vals = set_values(st, &z->nvals);
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

/* z, ANDed with the test of its leaf in tests (a leaf is one entry: its range narrows, the first set stays) */
static List *
and_zone(List *tests, const SiftSrcPrune *z)
{
	SiftSrcPrune *t = NULL;

	foreach_ptr(SiftSrcPrune, e, tests)
		if (e->nkids == 0 && e->leaf == z->leaf)
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
	return tests;
}

static List *zone_tests(AggExec *ex, SiftAggPred *p, List *tests);

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

/* the entries of the leaves the conjuncts compare (preagg_zones), then those of the ORs: all of them prune the scan */
void
prune_init(AggExec *ex)
{
	List	   *tests = NIL;

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

/* The text tests of the query a source can run on the encoded form: they prune its units and narrow the rows of each (process_unit) */
void
str_init(AggExec *ex)
{
	SiftSrcStrTest *all = palloc(Max(ex->spec->npred, 1) * sizeof(SiftSrcStrTest));
	int			n = 0;

	for (int i = 0; i < ex->spec->npred; i++)
		if (sift_aggspec_str_test(ex->spec->pred[i], ex->lay, ex->nbase, &all[n]))
			ex->ps[ex->spec->pred[i]->id].str = &all[n++];
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
		if ((p->op && st->in_hasnull) || st->in_htab || st->in_hasnan)
			return false;
		for (uint32 i = 0; i < st->in_nlut; i++)
			if (st->in_lut[i] && n++ < FILTER_MAX_SET)
				set[n - 1] = (int64) st->in_lo + i;
		if (n == 0 || n > FILTER_MAX_SET)
			return false;
		f->op = p->op ? SSF_NOTIN : SSF_IN;
		f->nset = n;
		f->set = set;
		return true;
	}
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

		if (p->level != 0 || p->late || p->leaf >= ex->nbase || (p->kind != SAP_CMP && p->kind != SAP_IN))
			continue;
		memset(&f[n], 0, sizeof(SiftSrcFilter));
		if (int_filter(ex, p, &f[n]))
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
			sift_colsrc_set_recheck(ex->src, s->leaves[i], s->recheck[i] == SRK_RECORDSET_LAZY ? SRK_RECORDSET : s->recheck[i] == SRK_CAST_TESTED ? SRK_CAST : s->recheck[i], NULL, InvalidOid);
			ex->recheck = true;
			ex->recheck_late |= s->recheck[i] == SRK_CAST;
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
	e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
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
		if (!ex->lay->is_table)
		{
			ex->tid_vis = palloc(sizeof(SiftVis));
			sift_vis_begin(ex->tid_vis, ex->heap, snapshot, NULL, 0);
			ex->tid_slot = table_slot_create(ex->heap, NULL);
		}
	}
	MemoryContextSwitchTo(old);
}
