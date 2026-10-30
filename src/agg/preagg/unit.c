/*
 * agg/preagg/unit.c - the pre-aggregates of a unit as the scan asks for them
 * The checks that a unit may answer from its stored sums, histograms and rollups, and the
 * entry the scan calls for each unit.
 */
#include "postgres.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/agg_int.h"
#include "agg/preagg/int.h"

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* a unit that holds a value a leaf the source reads again cannot store has the row SQL raises for, also when the query reads other columns: it is scanned */
static bool
preagg_rechecked(AggExec *ex)
{
	const SiftAggSpec *s = ex->spec;

	for (int i = 0; ex->recheck && i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE && sift_colsrc_overflow(ex->src, &ex->unit, s->leaves[i]))
			return true;
	for (int j = 0; ex->recheck && j < s->nvleaf; j++)
		if (s->vleaf[j].recheck != SRK_NONE && sift_colsrc_overflow(ex->src, &ex->unit, ex->vl[j].val))
			return true;
	return false;
}

/*
 * The aggregates that add `leaf * mul + add` of an int8 leaf (PostgreSQL raises for a result outside int8) can take the unit's sums only when its zone
 * puts every result inside int8: a unit that may hold a value that raises is scanned, and the scan raises.
 */
static bool
preagg_arith(AggExec *ex)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		sift_i128	mul,
					add,
					lo,
					hi;
		SiftSrcUnitAgg ua;

		if (a->num.n < 2)
			continue;
		if (!sift_colsrc_unit_agg(ex->src, &ex->unit, sift_aggnum_linear(&a->num, &mul, &add), &ua))
			return false;
		if (ua.nnull == ua.nvals)
			continue;
		if (!ua.has_minmax)
			return false;
		lo = Min(mul * ua.min, mul * ua.max) + add;
		hi = Max(mul * ua.min, mul * ua.max) + add;
		if (lo < PG_INT64_MIN || hi > PG_INT64_MAX)
			return false;
	}
	return true;
}

/* the unit answered from the histogram of hdim (its calendar cells when hcal) */
static bool
preagg_hist_hdim(AggExec *ex)
{
	bool		ok;

	ex->pa.dim = ex->pa.hdim;
	ex->pa.cal = ex->pa.hcal;
	ok = preagg_zones(ex) && preagg_hist(ex);
	/* no histogram of the date in this unit (it has too many days): its calendar cells, which a comparison that cuts one leaves to a scan */
	if (!ok && ex->pa.hcalalt)
	{
		ex->pa.cal = true;
		ok = preagg_zones(ex) && preagg_hist(ex);
	}
	ex->pa.dim = -1;
	ex->pa.cal = false;
	return ok;
}

static bool preagg_unit_exact(AggExec *ex);

#define PA_DEAD_TRIES 8

/* The aggregates that a unit net of its dead rows can give: counts and integer sums, not the minimum and maximum, the distinct values or the sketch of a unit, nor a float sum, which only an exact unit adds in its order */
static bool
preagg_dead_ok(AggExec *ex)
{
	if (ex->pa.list >= 0)
		return false;
	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		int			leaf = pa_leaf(a);

		if (a->kind == SAA_MIN || a->kind == SAA_MAX || a->kind == SAA_COUNT_DISTINCT || a->kind == SAA_GEN || a->kind == SAA_SUM_F64 || a->kind == SAA_AVG_F64 ||
			(leaf >= 0 && a->kind != SAA_COUNT && ex->lay->leaf[leaf].vtype == SCV_F64))
			return false;
	}
	return true;
}

static bool
preagg_unit_try(AggExec *ex)
{
	bool		ok;
	int			dead;

	ex->pa_xleaf = -1;
	/* a segment the dictionary of the leaf counts the rows of: the rows that pass the one test, as the source read them from it */
	if (ex->pcount && ex->unit.nsu > 0)
	{
		int64		rows;

		if (sift_colsrc_unit_count(ex->src, &ex->unit, &rows))
		{
			preagg_count(ex, rows);
			return true;
		}
		if (!ex->pcount_cmp)
			return false;
	}
	if (ex->pa.ktab && ex->unit.nsu == 0)
		return false;
	if (PA_ROLLUP(ex->pa) && rollup_skip(ex))
		return false;
	if (sift_colsrc_unit_exact(ex->src, &ex->unit))
		return preagg_unit_exact(ex);
	/* a unit with dead rows and every page all-visible: its pre-aggregates minus the dead rows' (counts and integer sums), the unit's rows being the live ones meanwhile */
	/* a statement whose first dirty units, read for their dead rows, all went to their scans anyway (the histogram it needs is missing) stops paying for the attempt */
	if (!ex->pa_dead || (ex->n_dead_units == 0 && ex->pa_dead_miss >= PA_DEAD_TRIES))
		return false;
	/* its zone is checked before its dead rows are read: a comparison the zone is not inside sends the unit to its scan, which has them from the liveness bits */
	if (sift_colsrc_unit_dead(ex->src, &ex->unit, SSD_BOUNDS) < 0 || !preagg_zone_bounds(ex))
	{
		sift_colsrc_unit_dead(ex->src, &ex->unit, SSD_OFF);
		ex->n_dead_scan++;
		return false;
	}
	if ((dead = sift_colsrc_unit_dead(ex->src, &ex->unit, SSD_ON)) >= 0)
	{
		ex->unit.nrow -= dead;
		ok = preagg_unit_exact(ex);
		ex->unit.nrow += dead;
	}
	else
		ok = false;
	sift_colsrc_unit_dead(ex->src, &ex->unit, SSD_OFF);
	if (ok)
		ex->n_dead_units++;
	else
	{
		ex->n_dead_scan++;
		ex->pa_dead_miss++;
	}
	return ok;
}

/* A single NULL test needs only its zone count, even when the unit has no histogram of the leaf. */
static bool
preagg_null_count(AggExec *ex)
{
	const SiftAggSpec *s = ex->spec;
	const SiftAggPred *p;
	SiftSrcUnitAgg a;
	uint32 rows;

	if (s->nkeys != 0 || ex->pa.list >= 0 || s->npred != 1)
		return false;
	p = s->pred[0];
	if (p->kind != SAP_NULL || p->op > 1)
		return false;
	for (int ai = 0; ai < s->naggs; ai++)
		if (s->agg[ai].kind != SAA_COUNT_STAR || s->agg[ai].filter)
			return false;
	if (!sift_colsrc_unit_agg(ex->src, &ex->unit, p->leaf, &a) || a.has_nan || a.nvals != ex->unit.nrow)
		return false;
	rows = p->op ? a.nvals - a.nnull : a.nnull;
	preagg_count(ex, rows);
	return true;
}

static bool
preagg_unit_exact(AggExec *ex)
{
	if (preagg_rechecked(ex) || !preagg_arith(ex))
		return false;
	if (preagg_null_count(ex))
		return true;
	/* the rollup of the rows tests the comparisons of its dims on its tuples, so the unit need not lie inside them; the others (a range of a date) the unit lies inside, or it scans;
	 * a unit without the rollup waits for a merge, nothing is asked for */
	if (ex->pa.rows)
	{
		if (!preagg_zones(ex))
			return false;
		if (preagg_rollup(ex))
			return true;
		ex->n_rollup_miss += ex->pa_miss;
		return false;
	}
	/* the zone is not inside a comparison on the dim: its groups that pass it, the other comparisons still hold for the zone */
	if (ex->pa.hdim >= 0 && !preagg_zones(ex))
		return preagg_hist_hdim(ex);
	if (!preagg_zones(ex))
		return false;
	if (ex->pa.list < 0)
	{
		if (ex->pa.dim >= 0)
		{
			bool		ok = preagg_hist(ex);

			/* no histogram of the date in this unit (it has too many days): its calendar cells, which a bound that cuts one leaves to a scan */
			if (!ok && ex->pa.calalt)
			{
				ex->pa.cal = true;
				ok = preagg_hist(ex);
				ex->pa.cal = false;
			}
			return ok;
		}
		/* the sums lack something the unit does not keep (the key list of a leaf of key_columns): its histogram of hdim may hold it */
		return preagg_sums(ex) || (ex->pa.hdim >= 0 && preagg_hist_hdim(ex));
	}
	if (preagg_rollup(ex))
		return true;
	/* the unit is clean but its rollup lacks something: scan it now, and ask for the rollup it should have */
	if (ex->pa_miss)
	{
		ex->n_rollup_miss++;
		ex->n_rollup_queued += sift_colsrc_unit_rollup_miss(ex->src, &ex->unit);
	}
	return false;
}

/* the unit from its pre-aggregates, or false for a scan; a unit that needed its exceptions (an IS NULL the zone could not decide) counts as net of them or scanned */
bool
preagg_unit(AggExec *ex)
{
	bool		ok = preagg_unit_try(ex);

	if (ex->pa_xleaf >= 0 && ex->unit.nsu == 0)
	{
		if (ok)
			ex->n_exc_units++;
		else
			ex->n_exc_scan++;
	}
	ex->pa_xleaf = -1;
	return ok;
}

void
preagg_init(AggExec *ex)
{
	SiftSrcPreagg pa;

	ex->pa_on = ex->pa_dead = ex->whole_on = false;
	ex->pa_dead_miss = 0;
	ex->pa_why = SPW_OFF;
	if (!sift_agg_preagg)
		return;
	sift_colsrc_preagg(ex->index ? ex->index : ex->spec->nparts > 0 ? sift_aggpart_heap(ex->src, 0) : ex->heap, &pa);
	ex->pa_why = !pa.aggs && pa.ndims == 0 && pa.nrollups == 0 ? SPW_NONE : sift_aggspec_preagg_why(ex->spec, ex->lay, ex->nbase, &pa, &ex->pa);
	ex->pa_on = ex->pa_why == SPW_OK;
	ex->pa_dead = ex->pa_on && sift_agg_dead_delta > 0 && preagg_dead_ok(ex);
	ex->super_on = ex->pa_on && !PA_ROLLUP(ex->pa) && sift_agg_super && ex->search == NULL && !ex->pa.ktab && sift_colsrc_super(ex->src, true);
	if (ex->pa_on && PA_ROLLUP(ex->pa))
	{
		rollup_need(ex);
		sift_aggspec_unit_costs(ex->spec, ex->lay, &ex->pa_elem_cost, &ex->pa_tuple_cost);
		ex->pa_scan_run = 0;
	}
	/* a comparison with NULL keeps no row, and leaves no interval for the zone to lie inside; nor does an IN list with gaps (or a list that prune_init gave no zone) */
	foreach_ptr(SiftAggPred, p, sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase, false))
	{
		PredState  *st = &ex->ps[p->id];

		if (!st->ready)
			eval_operand(ex, p, st);
		/* the tuples of the rollup of the rows test the comparison themselves, whatever interval it gives the zones */
		if (ex->pa_on && !ex->pa.rows && (st->cnull || (p->kind == SAP_IN && !st->in_dense)))
		{
			/* the groups of the dim's histogram test an IN list with gaps, which gives the zones no interval: the sums cannot answer */
			if (!st->cnull && p->leaf == ex->pa.hdim && !ex->pa.hcal)
			{
				ex->pa.dim = ex->pa.hdim;
				ex->pa.hdim = -1;
			}
			else
			{
				ex->pa_on = ex->pa_dead = false;
				ex->pa_why = SPW_ZONE;
			}
		}
	}
	/* a count(*) of one text test (equality, range or prefix): the source may count a segment from its dictionary (option count_columns) */
	ex->pcount = ex->pcount_cmp = false;
	if ((ex->super_on || (sift_agg_super && ex->search == NULL)) && ex->spec->nkeys == 0 && ex->spec->group < 0 && ex->spec->npred >= 1 && !ex->spec->left_lateral && ex->ntype == 0 && ex->nwide == 0)
	{
		const SiftSrcStrTest *t = ex->spec->npred == 1 && ex->nfilter == 0 ? ex->ps[ex->spec->pred[0]->id].str : NULL;
		bool		only = true,
					text;

		for (int ai = 0; only && ai < ex->spec->naggs; ai++)
			only = ex->spec->agg[ai].kind == SAA_COUNT_STAR && ex->spec->agg[ai].filter == NULL;
		for (int i = 0; only && ex->pushed && t != NULL && i < ex->spec->npred; i++)
			only = !ex->pushed[i];
		text = ex->super_on && only && t != NULL && t->nvals == 1 && (t->op == SSTR_EQ || (t->op >= SSTR_LT && t->op <= SSTR_GE) || t->op == SSTR_PREFIX);
		if (text)
		{
			sift_colsrc_count_range(ex->src, t->leaf, t->op, t->vals[0], t->lens[0]);
			ex->pcount = true;
		}
		/* ... or of the one test that an element of a list equals a value: a segment whose point directory lists the rows of the value counts them */
		else if (only && t == NULL && ex->spec->npred == 1 && ex->nfilter == 0 && ex->spec->pred[0]->kind == SAP_ANY_ELEM && ex->spec->pred[0]->op == 0 &&
				 !ex->spec->pred[0]->late && ex->npoint_elem == 1 && ex->ps[ex->spec->pred[0]->id].elem == 1)
		{
			sift_colsrc_count_elem(ex->src);
			ex->pcount = true;
		}
		/* ... or of comparisons of one integer leaf (the source applies them to the units it reads): a segment with the global order of it counts the rows between the bounds */
		else if (ex->super_on && only && t == NULL)
		{
			int			leaf = sift_agg_cmp_only_leaf(ex);

			if (leaf >= 0)
			{
				sift_colsrc_count_cmp(ex->src, leaf);
				ex->pcount = ex->pcount_cmp = true;
			}
		}
	}
	/* the key table of the leaf the histogram is of answers for a segment, when the comparisons are all on that leaf */
	if (ex->pa_on && !PA_ROLLUP(ex->pa) && sift_agg_super && ex->search == NULL && !ex->pa.cal && !ex->pa.hcal && (ex->pa.dim >= 0 || ex->pa.hdim >= 0))
	{
		int			key = ex->pa.dim >= 0 ? ex->pa.dim : ex->pa.hdim,
					meas[SIFT_SRC_MAX_MEAS],
					nmeas = 0;
		bool		only = true;

		for (int k = 0; k < ex->nprune; k++)
			only &= ex->prune[k].leaf == key && !ex->prune[k].nulltest;
		/* the measures it reads: the table is taken only when it keeps them all (a unit that lacks one is scanned, so is a segment) */
		for (int ai = 0; only && ai < ex->spec->naggs; ai++)
		{
			int			leaf = pa_leaf(&ex->spec->agg[ai]),
						m = 0;

			if (leaf < 0 || leaf == key || ex->spec->agg[ai].kind == SAA_COUNT_DISTINCT)
				continue;
			while (m < nmeas && meas[m] != leaf)
				m++;
			if (m == nmeas && nmeas == SIFT_SRC_MAX_MEAS)
				only = false;
			else if (m == nmeas)
				meas[nmeas++] = leaf;
		}
		ex->whole_on = only && sift_colsrc_whole(ex->src, key, meas, nmeas);
	}
}
