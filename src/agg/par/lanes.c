/*
 * agg/par/lanes.c - the float sums of the lanes of a scan
 * The lanes keep float sums apart so that a result does not depend on the number of workers; the checks of avg(float8) of the whole scan.
 */
#include "postgres.h"

#include <float.h>

#include "utils/float.h"

#include "agg/par/int.h"

/* the check that avg(float8) owes (float_guard): the inputs of the whole scan could have overflowed the sum of squares */
void
final_checks(AggExec *g, int64 ntuples)
{
	for (int i = 0; i < g->spec->naggs; i++)
		if (g->spec->agg[i].kind == SAA_AVG_F64 && 8.0 * ntuples * g->amax[i] * g->amax[i] >= DBL_MAX)
			float_overflow_error();
	for (int k = 0; k < g->spec->narms; k++)
		for (int i = 0; i < g->spec->arm[k].ninner; i++)
		{
			SiftAggKind kind = g->spec->arm[k].inner[i].kind;

			if ((kind == SAA_AVG_F64 || kind == SAA_X_AVG) && 8.0 * ntuples * g->arm[k].amax[i] * g->arm[k].amax[i] >= DBL_MAX)
				float_overflow_error();
		}
}

/* the float sums of the lane just scanned are added to those of the lanes before it, as the leader adds the table of each lane */
void
lane_fold(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		double	   *pg_restrict dt = ex->acc[i].dt;
		const double *pg_restrict d = ex->acc[i].d;
		uint32		g = 0;

		if (dt == NULL)
			continue;
		/* the pointers in locals: through the accumulator every store would reload them and keep the loop scalar */
		for (; g + 4 <= n; g += 4)
		{
			dt[g] += d[g];
			dt[g + 1] += d[g + 1];
			dt[g + 2] += d[g + 2];
			dt[g + 3] += d[g + 3];
		}
		for (; g < n; g++)
			dt[g] += d[g];
		memset(ex->acc[i].d, 0, n * sizeof(double));
	}
}

/* the float sums of the lane just scanned are kept apart: the lanes are added in order when every process is done (lanes_sum) */
void
lane_keep(AggExec *ex, uint32 lane)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		AggAcc	   *a = &ex->acc[i];

		if (a->lv == NULL)
			continue;
		a->lv[lane] = MemoryContextAllocZero(ex->tcxt, ex->gcap * sizeof(double));
		memcpy(a->lv[lane], a->d, n * sizeof(double));
		memset(a->d, 0, n * sizeof(double));
	}
}

/* the sums of the lanes, in lane order, then the sums of the rows after the lanes (the tail of a preserved side) */
void
lanes_sum(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		AggAcc	   *a = &ex->acc[i];

		if (a->lv == NULL)
			continue;
		for (uint32 g = 0; g < n; g++)
		{
			double		t = 0;

			for (int l = 0; l < SIFT_AGG_LANES; l++)
				if (a->lv[l])
					t += a->lv[l][g];
			a->d[g] = t + a->d[g];
		}
	}
}
