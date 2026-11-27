/*
 * col/f2/agg/proj/distinct.c - the multiplicities behind count(DISTINCT) (col/f2/agg/proj/distinct.h).
 */
#include "core/host.h"

#include "col/f2/agg/proj/distinct.h"

struct SiftProjDistinct
{
	SiftProjSpec pairs;			/* the statement's groups, then the counted column: a cube with no measure */
	SiftProjSpec src;
	SiftProjRollup map;			/* of the pairs */
	SiftProjMeasure measure;	/* the statement's: its FILTER */
	SiftProjCube *cube;
	int			nq;				/* the statement's group columns */
	int			index;			/* of the measure in the statement */
	int16		leaf;
	int64		max_pairs;
	bool		over;
};

SiftProjDistinct *
sift_proj_distinct_new(const SiftProjSpec *query, const SiftProjSpec *src, const SiftProjColumns *cols, const SiftProjRollup *map, int measure, int64 max_pairs)
{
	SiftProjDistinct *d = palloc0(sizeof(SiftProjDistinct));
	int			at = -1;

	d->pairs = (SiftProjSpec) {.kind = SIFT_PROJ_AGG,.nkeys = query->nkeys + 1,.distinct_leaf = -1};
	memcpy(d->pairs.key, query->key, query->nkeys * sizeof(SiftProjKey));
	d->leaf = query->meas[measure].leaf;
	d->pairs.key[query->nkeys] = (SiftProjKey) {.leaf = d->leaf,.fn = SPF_COLUMN};
	for (int k = 0; k < src->nkeys; k++)
		if (src->key[k].leaf == d->leaf && src->key[k].fn == SPF_COLUMN)
			at = k;
	d->src = *src;
	d->map = (SiftProjRollup) {.nkeys = query->nkeys + 1};
	memcpy(d->map.src_key, map->src_key, query->nkeys * sizeof(int));
	memcpy(d->map.unit, map->unit, query->nkeys * sizeof(uint8));
	d->map.src_key[query->nkeys] = at;
	d->measure = query->meas[measure];
	d->cube = sift_proj_cube_new(&d->pairs, cols);
	d->nq = query->nkeys;
	d->index = measure;
	d->max_pairs = max_pairs;
	return d;
}

static void
check_bound(SiftProjDistinct *d)
{
	d->over |= sift_proj_cube_size(d->cube) > d->max_pairs;
}

void
sift_proj_distinct_stored(SiftProjDistinct *d, const SiftProjLayoutCol *lay, int nlay, const SiftCell *stored, bool filter_ok)
{
	if (!filter_ok || d->over)
		return;
	sift_proj_cube_absorb(d->cube, &d->src, lay, nlay, &d->map, stored);
	check_bound(d);
}

void
sift_proj_distinct_row(SiftProjDistinct *d, const SiftCell *row, int sign)
{
	if (d->over || !sift_proj_spec_preds_pass(d->measure.filter, d->measure.nfilter, row))
		return;
	(void) sift_proj_cube_add(d->cube, row, sign);
	check_bound(d);
}

bool
sift_proj_distinct_ok(const SiftProjDistinct *d)
{
	return !d->over;
}

void
sift_proj_distinct_finish(SiftProjDistinct *d, SiftProjCube *cube)
{
	int			n = sift_proj_cube_ngroups(d->cube);

	for (int i = 0; i < n; i++)
	{
		const SiftProjGroup *g = sift_proj_cube_group(d->cube, i);

		if (g->rows > 0 && !g->key[d->nq].isnull)
			sift_proj_cube_touch(cube, g->key)->st[d->index].cnt++;
	}
}
