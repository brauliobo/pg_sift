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
#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
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

/* what the zones of the units say of one integer leaf (the columns of sift.col_clustering) */
typedef struct LeafClust
{
	int64		n;				/* units with a value and a usable zone */
	double		useful_pct;		/* NaN when the leaf has no unit */
	double		point;			/* units an equality touches, on average */
	double		range;
	double		ideal;
	double		depth;
	double		corr;
	bool		has_range;		/* point, range, ideal and depth are set */
	bool		has_corr;
	const char *verdict;
} LeafClust;

static void
leaf_clustering(SiftColSegment **seg, int nseg, int c, int64 units, double width, LeafClust *o)
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
				depth = 0,
				span;

	*o = (LeafClust) {.useful_pct = NAN};
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
	o->n = n;
	if (n + nbad > 0)
		o->useful_pct = 100.0 * useful / (n + nbad);
	if (n > 0)
	{
		double		half = width * span / 2;
		double	   *slo = palloc(n * sizeof(double)),
				   *shi = palloc(n * sizeof(double));
		double		share;

		memcpy(slo, lo, n * sizeof(double));
		memcpy(shi, hi, n * sizeof(double));
		qsort(slo, n, sizeof(double), dbl_cmp);
		qsort(shi, n, sizeof(double), dbl_cmp);
		for (int64 i = 0; i < n; i++)
		{
			/* the zones that hold the middle of zone i, and those that meet the window around it; a zone with an overflow value is always read */
			point += count_below(slo, n, pr[i].mid, true) - count_below(shi, n, pr[i].mid, false);
			range += count_below(slo, n, pr[i].mid + half, true) - count_below(shi, n, pr[i].mid - half, false);
			/* the deepest point of the zones is at the lower end of one of them */
			depth = Max(depth, count_below(slo, n, lo[i], true) - count_below(shi, n, lo[i], false));
		}
		point = point / n + nbad;
		range = range / n + nbad;
		share = point / (n + nbad);
		o->has_range = true;
		o->depth = (int64) depth + nbad;
		o->point = point;
		o->range = range;
		o->ideal = Min(n, 1 + width * n) + nbad;
		o->has_corr = order_correlation(pr, n, &o->corr);
		o->verdict = point <= 2 || share <= 0.02 ? "clustered" : share <= 0.25 ? "partial" : "scattered";
	}
	pfree(za);
	pfree(lo);
	pfree(hi);
	pfree(pr);
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
		Datum		v[11] = {0};
		bool		nulls[11] = {false, false, false, true, true, true, true, true, true, true, true};
		bool		scored = def->store == SIFT_COLS_INT && def->kind == SIFT_COLK_SCALAR && def->lane == 0 && units > 0;

		v[0] = Int32GetDatum(c);
		v[1] = CStringGetTextDatum(def->path[0] ? def->path : NameStr(TupleDescAttr(RelationGetDescr(index), def->attno - 1)->attname));
		v[2] = Int64GetDatum(units);
		if (scored)
		{
			LeafClust	lc;

			leaf_clustering(seg, nseg, c, units, width, &lc);
			nulls[3] = false;
			v[3] = Int64GetDatum(lc.n);
			if (!isnan(lc.useful_pct))
			{
				nulls[4] = false;
				v[4] = Float8GetDatum(lc.useful_pct);
			}
			if (lc.has_range)
			{
				nulls[5] = nulls[6] = nulls[7] = nulls[10] = false;
				v[10] = Int64GetDatum((int64) lc.depth);
				v[5] = Float8GetDatum(lc.point);
				v[6] = Float8GetDatum(lc.range);
				v[7] = Float8GetDatum(lc.ideal);
				if (lc.has_corr)
				{
					nulls[8] = false;
					v[8] = Float8GetDatum(lc.corr);
				}
				nulls[9] = false;
				v[9] = CStringGetTextDatum(lc.verdict);
			}
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	for (int s = 0; s < nseg; s++)
		sift_cseg_close(seg[s]);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* the rows a value of the heap column of a leaf has, from the statistics of the column (ANALYZE): 0 when they say nothing */
static double
leaf_rows_per_value(Relation index, const SiftColDef *def, double rows)
{
	AttrNumber	att = index->rd_index->indkey.values[def->attno - 1];
	HeapTuple	tp;
	double		nd;

	if (att == 0 || (tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(index->rd_index->indrelid), Int16GetDatum(att), BoolGetDatum(false))) == NULL)
		return 0;
	nd = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
	ReleaseSysCache(tp);
	if (nd < 0)
		nd = -nd * rows;
	return nd > 0 ? rows / nd : 0;
}

/*
 * After a build: the key a workload looks rows up by (a leaf of point_columns, point_tid_columns or key_columns that is not unique) whose equality reads
 * many units, when no other leaf is ordered by the table: key_cut then keeps the key ordered through writes and merges and widens nothing. The same
 * rule as the key_cut note of sift.cluster_advice(), which also reads the state of an index that was built before.
 */
void
sift_col_report_key_cut(Relation index)
{
	const SiftColRel *rel;
	const SiftColSchema *sc;
	SiftDirSnapshot *dir;
	SiftColSegment **seg;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col key_cut report", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	int			nseg = 0;
	int64		units = 0;
	int			key = -1;
	LeafClust	kc = {0};
	double		rows = 0;
	bool		competing = false;

	if (!sift_cluster_advice || !sift_col_options(index)->cluster_advice || sift_col_key_cut(sift_col_rel(index)))
		goto done;
	rel = sift_col_rel_private(index, cx);	/* the loops read catalogs, which reload the cached state */
	sc = rel->schema;
	dir = palloc(sizeof(SiftDirSnapshot));
	seg = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftColSegment *));
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			seg[nseg] = sift_cseg_open(index, &dir->segs[i], cx);
			units += seg[nseg]->nsb;
			rows += dir->segs[i].ndocs;
			nseg++;
		}
	if (units < 16)
		goto close;
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		LeafClust	lc;

		if (def->store != SIFT_COLS_INT || def->kind != SIFT_COLK_SCALAR || def->lane != 0 ||
			!(rel->pagg.point[c] || rel->pagg.point_tid[c] || rel->pagg.pres[c] || rel->pagg.set[c]))
			continue;
		leaf_clustering(seg, nseg, c, units, 0.01, &lc);
		if (lc.has_range && strcmp(lc.verdict, "scattered") == 0 && lc.point > kc.point &&
			(leaf_rows_per_value(index, def, rows) == 0 || leaf_rows_per_value(index, def, rows) >= 2))
		{
			key = c;
			kc = lc;
		}
	}
	for (int c = 0; key >= 0 && !competing && c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		LeafClust	lc;

		if (c == key || def->store != SIFT_COLS_INT || def->kind != SIFT_COLK_SCALAR || def->lane != 0)
			continue;
		leaf_clustering(seg, nseg, c, units, 0.01, &lc);
		competing = lc.has_range && strcmp(lc.verdict, "scattered") != 0;
	}
	if (key >= 0 && !competing)
	{
		const char *name = quote_identifier(RelationGetRelationName(index));
		const char *leaf = NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[key].attno - 1)->attname);

		MemoryContextSwitchTo(old);
		ereport(NOTICE,
				(errmsg("sift_col index %s: an equality on \"%s\" reads %.0f of %lld units, and no other leaf of the index follows the order of the table",
						name, leaf, kc.point, (long long) units),
				 errdetail("key_cut = '%s' cuts the units by that key instead of by block range, so a lookup reads the few units of its range and keeps doing so through writes and merges without a rewrite of the table; no leaf loses its zones, and the index grows by the address lists of the units.", leaf),
				 errhint("CREATE INDEX .. WITH (key_cut = '%s'), or ALTER INDEX %s SET (key_cut = '%s') and REINDEX; sift.cluster_advice() says the same for an index built before, and the setting sift.cluster_advice = off silences both.", leaf, name, leaf)));
		MemoryContextSwitchTo(cx);
	}
close:
	for (int i = 0; i < nseg; i++)
		sift_cseg_close(seg[i]);
done:
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}
