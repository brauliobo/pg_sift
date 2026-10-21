/*
 * agg/init_filter.c - the source's filters from the spec: zone pruning, text, integer, type, wide and TID conjuncts
 */
#include "postgres.h"

#include "utils/datum.h"

#include "agg/wide.h"
#include "agg/agg_int.h"

static int
cmp_int64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

/* the values of a hash table of an IN set, sorted */
static int64 *
hash_values(const PredState *st)
{
	int64	   *vals = palloc(st->in_nv * sizeof(int64));
	uint32		n = 0;

	for (uint64 h = 0; h <= st->in_hmask; h++)
		if (st->in_hused[h])
			vals[n++] = st->in_htab[h];
	qsort(vals, n, sizeof(int64), cmp_int64);
	return vals;
}

void
prune_init(AggExec *ex)
{
	List	   *cmps = sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase);

	ex->prune = MemoryContextAlloc(ex->cxt, Max(list_length(cmps), 1) * sizeof(SiftSrcPrune));
	foreach_ptr(SiftAggPred, p, cmps)
	{
		PredState  *st = &ex->ps[p->id];
		SiftSrcPrune *z = NULL;
		int64	   *vals = NULL;
		int64		lo = PG_INT64_MIN,
					hi = PG_INT64_MAX;

		if (!st->ready)
			eval_operand(ex, p, st);
		if (st->cnull)
			continue;
		if (p->kind == SAP_IN)
		{
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
				/* values far apart: a unit whose zone holds none of them is not read, which their range cannot tell */
				if (st->in_htab)
					vals = hash_values(st);
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
		for (int k = 0; k < ex->nprune; k++)
			if (ex->prune[k].leaf == p->leaf)
				z = &ex->prune[k];
		if (z == NULL)
		{
			z = &ex->prune[ex->nprune++];
			*z = (SiftSrcPrune) {p->leaf, PG_INT64_MIN, PG_INT64_MAX};
		}
		z->hi = Min(z->hi, hi);
		z->lo = Max(z->lo, lo);
		if (vals && z->nvals == 0)
		{
			z->nvals = st->in_nv;
			z->vals = vals;
		}
	}
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

		if ((p->op && st->in_hasnull) || st->in_htab)
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
			sift_colsrc_set_recheck(ex->src, s->leaves[i], s->recheck[i]);
			ex->recheck = true;
			ex->recheck_late |= s->recheck[i] == SRK_CAST;
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
	if (n > 0 && sift_colsrc_set_wide(ex->src, t, n) != n)
		elog(ERROR, "sift: the source did not take the conjuncts on wide leaves");
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
