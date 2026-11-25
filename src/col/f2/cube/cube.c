/*
 * col/f2/cube/cube.c - the cube of a scope summary: exact aggregates of a set of rows that merge and subtract. The bytes are in cubewire.c.
 *
 * A cube holds the row count, the time bounds, for each measure the exact aggregates of its non-NULL values (count, minimum, maximum, sum and
 * sum of squares as 128-bit integers, or the float8 sum) and for each dim the histogram group key -> (count, per-measure non-NULL count and sum,
 * and the minimum and maximum when the shape says so). A rollup is the same histogram over the combinations of the values of several dims, a
 * distinct set the ids of the rows of one declared leaf: COUNT(DISTINCT) of any month the cube is of.
 * Two cubes of disjoint row sets merge into the cube of the union and a cube of a subset subtracts from the cube of the set: what a delete or a
 * pair edit does to a summary without a scan. The minimum, maximum and time bounds, the distinct sets and a float8 sum that cancels do not
 * subtract exactly; sift_scube_sub says which of them may be off.
 */
/* check-memory: allow-allocs the arrays follow the groups of one dim (at most SIFT_SCOPE_DIM_GROUPS), the ids of a set and the months of one key, not the rows of a table */
#include "core/host.h"

#include <math.h>

#include "core/cmp.h"
#include "col/f2/cube/int.h"

#define USECS_PER_DAY_V6	INT64CONST(86400000000)
#define DAYS_1970_TO_2000	10957

static inline int64
floor_div(int64 a, int64 b)
{
	int64		q = a / b;

	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* Hinnant's civil-from-days: the proleptic Gregorian date of a day count since 1970-01-01 */
static void
civil_from_days(int64 z, int64 *y, int *m)
{
	int64		era,
				yoe,
				doy,
				mp,
				doe;

	z += 719468;
	era = floor_div(z, 146097);
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*m = (int) (mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

static int64
days_from_civil(int64 y, int m)
{
	int64		era,
				yoe,
				doy,
				doe;

	y -= m <= 2;
	era = floor_div(y, 400);
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

int32
sift_scope_month_of(int64 ts)
{
	int64		y;
	int			m;

	civil_from_days(floor_div(ts, USECS_PER_DAY_V6) + DAYS_1970_TO_2000, &y, &m);
	return (int32) ((y - 2000) * 12 + (m - 1));
}

int64
sift_scope_month_start(int32 month_id)
{
	int64		y = 2000 + floor_div(month_id, 12);
	int			m = (int) (month_id - floor_div(month_id, 12) * 12) + 1;

	return (days_from_civil(y, m) - DAYS_1970_TO_2000) * USECS_PER_DAY_V6;
}

int64
sift_scope_month_end(int32 month_id)
{
	int64		next;

	if (month_id >= sift_scope_month_of(PG_INT64_MAX))
		return PG_INT64_MAX;
	next = sift_scope_month_start(month_id + 1);
	return next;
}

/*
 * A month cell holds the rows of one UTC calendar month. Its months are the local months of the session only when the stored value is not an instant
 * (timestamp, date: no zone converts it) or when the session TimeZone is fixed UTC; under any other zone (an offset, DST, a zone with history) the rows
 * in the hours around a month boundary sit in the neighbouring local month, and a cell answers a local month wrong.
 */
bool
sift_scope_monthmap_tz_safe(bool zoned, bool tz_is_utc)
{
	return !zoned || tz_is_utc;
}

/* The cells that lie inside the instants [lo, hi) whatever the zone: months first..last (false: none). The rows of [lo, month_start(first)) and
 * [month_end(last), hi) are the ones a scan reads; the top month is never inside, its end is the clamp and not an instant. */
bool
sift_scope_months_inside(int64 lo, int64 hi, int32 *first, int32 *last)
{
	int32		f = sift_scope_month_of(lo);

	if (sift_scope_month_start(f) < lo)
		f++;
	*first = f;
	*last = sift_scope_month_of(hi) - 1;
	return f <= *last;
}

/* ---------------------------------------------------------------------------
 * 1. Accumulator
 * ------------------------------------------------------------------------- */

static inline bool
is_float(const SiftScopeCubeAcc *a, uint32 m)
{
	return (a->shape.float_meas >> m) & 1;
}

static inline double
bits_double(int64 v)
{
	double		d;

	memcpy(&d, &v, sizeof(d));
	return d;
}

/* The order of two values of a measure: as float8 for a float measure (NaN last, -0.0 equal to 0.0) */
static inline int
meas_cmp(const SiftScopeCubeAcc *a, uint32 m, int64 x, int64 y)
{
	return is_float(a, m) ? sift_f8_cmp(bits_double(x), bits_double(y)) : SIFT_CMP3(x, y);
}

static inline bool
meas_less(const SiftScopeCubeAcc *a, uint32 m, int64 x, int64 y)
{
	return meas_cmp(a, m, x, y) < 0;
}

bool
sift_scope_shape_equal(const SiftScopeShape *a, const SiftScopeShape *b)
{
	if (a->nmeas != b->nmeas || a->ndims != b->ndims || a->float_meas != b->float_meas || a->nrollups != b->nrollups ||
		a->ndsets != b->ndsets || a->minmax != b->minmax)
		return false;
	for (uint32 r = 0; r < a->nrollups; r++)
		if (a->rollup[r].nd != b->rollup[r].nd || memcmp(a->rollup[r].dim, b->rollup[r].dim, a->rollup[r].nd) != 0)
			return false;
	return true;
}

/* The dims then the rollups, one numbering: what the loops over every histogram of a cube walk */
static inline uint32
ndim_all(const SiftScopeCubeAcc *a)
{
	return a->shape.ndims + a->shape.nrollups;
}

static inline SiftScopeDim *
dim_at(const SiftScopeCubeAcc *a, uint32 i)
{
	return (SiftScopeDim *) (i < a->shape.ndims ? &a->dim[i] : &a->rollup[i - a->shape.ndims]);
}

/* A cube read in key order has its tables ascending: building leaves one append-only and hashed, and reading puts it right (no change to what the cube holds) */
static void
sort_dims(const SiftScopeCubeAcc *a)
{
	for (uint32 i = 0; i < ndim_all(a); i++)
		sift_sdim_sort(&a->shape, dim_at(a, i));
}

/* The dim of the cube that gives component j of the key of histogram i */
static inline uint32
comp_dim(const SiftScopeCubeAcc *a, uint32 i, uint32 j)
{
	return i < a->shape.ndims ? i : a->shape.rollup[i - a->shape.ndims].dim[j];
}

uint32
sift_dim_limit(const SiftScopeDim *d)
{
	return d->nk == 1 ? SIFT_SCOPE_DIM_GROUPS : SIFT_SCOPE_ROLLUP_TUPLES;
}

void
sift_dim_size(SiftScopeCubeAcc *a, SiftScopeDim *d, uint32 ngroups)
{
	d->groups = palloc0((Size) ngroups * sift_rec_size(&a->shape, d->nk));
	d->cap = ngroups;
	d->ngroups = ngroups;
}

void
sift_scube_init(SiftScopeCubeAcc *a, SiftScopeShape shape)
{
	SIFT_INVARIANT(shape.nmeas <= SIFT_SCOPE_MEAS_CAP && shape.ndims <= SIFT_SCOPE_DIMS_CAP && shape.nrollups <= SIFT_SCOPE_ROLLUPS_CAP &&
				   shape.ndsets <= SIFT_SCOPE_DSETS_CAP, "scope cube shape out of range");
	memset(a, 0, sizeof(*a));
	a->shape = shape;
	for (uint32 m = 0; m < shape.nmeas; m++)
		a->leaf[m].zflags = is_float(a, m) ? SIFT_SLA_FLOAT : 0;
	for (uint32 d = 0; d < shape.ndims; d++)
		a->dim[d].nk = 1;
	for (uint32 r = 0; r < shape.nrollups; r++)
	{
		SIFT_INVARIANT(shape.rollup[r].nd >= 1 && shape.rollup[r].nd <= SIFT_SCOPE_RDIMS_CAP, "scope cube: a rollup of %u dims", shape.rollup[r].nd);
		for (uint32 j = 0; j < shape.rollup[r].nd; j++)
			SIFT_INVARIANT(shape.rollup[r].dim[j] < shape.ndims, "scope cube: a rollup of a dim that is not there");
		a->rollup[r].nk = shape.rollup[r].nd;
	}
}

void
sift_scube_clear(SiftScopeCubeAcc *a)
{
	SiftScopeDim keep[SIFT_SCOPE_DIMS_CAP + SIFT_SCOPE_ROLLUPS_CAP];
	SiftScopeDSet *sets[SIFT_SCOPE_DSETS_CAP];

	for (uint32 i = 0; i < ndim_all(a); i++)
	{
		keep[i] = *dim_at(a, i);
		keep[i].ngroups = 0;
		keep[i].overflow = keep[i].unsorted = false;
		if (keep[i].ht != NULL)
			pfree(keep[i].ht);
		keep[i].ht = NULL;
		keep[i].ht_cap = 0;
	}
	memcpy(sets, a->dset, sizeof(sets));
	sift_scube_init(a, a->shape);
	for (uint32 i = 0; i < ndim_all(a); i++)
		*dim_at(a, i) = keep[i];
	for (uint32 s = 0; s < a->shape.ndsets; s++)
		if ((a->dset[s] = sets[s]) != NULL)
			a->dset[s]->n = a->dset[s]->sorted = 0;
}

void
sift_scube_free(SiftScopeCubeAcc *a)
{
	for (uint32 i = 0; i < ndim_all(a); i++)
		sift_sdim_release(dim_at(a, i));
	for (uint32 s = 0; s < SIFT_SCOPE_DSETS_CAP; s++)
		if (a->dset[s] != NULL)
		{
			if (a->dset[s]->ids != NULL)
				pfree(a->dset[s]->ids);
			pfree(a->dset[s]);
		}
	memset(a->dim, 0, sizeof(a->dim));
	memset(a->rollup, 0, sizeof(a->rollup));
	memset(a->dset, 0, sizeof(a->dset));
}

void
sift_scube_copy(SiftScopeCubeAcc *dst, const SiftScopeCubeAcc *src)
{
	sort_dims(src);
	*dst = *src;
	for (uint32 i = 0; i < ndim_all(src); i++)
	{
		const SiftScopeDim *sd = dim_at(src, i);
		SiftScopeDim *dd = dim_at(dst, i);
		Size		n = (Size) sd->ngroups * sift_rec_size(&src->shape, sd->nk);

		dd->groups = NULL;
		dd->ht = NULL;
		dd->ht_cap = dd->cap = 0;
		if (n > 0)
		{
			dd->groups = palloc(n);
			dd->cap = sd->ngroups;
			memcpy(dd->groups, sd->groups, n);
		}
	}
	for (uint32 s = 0; s < SIFT_SCOPE_DSETS_CAP; s++)
	{
		uint32		n;
		const uint32 *ids = sift_scube_dset_ids(src, s, &n);

		dst->dset[s] = NULL;
		sift_scube_dset_append(dst, s, ids, n);
	}
}

Size
sift_scube_bytes(const SiftScopeCubeAcc *a)
{
	Size		n = sizeof(*a);

	for (uint32 i = 0; i < ndim_all(a); i++)
		n += (Size) dim_at(a, i)->cap * sift_rec_size(&a->shape, dim_at(a, i)->nk) + (Size) dim_at(a, i)->ht_cap * sizeof(uint16);
	for (uint32 s = 0; s < SIFT_SCOPE_DSETS_CAP; s++)
		if (a->dset[s] != NULL)
			n += sizeof(SiftScopeDSet) + (Size) a->dset[s]->cap * sizeof(uint32);
	return n;
}

/* ---------------------------------------------------------------------------
 * 2. The distinct sets
 * ------------------------------------------------------------------------- */

static void
dset_norm(SiftScopeDSet *s)
{
	uint32	   *merged;
	uint32		i = 0,
				j,
				u = 0;

	if (s->sorted == s->n)
		return;
	qsort(s->ids + s->sorted, s->n - s->sorted, sizeof(uint32), sift_cmp_u32);
	merged = palloc(sizeof(uint32) * s->n);
	j = s->sorted;
	while (i < s->sorted || j < s->n)
	{
		uint32		v = (j == s->n || (i < s->sorted && s->ids[i] <= s->ids[j])) ? s->ids[i++] : s->ids[j++];

		if (u == 0 || merged[u - 1] != v)
			merged[u++] = v;
	}
	memcpy(s->ids, merged, sizeof(uint32) * u);
	pfree(merged);
	s->n = s->sorted = u;
}

void
sift_scube_dset_append(SiftScopeCubeAcc *a, uint32 d, const uint32 *ids, uint32 n)
{
	SiftScopeDSet *s = a->dset[d];

	if (n == 0)
		return;
	if (s == NULL)
		s = a->dset[d] = palloc0(sizeof(*s));
	if (s->n + n > s->cap)
	{
		s->cap = Max(Max(s->cap * 2, 16), s->n + n);
		s->ids = s->ids == NULL ? palloc(sizeof(uint32) * s->cap) : repalloc(s->ids, sizeof(uint32) * s->cap);
	}
	memcpy(s->ids + s->n, ids, sizeof(uint32) * n);
	s->n += n;
	if (s->n - s->sorted > Max(64, s->sorted))
		dset_norm(s);
}

const uint32 *
sift_scube_dset_ids(const SiftScopeCubeAcc *a, uint32 d, uint32 *n)
{
	SiftScopeDSet *s = a->dset[d];

	if (s == NULL)
	{
		*n = 0;
		return NULL;
	}
	dset_norm(s);
	*n = s->n;
	return s->ids;
}

uint32
sift_scube_distinct(const SiftScopeCubeAcc *a, uint32 d)
{
	uint32		n;

	(void) sift_scube_dset_ids(a, d, &n);
	return n;
}

/* ---------------------------------------------------------------------------
 * 3. Groups
 * ------------------------------------------------------------------------- */

static inline int64
mapped_key(const SiftScopeDictMap *map, uint32 d, int64 v)
{
	if (map == NULL || map->map[d] == NULL)
		return v;
	SIFT_INVARIANT(v >= 0 && (uint64) v < map->nmap[d], "scope cube: dictionary code %lld outside its part dictionary", (long long) v);
	return map->map[d][v];
}

/* One row into a group record of histogram d */
static void
group_note(const SiftScopeCubeAcc *a, char *rec, uint32 nk, const SiftScopeRows *r, uint32 row, const bool *valid)
{
	SiftScopeGroupM *gm = sift_rec_m(rec, nk);

	(*sift_rec_cnt(rec, nk))++;
	for (uint32 m = 0; m < a->shape.nmeas; m++)
	{
		int64		v;
		bool		first;

		if (!valid[m])
			continue;
		v = r->meas[m][row];
		first = gm[m].nn == 0;
		gm[m].nn++;
		if (is_float(a, m))
			gm[m].sum_f += bits_double(v);
		else
			gm[m].sum += v;
		if (a->shape.minmax)
		{
			if (first || meas_less(a, m, v, gm[m].min))
				gm[m].min = v;
			if (first || meas_less(a, m, gm[m].max, v))
				gm[m].max = v;
		}
	}
}

void
sift_scube_add_row(SiftScopeCubeAcc *a, const SiftScopeRows *r, uint32 row, const SiftScopeDictMap *map)
{
	SiftScopeShape s = a->shape;
	bool		valid[SIFT_SCOPE_MEAS_CAP];

	for (uint32 m = 0; m < s.nmeas; m++)
	{
		SiftScopeLeafAgg *l = &a->leaf[m];

		valid[m] = r->meas_null[m] == NULL || !r->meas_null[m][row];
		if (!valid[m])
		{
			l->nnull++;
			continue;
		}
		{
			int64		v = r->meas[m][row];
			bool		first = a->row_count == l->nnull;

			if (is_float(a, m))
				l->sum_f += bits_double(v);
			else
			{
				SiftCI128	sq = (SiftCI128) v * v;

				l->sum += v;
				if (__builtin_add_overflow(l->sumsq, sq, &l->sumsq))
					l->zflags |= SIFT_SLA_SUMSQ_OVF;
			}
			if (first || meas_less(a, m, v, l->min))
				l->min = v;
			if (first || meas_less(a, m, l->max, v))
				l->max = v;
		}
	}
	if (r->time != NULL && (r->time_null == NULL || !r->time_null[row]))
	{
		int64		t = r->time[row];

		if (!a->has_time || t < a->time_min)
			a->time_min = t;
		if (!a->has_time || t > a->time_max)
			a->time_max = t;
		a->has_time = true;
	}
	for (uint32 i = 0; i < ndim_all(a); i++)
	{
		SiftScopeDim *d = dim_at(a, i);
		int64		key[SIFT_SCOPE_RDIMS_CAP];
		char	   *rec;

		for (uint32 j = 0; j < d->nk; j++)
		{
			uint32		cd = comp_dim(a, i, j);

			key[j] = mapped_key(map, cd, r->dim[cd][row]);
		}
		if ((rec = sift_sdim_group_for(&a->shape, d, key)) != NULL)
			group_note(a, rec, d->nk, r, row, valid);
	}
	for (uint32 d = 0; d < s.ndsets; d++)
		if (r->dset[d] != NULL && (r->dset_null[d] == NULL || !r->dset_null[d][row]))
			sift_scube_dset_append(a, d, &r->dset[d][row], 1);
	a->row_count++;
}

void
sift_scube_add_rows(SiftScopeCubeAcc *a, const SiftScopeRows *r, const uint32 *sel, uint32 nsel, const SiftScopeDictMap *map)
{
	for (uint32 i = 0; i < nsel; i++)
		sift_scube_add_row(a, r, sel == NULL ? i : sel[i], map);
}

/* ---------------------------------------------------------------------------
 * 4. Merge and subtract
 * ------------------------------------------------------------------------- */

static bool
dim_remapped(const SiftScopeCubeAcc *a, uint32 i, const SiftScopeDictMap *map)
{
	for (uint32 j = 0; map != NULL && j < dim_at(a, i)->nk; j++)
		if (map->map[comp_dim(a, i, j)] != NULL)
			return true;
	return false;
}

static void
group_merge(const SiftScopeCubeAcc *a, char *drec, const char *srec, uint32 nk)
{
	SiftScopeGroupM *dm = sift_rec_m(drec, nk);
	const SiftScopeGroupM *sm = sift_rec_m((char *) srec, nk);

	*sift_rec_cnt(drec, nk) += *sift_rec_cnt((char *) srec, nk);
	for (uint32 m = 0; m < a->shape.nmeas; m++)
	{
		if (a->shape.minmax && sm[m].nn > 0)
		{
			if (dm[m].nn == 0 || meas_less(a, m, sm[m].min, dm[m].min))
				dm[m].min = sm[m].min;
			if (dm[m].nn == 0 || meas_less(a, m, dm[m].max, sm[m].max))
				dm[m].max = sm[m].max;
		}
		dm[m].nn += sm[m].nn;
		dm[m].sum += sm[m].sum;
		dm[m].sum_f += sm[m].sum_f;
	}
}

void
sift_scube_merge(SiftScopeCubeAcc *dst, const SiftScopeCubeAcc *src, const SiftScopeDictMap *map)
{
	SiftScopeShape s = dst->shape;

	SIFT_INVARIANT(sift_scope_shape_equal(&s, &src->shape), "scope cube merge: shapes differ");
	for (uint32 m = 0; m < s.nmeas; m++)
	{
		SiftScopeLeafAgg *l = &dst->leaf[m];
		const SiftScopeLeafAgg *o = &src->leaf[m];
		bool		dst_has = dst->row_count > l->nnull,
					src_has = src->row_count > o->nnull;

		if (src_has && (!dst_has || meas_less(dst, m, o->min, l->min)))
			l->min = o->min;
		if (src_has && (!dst_has || meas_less(dst, m, l->max, o->max)))
			l->max = o->max;
		l->nnull += o->nnull;
		l->sum += o->sum;
		l->sum_f += o->sum_f;
		l->zflags |= o->zflags & SIFT_SLA_SUMSQ_OVF;
		if (__builtin_add_overflow(l->sumsq, o->sumsq, &l->sumsq))
			l->zflags |= SIFT_SLA_SUMSQ_OVF;
	}
	if (src->has_time)
	{
		if (!dst->has_time || src->time_min < dst->time_min)
			dst->time_min = src->time_min;
		if (!dst->has_time || src->time_max > dst->time_max)
			dst->time_max = src->time_max;
		dst->has_time = true;
	}
	dst->row_count += src->row_count;
	for (uint32 i = 0; i < ndim_all(dst); i++)
	{
		SiftScopeDim *dd = dim_at(dst, i);
		const SiftScopeDim *sd = dim_at(src, i);

		if (sd->overflow)
		{
			sift_sdim_overflow(dd);
			continue;
		}
		if (!dim_remapped(dst, i, map))
		{
			if (!dd->overflow)
				sift_sdim_merge(dst, dd, (SiftScopeDim *) sd, group_merge);
			continue;
		}
		for (uint32 g = 0; g < sd->ngroups; g++)
		{
			char	   *srec = sift_dim_rec(&src->shape, sd, g),
					   *drec;
			int64		key[SIFT_SCOPE_RDIMS_CAP];

			for (uint32 j = 0; j < sd->nk; j++)
				key[j] = mapped_key(map, comp_dim(dst, i, j), sift_rec_key(srec)[j]);
			if ((drec = sift_sdim_group_for(&dst->shape, dd, key)) == NULL)
				break;
			group_merge(dst, drec, srec, sd->nk);
		}
	}
	for (uint32 d = 0; d < s.ndsets; d++)
		if (src->dset[d] != NULL)
		{
			uint32		n;
			const uint32 *ids = sift_scube_dset_ids(src, d, &n);

			sift_scube_dset_append(dst, d, ids, n);
		}
}

/* A float sum that a subtraction took down to a small share of what it was, or to no number: the digits that are left are the rounding of the ones that went */
static bool
float_cancelled(double gross, double net)
{
	return isnan(net) || isinf(net) || fabs(net) < fabs(gross) / 64.0;
}

static uint32
group_sub(const SiftScopeCubeAcc *a, char *drec, const char *srec, uint32 nk)
{
	SiftScopeGroupM *dm = sift_rec_m(drec, nk);
	const SiftScopeGroupM *sm = sift_rec_m((char *) srec, nk);
	uint32		mask = 0;

	*sift_rec_cnt(drec, nk) -= *sift_rec_cnt((char *) srec, nk);
	for (uint32 m = 0; m < a->shape.nmeas; m++)
	{
		double		gross = dm[m].sum_f;

		if (a->shape.minmax && sm[m].nn > 0 && (meas_cmp(a, m, sm[m].min, dm[m].min) == 0 || meas_cmp(a, m, sm[m].max, dm[m].max) == 0))
			mask |= SIFT_SUB_BOUNDS;
		dm[m].nn -= sm[m].nn;
		dm[m].sum -= sm[m].sum;
		dm[m].sum_f -= sm[m].sum_f;
		if (is_float(a, m) && sm[m].nn > 0 && *sift_rec_cnt(drec, nk) > 0 && float_cancelled(gross, dm[m].sum_f))
			mask |= SIFT_SUB_FLOAT;
	}
	return mask;
}

uint32
sift_scube_sub(SiftScopeCubeAcc *dst, const SiftScopeCubeAcc *src)
{
	SiftScopeShape s = dst->shape;
	uint32		mask = 0;

	SIFT_INVARIANT(dst->row_count >= src->row_count, "scope cube subtract: more rows out than in");
	SIFT_INVARIANT(sift_scope_shape_equal(&s, &src->shape), "scope cube subtract: shapes differ");
	for (uint32 m = 0; m < s.nmeas; m++)
	{
		SiftScopeLeafAgg *l = &dst->leaf[m];
		const SiftScopeLeafAgg *o = &src->leaf[m];
		double		gross = l->sum_f;
		bool		has = src->row_count > o->nnull;

		if (has && (meas_cmp(dst, m, o->min, l->min) == 0 || meas_cmp(dst, m, o->max, l->max) == 0))
			mask |= SIFT_SUB_BOUNDS;
		l->nnull -= o->nnull;
		l->sum -= o->sum;
		l->sum_f -= o->sum_f;
		l->sumsq -= o->sumsq;
		l->zflags |= o->zflags & SIFT_SLA_SUMSQ_OVF;
		if (has && is_float(dst, m) && float_cancelled(gross, l->sum_f))
			mask |= SIFT_SUB_FLOAT;
	}
	if (src->has_time && dst->has_time && (src->time_min == dst->time_min || src->time_max == dst->time_max))
		mask |= SIFT_SUB_BOUNDS;
	dst->row_count -= src->row_count;
	for (uint32 i = 0; i < ndim_all(dst); i++)
	{
		SiftScopeDim *dd = dim_at(dst, i);
		const SiftScopeDim *sd = dim_at(src, i);

		if (dd->overflow)
			continue;
		if (sd->overflow)
			sift_sdim_overflow(dd);
		else
			mask |= sift_sdim_sub(dst, dd, (SiftScopeDim *) sd, group_sub);
	}
	for (uint32 d = 0; d < s.ndsets; d++)
		if (src->dset[d] != NULL && src->dset[d]->n > 0)
			mask |= SIFT_SUB_DSETS;
	return mask;
}

/* ---------------------------------------------------------------------------
 * 5. Comparison and lookups
 * ------------------------------------------------------------------------- */

static bool
group_same(const SiftScopeCubeAcc *a, const char *x, const char *y, uint32 nk, uint32 ignore)
{
	const SiftScopeGroupM *mx = sift_rec_m((char *) x, nk),
			   *my = sift_rec_m((char *) y, nk);

	if (memcmp(x, y, sizeof(int64) * nk) != 0 || *sift_rec_cnt((char *) x, nk) != *sift_rec_cnt((char *) y, nk))
		return false;
	for (uint32 m = 0; m < a->shape.nmeas; m++)
	{
		if (mx[m].nn != my[m].nn || mx[m].sum != my[m].sum || sift_f8_cmp(mx[m].sum_f, my[m].sum_f) != 0)
			return false;
		if (a->shape.minmax && !(ignore & SIFT_SAME_EXTREMES) && mx[m].nn > 0 &&
			(meas_cmp(a, m, mx[m].min, my[m].min) != 0 || meas_cmp(a, m, mx[m].max, my[m].max) != 0))
			return false;
	}
	return true;
}

bool
sift_scube_same(const SiftScopeCubeAcc *a, const SiftScopeCubeAcc *b, uint32 ignore)
{
	SiftScopeShape s = a->shape;

	if (!sift_scope_shape_equal(&s, &b->shape) || a->row_count != b->row_count || (!(ignore & SIFT_SAME_TIME) && a->has_time != b->has_time))
		return false;
	sort_dims(a);
	sort_dims(b);
	if (!(ignore & SIFT_SAME_TIME) && a->has_time && (a->time_min != b->time_min || a->time_max != b->time_max))
		return false;
	for (uint32 m = 0; m < s.nmeas; m++)
	{
		const SiftScopeLeafAgg *x = &a->leaf[m],
				   *y = &b->leaf[m];
		bool		has = a->row_count > x->nnull;

		if (x->nnull != y->nnull || x->sum != y->sum || sift_f8_cmp(x->sum_f, y->sum_f) != 0)
			return false;
		if (has && !(ignore & SIFT_SAME_EXTREMES) && (meas_cmp(a, m, x->min, y->min) != 0 || meas_cmp(a, m, x->max, y->max) != 0))
			return false;
	}
	for (uint32 i = 0; i < ndim_all(a); i++)
	{
		const SiftScopeDim *x = dim_at(a, i),
				   *y = dim_at(b, i);

		if (x->overflow != y->overflow || x->ngroups != y->ngroups)
			return false;
		for (uint32 g = 0; g < x->ngroups; g++)
			if (!group_same(a, sift_dim_rec(&s, x, g), sift_dim_rec(&s, y, g), x->nk, ignore))
				return false;
	}
	if (!(ignore & SIFT_SAME_DSETS))
		for (uint32 d = 0; d < s.ndsets; d++)
		{
			uint32		nx,
						ny;
			const uint32 *ix = sift_scube_dset_ids(a, d, &nx),
					   *iy = sift_scube_dset_ids(b, d, &ny);

			if (nx != ny || (nx > 0 && memcmp(ix, iy, sizeof(uint32) * nx) != 0))
				return false;
		}
	return true;
}

bool
sift_scube_equal(const SiftScopeCubeAcc *a, const SiftScopeCubeAcc *b)
{
	return sift_scube_same(a, b, 0);
}

const SiftScopeGroup *
sift_scube_group(const SiftScopeCubeAcc *a, uint32 dim, int64 key)
{
	bool		found;
	uint32		at;

	sift_sdim_sort(&a->shape, (SiftScopeDim *) &a->dim[dim]);
	at = sift_sdim_search(&a->shape, &a->dim[dim], &key, &found);
	return found ? (const SiftScopeGroup *) sift_dim_rec(&a->shape, &a->dim[dim], at) : NULL;
}

const SiftScopeGroup *
sift_scube_group_at(const SiftScopeCubeAcc *a, uint32 dim, uint32 i)
{
	sift_sdim_sort(&a->shape, (SiftScopeDim *) &a->dim[dim]);
	return (const SiftScopeGroup *) sift_dim_rec(&a->shape, &a->dim[dim], i);
}

uint32
sift_scube_rollup_cells(const SiftScopeCubeAcc *a, uint32 r)
{
	return a->rollup[r].ngroups;
}

uint64
sift_scube_rollup_rows(const SiftScopeCubeAcc *a, uint32 r, const int64 *tuple)
{
	bool		found;
	uint32		at;

	sift_sdim_sort(&a->shape, (SiftScopeDim *) &a->rollup[r]);
	at = sift_sdim_search(&a->shape, &a->rollup[r], tuple, &found);
	return found ? *sift_rec_cnt(sift_dim_rec(&a->shape, &a->rollup[r], at), a->rollup[r].nk) : 0;
}

/* ---------------------------------------------------------------------------
 * 6. Month table
 * ------------------------------------------------------------------------- */

#define SMONTH_SORTED_MAX 16

void
sift_smonth_init(SiftScopeMonthTable *t)
{
	memset(t, 0, sizeof(*t));
}

void
sift_smonth_clear(SiftScopeMonthTable *t)
{
	for (uint32 i = 0; i < t->n; i++)
		sift_scube_free(&t->e[i].cube);
	t->n = t->last = 0;
	t->unsorted = false;
	t->bytes = (Size) t->cap * sizeof(*t->e);
}

void
sift_smonth_free(SiftScopeMonthTable *t)
{
	sift_smonth_clear(t);
	if (t->e != NULL)
		pfree(t->e);
	if (t->ht != NULL)
		pfree(t->ht);
	memset(t, 0, sizeof(*t));
}

static int
month_cmp(const void *x, const void *y)
{
	return ((const SiftScopeMonthEntry *) x)->month_id < ((const SiftScopeMonthEntry *) y)->month_id ? -1 : 1;
}

/* An appended-to table is put in month order before anything reads it in order: no change to what it holds */
void
sift_smonth_sort(const SiftScopeMonthTable *ct)
{
	SiftScopeMonthTable *t = (SiftScopeMonthTable *) ct;

	if (!t->unsorted)
		return;
	qsort(t->e, t->n, sizeof(*t->e), month_cmp);
	t->unsorted = false;
	t->last = 0;
}

static uint32
month_search(const SiftScopeMonthTable *t, int32 month_id, bool *found)
{
	uint32		lo = 0,
				hi = t->n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (t->e[mid].month_id == month_id)
		{
			*found = true;
			return mid;
		}
		if (t->e[mid].month_id < month_id)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = false;
	return lo;
}

/* The cube of a month, empty when it is new: the last one hit, else a binary search (ascending) or the index (appended); a month out of order among many is appended */
SiftScopeCubeAcc *
sift_smonth_get(SiftScopeMonthTable *t, int32 month_id, SiftScopeShape shape)
{
	bool		found = false;
	uint32		at;

	if (t->last < t->n && t->e[t->last].month_id == month_id)
		return &t->e[t->last].cube;
	if (t->unsorted)
	{
		int32		i = sift_idix_find(t->ht, t->ht_cap, t->e, sizeof(*t->e), month_id);

		found = i >= 0;
		at = found ? (uint32) i : t->n;
	}
	else
		at = month_search(t, month_id, &found);
	if (found)
	{
		t->last = at;
		return &t->e[at].cube;
	}
	if (t->n == t->cap)
	{
		Size		old = (Size) t->cap * sizeof(*t->e);

		t->cap = t->cap == 0 ? 8 : t->cap * 2;
		t->e = t->e == NULL ? palloc(t->cap * sizeof(*t->e)) : repalloc(t->e, t->cap * sizeof(*t->e));
		t->bytes += (Size) t->cap * sizeof(*t->e) - old;
	}
	if (!t->unsorted && at < t->n && t->n >= SMONTH_SORTED_MAX)
	{
		t->unsorted = true;
		sift_idix_rebuild(&t->ht, &t->ht_cap, t->e, sizeof(*t->e), t->n);
		at = t->n;
	}
	if (!t->unsorted)
		memmove(&t->e[at + 1], &t->e[at], (t->n - at) * sizeof(*t->e));
	t->n++;
	t->e[at].month_id = month_id;
	sift_scube_init(&t->e[at].cube, shape);
	t->bytes += sift_scube_bytes(&t->e[at].cube);
	if (t->unsorted && t->n * 2 > t->ht_cap)
		sift_idix_rebuild(&t->ht, &t->ht_cap, t->e, sizeof(*t->e), t->n);
	else if (t->unsorted)
		sift_idix_note(t->ht, t->ht_cap, t->e, sizeof(*t->e), at);
	t->last = at;
	return &t->e[at].cube;
}

const SiftScopeCubeAcc *
sift_smonth_find(const SiftScopeMonthTable *t, int32 month_id)
{
	bool		found;
	uint32		at;

	sift_smonth_sort(t);
	at = month_search(t, month_id, &found);
	return found ? &t->e[at].cube : NULL;
}

void
sift_smonth_add_rows(SiftScopeMonthTable *t, SiftScopeShape shape, const SiftScopeRows *r, const uint32 *sel, uint32 nsel, const SiftScopeDictMap *map)
{
	if (r->time == NULL)
		return;
	for (uint32 i = 0; i < nsel; i++)
	{
		uint32		row = sel == NULL ? i : sel[i];
		SiftScopeCubeAcc *c;
		Size		before;

		if (r->time_null != NULL && r->time_null[row])
			continue;
		c = sift_smonth_get(t, sift_scope_month_of(r->time[row]), shape);
		before = sift_scube_bytes(c);
		sift_scube_add_row(c, r, row, map);
		t->bytes += sift_scube_bytes(c) - before;
	}
}

void
sift_smonth_merge_cube(SiftScopeMonthTable *t, SiftScopeShape shape, int32 month_id, const SiftScopeCubeAcc *src, const SiftScopeDictMap *map)
{
	SiftScopeCubeAcc *c = sift_smonth_get(t, month_id, shape);
	Size		before = sift_scube_bytes(c);

	sift_scube_merge(c, src, map);
	t->bytes += sift_scube_bytes(c) - before;
}

void
sift_smonth_merge(SiftScopeMonthTable *dst, SiftScopeShape shape, const SiftScopeMonthTable *src, const SiftScopeDictMap *map)
{
	for (uint32 i = 0; i < src->n; i++)
		sift_smonth_merge_cube(dst, shape, src->e[i].month_id, &src->e[i].cube, map);
}

Size
sift_smonth_bytes(const SiftScopeMonthTable *t)
{
	return t->bytes + (Size) t->ht_cap * sizeof(uint16);
}

/* What sift_smonth_bytes must equal: the sum over the cubes, for the tests of the accounting */
Size
sift_smonth_bytes_sum(const SiftScopeMonthTable *t)
{
	Size		n = (Size) t->cap * sizeof(*t->e) + (Size) t->ht_cap * sizeof(uint16);

	for (uint32 i = 0; i < t->n; i++)
		n += sift_scube_bytes(&t->e[i].cube);
	return n;
}
