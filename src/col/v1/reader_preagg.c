/*
 * col/v1/reader_preagg.c - the pre-aggregates the segments of a Format 1 index were written with, as the planner reads them (col/v1/reader.h), and the leaf its units are cut by.
 */
#include "postgres.h"

#include "col/am.h"
#include "col/col.h"
#include "col/v1/pagg.h"
#include "col/v1/reader.h"

void
sift_src_preagg_spec(const SiftCPaggSpec *spec, SiftSrcPreagg *pa)
{
	pa->aggs = true;
	if (spec->off)
		return;
	memcpy(pa->dsets, spec->dsets, sizeof(pa->dsets));
	for (int c = 0; c < SIFT_COL_MAX_COLS && pa->npres < SIFT_SRC_MAX_DIMS; c++)
		if (spec->pres[c])
			pa->pres[pa->npres++] = c;
	memcpy(pa->hll, spec->hll, sizeof(pa->hll));
	for (int d = 0; d < spec->ndims; d++)
	{
		pa->dim[pa->ndims].leaf = spec->dim[d].leaf;
		pa->dim[pa->ndims].nmeas = spec->dim[d].nmeas;
		pa->dim[pa->ndims].minmax = spec->minmax;
		pa->dim[pa->ndims].cal = spec->dim[d].cal;
		for (int m = 0; m < spec->dim[d].nmeas; m++)
			pa->dim[pa->ndims].meas[m] = spec->dim[d].meas[m];
		pa->ndims++;
	}
	/* the measures of the key tables, which a unit's cube need not hold: the key leaf is a dim of the segments (only theirs, unless a unit has its own histogram of it) */
	for (int c = 0; c < SIFT_COL_MAX_COLS; c++)
	{
		int			d = -1;
		bool		cal = false;

		if (spec->ktn[c] == 0)
			continue;
		for (int i = 0; i < pa->ndims; i++)
			if (pa->dim[i].leaf == c)
			{
				cal |= pa->dim[i].cal;
				d = pa->dim[i].cal ? d : i;
			}
		/* a date with calendar cells only: they answer, the table does not */
		if (d < 0 && (cal || pa->ndims == SIFT_SRC_MAX_DIMS))
			continue;
		if (d < 0)
		{
			d = pa->ndims++;
			pa->dim[d].leaf = c;
			pa->dim[d].nmeas = 0;
			pa->dim[d].minmax = true;
			pa->dim[d].cal = false;
			pa->dim[d].ktab = true;
		}
		for (int m = 0; m < spec->ktn[c]; m++)
		{
			int			l = spec->ktm[c][m],
						j = 0;

			while (j < pa->dim[d].nmeas && pa->dim[d].meas[j] != l)
				j++;
			if (j == pa->dim[d].nmeas && j < SIFT_SRC_MAX_MEAS)
				pa->dim[d].meas[pa->dim[d].nmeas++] = l;
		}
	}
	for (int r = 0; r < spec->nrollups; r++)
	{
		const SiftCPaggRollup *ru = &spec->rollup[r];

		pa->rollup[pa->nrollups].list = ru->list;
		pa->rollup[pa->nrollups].ndims = ru->ndims;
		pa->rollup[pa->nrollups].nmeas = ru->nmeas;
		for (int j = 0; j < ru->ndims; j++)
			pa->rollup[pa->nrollups].dim[j] = ru->dim[j];
		for (int m = 0; m < ru->nmeas; m++)
			pa->rollup[pa->nrollups].meas[m] = ru->meas[m];
		pa->nrollups++;
	}
}

void
sift_v1_idx_preagg(Relation index, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;

	sift_col_pagg_spec(index, &spec);
	sift_src_preagg_spec(&spec, pa);
}

int
sift_v1_idx_key_leaf(Relation index)
{
	return sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index)) ? sift_col_rel(index)->cut[0] : -1;
}
