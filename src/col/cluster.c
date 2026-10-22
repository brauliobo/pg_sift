/*
 * col/cluster.c - sift.col_clustering: how well the rows of a sift_col index are ordered for its zone maps.
 *
 * A unit is skipped by a comparison when its zone [min, max] holds no value that passes. So an equality or a narrow range touches few units
 * when the values of a leaf are sorted or arrive in order, and every unit when they are scattered. This reads the zones of the immutable
 * segments only (no chunk): for each integer leaf the units a predicate on a value of the data would touch, as the planner's sample counts them.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/seg.h"

/* sift.cluster_advice: read by the SQL function sift.cluster_advice() through current_setting */
bool		sift_cluster_advice = true;

PG_FUNCTION_INFO_V1(sift_col_clustering);

typedef struct Probe
{
	double		mid;
	uint32		pos;
	double		rank;
} Probe;

typedef struct ZoneAt
{
	uint32		sb;
	double		lo;
	double		hi;
} ZoneAt;

static int
zone_cmp(const void *a, const void *b)
{
	uint32		x = ((const ZoneAt *) a)->sb,
				y = ((const ZoneAt *) b)->sb;

	return x < y ? -1 : x > y;
}

static int
dbl_cmp(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return x < y ? -1 : x > y;
}

static int
probe_cmp(const void *a, const void *b)
{
	double		x = ((const Probe *) a)->mid,
				y = ((const Probe *) b)->mid;

	return x < y ? -1 : x > y;
}

/* the number of elements of the sorted a[0..n) that are < v (strict) or <= v */
static int64
count_below(const double *a, int64 n, double v, bool inclusive)
{
	int64		lo = 0,
				hi = n;

	while (lo < hi)
	{
		int64		mid = lo + (hi - lo) / 2;

		if (inclusive ? a[mid] <= v : a[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Spearman's correlation of the position of the zones with the order of their middles; NULL when all the middles are equal */
static bool
order_correlation(Probe *p, int64 n, double *out)
{
	double		mean = (n - 1) / 2.0,
				sxy = 0,
				sxx = 0,
				syy = 0;

	if (n < 2)
		return false;
	qsort(p, n, sizeof(Probe), probe_cmp);
	for (int64 i = 0; i < n;)
	{
		int64		j = i;

		while (j < n && p[j].mid == p[i].mid)
			j++;
		for (int64 k = i; k < j; k++)
			p[k].rank = (i + j - 1) / 2.0;
		i = j;
	}
	for (int64 i = 0; i < n; i++)
	{
		double		x = p[i].pos - mean,
					y = p[i].rank - mean;

		sxy += x * y;
		sxx += x * x;
		syy += y * y;
	}
	if (syy == 0)
		return false;
	*out = sxy / sqrt(sxx * syy);
	return true;
}

Datum
sift_col_clustering(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	double		width = PG_GETARG_FLOAT8(1) / 100.0;
	const SiftColSchema *sc;
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftColSegment **seg = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftColSegment *));
	int			nseg = 0;
	int64		units = 0;

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	if (width < 0 || width > 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("width_pct must be between 0 and 100")));
	sc = sift_col_rel(index)->schema;
	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
			seg[nseg++] = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
	for (int s = 0; s < nseg; s++)
		units += seg[s]->nsb;

	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		Datum		v[10] = {0};
		bool		nulls[10] = {false, false, false, true, true, true, true, true, true, true};
		bool		scored = def->store == SIFT_COLS_INT && def->kind == SIFT_COLK_SCALAR && def->lane == 0 && units > 0;

		v[0] = Int32GetDatum(c);
		v[1] = CStringGetTextDatum(def->path[0] ? def->path : NameStr(TupleDescAttr(RelationGetDescr(index), def->attno - 1)->attname));
		v[2] = Int64GetDatum(units);
		if (scored)
		{
			ZoneAt	   *za = palloc(units * sizeof(ZoneAt));
			double	   *lo = palloc(units * sizeof(double)),
					   *hi = palloc(units * sizeof(double));
			Probe	   *pr = palloc(units * sizeof(Probe));
			int64		n = 0,
						nbad = 0,
						useful = 0;
			double		gmin = INFINITY,
						gmax = -INFINITY,
						point = 0,
						range = 0,
						corr,
						span,
						share;

			for (int s = 0; s < nseg; s++)
				for (uint32 u = 0; u < seg[s]->nsb; u++)
				{
					SiftCZone	z;

					sift_cseg_zone(seg[s], c, u, &z);
					if (z.flags & SIFT_COLZ_OVERFLOW)
						nbad++;
					else if (z.nnull < z.nvals)
						za[n++] = (ZoneAt) {seg[s]->sbdir[u].sb, (double) z.min, (double) z.max};
				}
			/* the segments of a parallel build interleave their superblocks: the order of the units is that of the heap */
			qsort(za, n, sizeof(ZoneAt), zone_cmp);
			for (int64 i = 0; i < n; i++)
			{
				lo[i] = za[i].lo;
				hi[i] = za[i].hi;
				pr[i] = (Probe) {(lo[i] + hi[i]) / 2, i, 0};
				gmin = Min(gmin, lo[i]);
				gmax = Max(gmax, hi[i]);
			}
			span = gmax - gmin;
			for (int64 i = 0; i < n; i++)
				useful += hi[i] - lo[i] < span;
			nulls[3] = false;
			v[3] = Int64GetDatum(n);
			if (n + nbad > 0)
			{
				nulls[4] = false;
				v[4] = Float8GetDatum(100.0 * useful / (n + nbad));
			}
			if (n > 0)
			{
				double		half = width * span / 2;
				double	   *slo = palloc(n * sizeof(double)),
						   *shi = palloc(n * sizeof(double));

				memcpy(slo, lo, n * sizeof(double));
				memcpy(shi, hi, n * sizeof(double));
				qsort(slo, n, sizeof(double), dbl_cmp);
				qsort(shi, n, sizeof(double), dbl_cmp);
				for (int64 i = 0; i < n; i++)
				{
					/* the zones that hold the middle of zone i, and those that meet the window around it; a zone with an overflow value is always read */
					point += count_below(slo, n, pr[i].mid, true) - count_below(shi, n, pr[i].mid, false);
					range += count_below(slo, n, pr[i].mid + half, true) - count_below(shi, n, pr[i].mid - half, false);
				}
				point = point / n + nbad;
				range = range / n + nbad;
				share = point / (n + nbad);
				nulls[5] = nulls[6] = nulls[7] = false;
				v[5] = Float8GetDatum(point);
				v[6] = Float8GetDatum(range);
				v[7] = Float8GetDatum(Min(n, 1 + width * n) + nbad);
				if (order_correlation(pr, n, &corr))
				{
					nulls[8] = false;
					v[8] = Float8GetDatum(corr);
				}
				nulls[9] = false;
				v[9] = CStringGetTextDatum(point <= 2 || share <= 0.02 ? "clustered" : share <= 0.25 ? "partial" : "scattered");
			}
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	for (int s = 0; s < nseg; s++)
		sift_cseg_close(seg[s]);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
