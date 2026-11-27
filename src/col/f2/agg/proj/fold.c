/*
 * col/f2/agg/proj/fold.c - the stored, dead and live rows of a statement folded into one cube (col/f2/agg/proj/fold.h).
 */
#include "core/host.h"

#include "col/f2/agg/proj/fold.h"

SiftProjFold *
sift_proj_fold_new(const SiftProjSpec *query, const SiftProjSpec *src, const SiftProjLayoutCol *lay, int nlay, const SiftProjRollup *map, const SiftProjPred *preds, int npreds,
				   int ncols, const SiftProjColumns *cols)
{
	SiftProjFold *f = palloc0(sizeof(SiftProjFold));
	MemoryContext old;

	*f = (SiftProjFold) {.query = query,.src = src,.lay = lay,.nlay = nlay,.map = map,.preds = preds,.npreds = npreds,
	.ncols = ncols,.row = palloc0(Max(ncols, 1) * sizeof(SiftCell)),.ok = true};
	f->stored_preds = palloc(Max(npreds, 1) * sizeof(SiftProjPred));
	f->stored_vals = palloc(Max(npreds, 1) * sizeof(SiftProjConst));
	f->nstored_preds = sift_proj_stored_preds(map, preds, npreds, f->stored_preds, f->stored_vals);
	sift_mem_init(&f->mem, "proj_fold", CurrentMemoryContext, true, SIFT_BUDGET_WORK_MEM, 1, 1);
	old = MemoryContextSwitchTo(f->mem.root_cx);
	f->cube = sift_proj_cube_new(query, cols);
	for (int j = 0; j < query->nmeas; j++)
		if (query->meas[j].fn == SPM_COUNT_DISTINCT)
			f->dist[j] = sift_proj_distinct_new(query, src, cols, map, j, (int64) (f->mem.budget / 128));
	MemoryContextSwitchTo(old);
	return f;
}

void
sift_proj_fold_free(SiftProjFold *f)
{
	sift_mem_end(&f->mem);
	pfree(f->stored_preds);
	pfree(f->stored_vals);
	pfree(f->row);
	pfree(f);
}

static bool
reads_exception(const SiftProjSpec *q, const SiftCell *row)
{
	for (int i = 0; i < q->nkeys; i++)
		if (row[q->key[i].leaf].tag != 0)
			return true;
	for (int j = 0; j < q->nmeas; j++)
		if (q->meas[j].leaf >= 0 && row[q->meas[j].leaf].tag != 0)
			return true;
	return false;
}

/* The columns of a stored row a FILTER or a count(DISTINCT) of the statement reads hold no exception tag: the cells can judge them */
static bool
stored_judgeable(const SiftProjFold *f)
{
	for (int j = 0; j < f->query->nmeas; j++)
	{
		const SiftProjMeasure *m = &f->query->meas[j];

		if (f->map->filter_mode[j] == SPFM_ON_KEY)
			for (int k = 0; k < m->nfilter; k++)
				if (f->row[m->filter[k].leaf].tag != 0)
					return false;
		if (m->fn == SPM_COUNT_DISTINCT && f->row[m->leaf].tag != 0)
			return false;
	}
	return true;
}

/* A cube past work_mem stops growing: the statement is read from the base */
static void
check_budget(SiftProjFold *f)
{
	f->ok &= sift_mem_pressure(&f->mem, false) != SIFT_MEM_HARD;
}

void
sift_proj_fold_stored(SiftProjFold *f, const SiftCell *stored)
{
	f->nstored++;
	if (!f->ok)
		return;
	memset(f->row, 0, f->ncols * sizeof(SiftCell));
	for (int i = 0; i < f->nlay; i++)
		if (f->lay[i].role == SPR_KEY)
			f->row[f->lay[i].leaf] = stored[i];
	for (int i = 0; i < f->query->nkeys; i++)
		if (stored[f->map->src_key[i]].tag != 0)
			f->ok = false;
	if (!sift_proj_preds_decidable(f->stored_preds, f->nstored_preds, f->row) || !stored_judgeable(f))
		f->ok = false;
	else if (sift_proj_preds_pass(f->stored_preds, f->nstored_preds, f->row))
	{
		sift_proj_cube_absorb(f->cube, f->src, f->lay, f->nlay, f->map, stored);
		for (int j = 0; j < f->query->nmeas; j++)
			if (f->dist[j] != NULL)
				sift_proj_distinct_stored(f->dist[j], f->lay, f->nlay, stored,
										  f->map->filter_mode[j] != SPFM_ON_KEY || sift_proj_spec_preds_pass(f->query->meas[j].filter, f->query->meas[j].nfilter, f->row));
		check_budget(f);
	}
}

void
sift_proj_fold_dead(SiftProjFold *f, const SiftCell *row)
{
	f->ndead++;
	if (!f->ok)
		return;
	if (reads_exception(f->query, row) || !sift_proj_preds_decidable(f->preds, f->npreds, row))
		f->ok = false;
	else if (sift_proj_spec_preds_pass(f->src->where, f->src->nwhere, row) && sift_proj_preds_pass(f->preds, f->npreds, row))
	{
		f->ok &= sift_proj_cube_add(f->cube, row, -1);
		for (int j = 0; j < f->query->nmeas; j++)
			if (f->dist[j] != NULL)
				sift_proj_distinct_row(f->dist[j], row, -1);
		check_budget(f);
	}
}

void
sift_proj_fold_live(SiftProjFold *f, const SiftCell *row)
{
	f->nlive++;
	if (!f->ok)
		return;
	if (reads_exception(f->query, row) || !sift_proj_preds_decidable(f->preds, f->npreds, row))
		f->ok = false;
	else
	{
		f->ok &= sift_proj_cube_add(f->cube, row, 1);
		for (int j = 0; j < f->query->nmeas; j++)
			if (f->dist[j] != NULL)
				sift_proj_distinct_row(f->dist[j], row, 1);
		check_budget(f);
	}
}

bool
sift_proj_fold_finish(SiftProjFold *f)
{
	for (int j = 0; j < f->query->nmeas; j++)
		if (f->dist[j] != NULL)
		{
			f->ok &= sift_proj_distinct_ok(f->dist[j]);
			if (f->ok)
				sift_proj_distinct_finish(f->dist[j], f->cube);
		}
	return f->ok;
}
