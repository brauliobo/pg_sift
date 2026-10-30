/*
 * agg/part/stats.c - the planner side of a partitioned source
 * The check that the partitions share one layout, and the statistics of a partition packed
 * for the plan and added back.
 */
#include "postgres.h"

#include "agg/part/int.h"

bool
sift_aggpart_same_layout(const SiftSrcLayout *a, const SiftSrcLayout *b)
{
	if (a->is_table != b->is_table || a->natts != b->natts || a->nleaves != b->nleaves || !equal(a->attexprs, b->attexprs))
		return false;
	for (int i = 0; i < a->nleaves; i++)
	{
		const SiftSrcLeaf *x = &a->leaf[i],
				   *y = &b->leaf[i];

		if (x->id != y->id || x->attno != y->attno || x->parent != y->parent || x->kind != y->kind || x->vtype != y->vtype || x->typid != y->typid ||
			x->wtypid != y->wtypid || x->shape != y->shape || x->wide != y->wide || x->scale != y->scale || x->lossy != y->lossy || x->astext != y->astext ||
			!equal(x->path, y->path) || strcmp(x->name, y->name) != 0)
			return false;
	}
	return true;
}

SiftPartStats *
sift_aggpart_pack_stats(const SiftSrcStats *s, int nleaves)
{
	SiftPartStats *p = palloc(offsetof(SiftPartStats, leaf) + Min(nleaves, SIFT_COLSRC_MAX_LEAVES) * sizeof(p->leaf[0]));

	p->nrows = s->nrows;
	p->nunits = s->nunits;
	p->bytes = s->bytes;
	p->allvisible_frac = s->allvisible_frac;
	p->exact_frac = s->exact_frac;
	p->rows_miss = s->rows_miss;
	p->nleaves = Min(nleaves, SIFT_COLSRC_MAX_LEAVES);
	for (int i = 0; i < p->nleaves; i++)
	{
		p->leaf[i].elems_per_row = s->elems_per_row[i];
		p->leaf[i].dim_miss = s->dim_miss[i];
		p->leaf[i].mm_miss = s->mm_miss[i];
		p->leaf[i].roll_miss = s->roll_miss[i];
		p->leaf[i].ndistinct = s->ndistinct[i];
		p->leaf[i].min = s->min[i];
		p->leaf[i].max = s->max[i];
		p->leaf[i].ovf = s->ovf[i];
		p->leaf[i].nokinds = s->nokinds[i];
		p->leaf[i].jnull = s->jnull[i];
	}
	return p;
}

/* the number of a leaf's values is a lower bound over several sources (the distinct values of a leaf may repeat among them), its range the union */
void
sift_aggpart_add_stats(SiftSrcStats *tot, const SiftPartStats *s)
{
	double		w = tot->nrows + s->nrows;

	if (w > 0)
	{
		tot->allvisible_frac = (tot->allvisible_frac * tot->nrows + s->allvisible_frac * s->nrows) / w;
		tot->exact_frac = (tot->exact_frac * tot->nrows + s->exact_frac * s->nrows) / w;
		tot->rows_miss = (tot->rows_miss * tot->nrows + s->rows_miss * s->nrows) / w;
	}
	for (int i = 0; i < s->nleaves; i++)
	{
		if (w > 0)
		{
			tot->elems_per_row[i] = (tot->elems_per_row[i] * tot->nrows + s->leaf[i].elems_per_row * s->nrows) / w;
			tot->dim_miss[i] = (tot->dim_miss[i] * tot->nrows + s->leaf[i].dim_miss * s->nrows) / w;
			tot->mm_miss[i] = (tot->mm_miss[i] * tot->nrows + s->leaf[i].mm_miss * s->nrows) / w;
			tot->roll_miss[i] = (tot->roll_miss[i] * tot->nrows + s->leaf[i].roll_miss * s->nrows) / w;
		}
		if (tot->min[i] == 0 && tot->max[i] == 0)
		{
			tot->min[i] = s->leaf[i].min;
			tot->max[i] = s->leaf[i].max;
		}
		else if (s->leaf[i].min != 0 || s->leaf[i].max != 0)
		{
			tot->min[i] = Min(tot->min[i], s->leaf[i].min);
			tot->max[i] = Max(tot->max[i], s->leaf[i].max);
		}
		tot->ndistinct[i] = Max(tot->ndistinct[i], s->leaf[i].ndistinct);
		tot->ovf[i] |= s->leaf[i].ovf;
		tot->nokinds[i] |= s->leaf[i].nokinds;
		tot->jnull[i] |= s->leaf[i].jnull;
	}
	tot->nrows += s->nrows;
	tot->nunits += s->nunits;
	tot->bytes += s->bytes;
}
