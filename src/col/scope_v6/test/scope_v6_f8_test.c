/*
 * col/test/scope_v6_f8_test.c - the scenarios of sift._test_scope_v6 (scope_v6_test.c) about what a value or a zone does to a cube: the float8 measures
 * with NaN, infinities and signed zeros (minimum, maximum, sum and average of cubes built, merged, copied through bytes and subtracted, against
 * PostgreSQL's own float8 order), and the month cells of timestamptz under UTC and other session zones (a local month answered from cells and
 * boundary scans against a count of the rows by their local month).
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few hundred rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "datatype/timestamp.h"
#include "pgtime.h"
#include "utils/float.h"

#include "agg/zoneoff.h"
#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/test/scope_v6_world.h"

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

extern void scope_v6_scenario_f8(TCheck *t);
extern void scope_v6_scenario_monthtz(TCheck *t);

/* ---------------------------------------------------------------------------
 * f8: NaN, infinities and signed zeros in the extremes and sums of a cube
 * ------------------------------------------------------------------------- */

#define F8_NAN	1
#define F8_INF	2
#define F8_ZERO	4
#define F8_FIN	8
#define F8_NMODES 8
#define F8_NKEYS 4

static const uint32 f8_modes[F8_NMODES - 1] = {F8_NAN, F8_NAN | F8_FIN, F8_INF | F8_FIN, F8_INF | F8_NAN, F8_ZERO, F8_ZERO | F8_FIN,
	F8_NAN | F8_INF | F8_ZERO | F8_FIN};
static const uint32 f8_key_pools[F8_NKEYS] = {F8_NAN, F8_FIN, F8_NAN | F8_INF | F8_FIN, F8_ZERO};

typedef struct F8Rows
{
	uint32		n;
	double	   *v;
	int64	   *bits;
	int64	   *key;
	uint8	   *nul;
	SiftScopeRows r;
} F8Rows;

static double
f8_value(TCheck *t, uint32 pools)
{
	static const uint64 nans[] = {UINT64CONST(0x7FF8000000000000), UINT64CONST(0xFFF8000000000000), UINT64CONST(0x7FF8000000000001),
		UINT64CONST(0x7FF0000000000001)};
	uint32		set[4],
				nset = 0;
	double		d;

	for (uint32 b = 0; b < 4; b++)
		if ((pools >> b) & 1)
			set[nset++] = 1U << b;
	switch (set[TRAND(t, nset)])
	{
		case F8_NAN:
			{
				uint64		bits = nans[TRAND(t, lengthof(nans))];

				memcpy(&d, &bits, sizeof(d));
				return d;
			}
		case F8_INF:
			return TRAND(t, 2) ? INFINITY : -INFINITY;
		case F8_ZERO:
			return TRAND(t, 2) ? -0.0 : 0.0;
		default:
			return (double) ((int) TRAND(t, 41) - 20);
	}
}

static void
f8_gen(TCheck *t, F8Rows *f, uint32 n, uint32 mode)
{
	f->n = n;
	f->v = palloc(sizeof(double) * n);
	f->bits = palloc(sizeof(int64) * n);
	f->key = palloc(sizeof(int64) * n);
	f->nul = palloc(sizeof(uint8) * n);
	for (uint32 i = 0; i < n; i++)
	{
		f->key[i] = TRAND(t, F8_NKEYS);
		f->nul[i] = TRAND(t, 8) == 0;
		f->v[i] = f8_value(t, mode == F8_NMODES - 1 ? f8_key_pools[f->key[i]] : f8_modes[mode]);
		memcpy(&f->bits[i], &f->v[i], sizeof(double));
	}
	memset(&f->r, 0, sizeof(f->r));
	f->r.n = n;
	f->r.dim[0] = f->key;
	f->r.meas[0] = f->bits;
	f->r.meas_null[0] = f->nul;
}

typedef struct F8Truth
{
	uint64		rows;
	uint64		nn;
	double		min;
	double		max;
	double		sum;
} F8Truth;

/* PostgreSQL's MIN, MAX and SUM of the non-NULL values of rows [from, to) with key (any key: -1) */
static F8Truth
f8_truth(const F8Rows *f, uint32 from, uint32 to, int64 key)
{
	F8Truth		o = {0};

	for (uint32 i = from; i < to; i++)
	{
		if (key >= 0 && f->key[i] != key)
			continue;
		o.rows++;
		if (f->nul[i])
			continue;
		if (o.nn == 0 || float8_cmp_internal(f->v[i], o.min) < 0)
			o.min = f->v[i];
		if (o.nn == 0 || float8_cmp_internal(f->v[i], o.max) > 0)
			o.max = f->v[i];
		o.sum += f->v[i];
		o.nn++;
	}
	return o;
}

static bool
f8_same(double x, double y)
{
	return float8_cmp_internal(x, y) == 0;
}

static double
bits_f8(int64 v)
{
	double		d;

	memcpy(&d, &v, sizeof(d));
	return d;
}

static bool
f8_agrees(const F8Truth *w, uint64 nn, int64 min, int64 max, double sum, bool ext, bool sums)
{
	if (nn != w->nn)
		return false;
	if (w->nn == 0)
		return true;
	return (!ext || (f8_same(bits_f8(min), w->min) && f8_same(bits_f8(max), w->max))) &&
		(!sums || (f8_same(sum, w->sum) && f8_same(sum / (double) nn, w->sum / (double) w->nn)));
}

/* The cube of rows [from, to) against the truth: the leaf and every group of the dim; the extremes and the sums as asked (a subtraction may not keep them) */
static void
f8_check(TCheck *t, SiftScopeCubeAcc *c, const F8Rows *f, uint32 from, uint32 to, bool ext, bool sums, const char *what, int round)
{
	F8Truth		all = f8_truth(f, from, to, -1);
	const SiftScopeLeafAgg *l = &c->leaf[0];

	TCHK(t, c->row_count == all.rows, "round %d %s: %llu rows, not %llu", round, what, (unsigned long long) c->row_count, (unsigned long long) all.rows);
	TCHK(t, f8_agrees(&all, c->row_count - l->nnull, l->min, l->max, l->sum_f, ext, sums),
		 "round %d %s: the leaf has min %g max %g sum %g of %llu values, the rows min %g max %g sum %g of %llu", round, what, bits_f8(l->min), bits_f8(l->max), l->sum_f,
		 (unsigned long long) (c->row_count - l->nnull), all.min, all.max, all.sum, (unsigned long long) all.nn);
	for (int64 k = 0; k < F8_NKEYS; k++)
	{
		F8Truth		w = f8_truth(f, from, to, k);
		const SiftScopeGroup *g = sift_scube_group(c, 0, k);

		TCHK(t, (g != NULL) == (w.rows > 0) && (g == NULL || g->cnt == w.rows), "round %d %s: the group of key %lld has the wrong rows", round, what, (long long) k);
		if (g != NULL && w.rows > 0)
			TCHK(t, f8_agrees(&w, g->m[0].nn, g->m[0].min, g->m[0].max, g->m[0].sum_f, ext, sums),
				 "round %d %s: key %lld has min %g max %g sum %g of %llu values, the rows min %g max %g sum %g of %llu", round, what, (long long) k, bits_f8(g->m[0].min),
				 bits_f8(g->m[0].max), g->m[0].sum_f, (unsigned long long) g->m[0].nn, w.min, w.max, w.sum, (unsigned long long) w.nn);
	}
}

static void
f8_round(TCheck *t, int round, uint32 mode, int64 *exact)
{
	SiftScopeShape shape = {.nmeas = 1, .ndims = 1, .float_meas = 1, .minmax = true};
	uint32		n1 = TRAND(t, 30),
				n2 = TRAND(t, 30),
				n = n1 + n2;
	uint32	   *sel = palloc(sizeof(uint32) * (n2 + 1));
	F8Rows		f;
	SiftScopeCubeAcc a,
				b,
				ab,
				c,
				back;
	StringInfoData buf;
	uint32		mask;

	f8_gen(t, &f, n, mode);
	for (uint32 i = 0; i < n2; i++)
		sel[i] = n1 + i;
	sift_scube_init(&a, shape);
	sift_scube_init(&b, shape);
	sift_scube_init(&ab, shape);
	sift_scube_add_rows(&a, &f.r, NULL, n1, NULL);
	sift_scube_add_rows(&b, &f.r, sel, n2, NULL);
	sift_scube_add_rows(&ab, &f.r, NULL, n, NULL);
	f8_check(t, &a, &f, 0, n1, true, true, "built a", round);
	f8_check(t, &b, &f, n1, n, true, true, "built b", round);
	f8_check(t, &ab, &f, 0, n, true, true, "built a+b", round);

	sift_scube_copy(&c, &a);
	sift_scube_merge(&c, &b, NULL);
	f8_check(t, &c, &f, 0, n, true, true, "merged", round);
	TCHK(t, sift_scube_equal(&c, &ab), "round %d: merge(A, B) is not the cube of both", round);
	TCHK(t, sift_scube_equal(&ab, &ab), "round %d: a cube is not equal to itself", round);

	initStringInfo(&buf);
	sift_scube_write(&buf, &ab);
	TCHK(t, sift_scube_read(buf.data, buf.len, &back) && sift_scube_equal(&back, &ab), "round %d: the bytes of a cube do not read back", round);
	f8_check(t, &back, &f, 0, n, true, true, "read back", round);

	sift_scube_copy(&back, &ab);
	mask = sift_scube_sub(&back, &b);
	if (mask == 0)
	{
		TCHK(t, sift_scube_equal(&back, &a), "round %d: merge then subtract is not the first cube", round);
		(*exact)++;
	}
	f8_check(t, &back, &f, 0, n1, !(mask & SIFT_SUB_BOUNDS), !(mask & SIFT_SUB_FLOAT), "subtracted", round);
}

typedef struct F8Known
{
	double		v[3];
	uint32		n;
	double		min;
	double		max;
	double		sum;
} F8Known;

/* The cases PostgreSQL answers by its float8 order: NaN above +Infinity, -0.0 equal to 0.0 */
static void
f8_known(TCheck *t)
{
	static const F8Known cases[] = {
		{{1.0, NAN}, 2, 1.0, NAN, NAN},
		{{NAN, 1.0}, 2, 1.0, NAN, NAN},
		{{NAN, NAN}, 2, NAN, NAN, NAN},
		{{INFINITY, NAN}, 2, INFINITY, NAN, NAN},
		{{NAN, -INFINITY}, 2, -INFINITY, NAN, NAN},
		{{-INFINITY, 1.0, INFINITY}, 3, -INFINITY, INFINITY, NAN},
		{{INFINITY, 1.0}, 2, 1.0, INFINITY, INFINITY},
		{{-0.0, 0.0}, 2, 0.0, 0.0, 0.0},
		{{0.0, -0.0}, 2, -0.0, -0.0, 0.0},
		{{-0.0, -1.0, 1.0}, 3, -1.0, 1.0, 0.0},
		{{1.0, 3.0}, 2, 1.0, 3.0, 4.0},
	};
	SiftScopeShape shape = {.nmeas = 1, .ndims = 1, .float_meas = 1, .minmax = true};

	for (Size i = 0; i < lengthof(cases); i++)
	{
		F8Rows		f;
		SiftScopeCubeAcc c,
					rev;
		const SiftScopeGroup *g;

		f8_gen(t, &f, cases[i].n, 0);
		for (uint32 j = 0; j < cases[i].n; j++)
		{
			f.v[j] = cases[i].v[j];
			memcpy(&f.bits[j], &f.v[j], sizeof(double));
			f.key[j] = 0;
			f.nul[j] = 0;
		}
		sift_scube_init(&c, shape);
		sift_scube_add_rows(&c, &f.r, NULL, cases[i].n, NULL);
		g = sift_scube_group(&c, 0, 0);
		TCHK(t, f8_same(bits_f8(c.leaf[0].min), cases[i].min) && f8_same(bits_f8(c.leaf[0].max), cases[i].max) && f8_same(c.leaf[0].sum_f, cases[i].sum),
			 "known case %zu: the leaf has min %g max %g sum %g", i, bits_f8(c.leaf[0].min), bits_f8(c.leaf[0].max), c.leaf[0].sum_f);
		TCHK(t, g != NULL && f8_same(bits_f8(g->m[0].min), cases[i].min) && f8_same(bits_f8(g->m[0].max), cases[i].max) && f8_same(g->m[0].sum_f, cases[i].sum),
			 "known case %zu: the group has min %g max %g sum %g", i, bits_f8(g->m[0].min), bits_f8(g->m[0].max), g->m[0].sum_f);
		TCHK(t, f8_same(c.leaf[0].sum_f / (double) cases[i].n, cases[i].sum / (double) cases[i].n), "known case %zu: the average", i);
		for (uint32 j = 0; j < cases[i].n / 2; j++)
		{
			double		x = f.v[j];
			int64		bx = f.bits[j];

			f.v[j] = f.v[cases[i].n - 1 - j];
			f.bits[j] = f.bits[cases[i].n - 1 - j];
			f.v[cases[i].n - 1 - j] = x;
			f.bits[cases[i].n - 1 - j] = bx;
		}
		sift_scube_init(&rev, shape);
		sift_scube_add_rows(&rev, &f.r, NULL, cases[i].n, NULL);
		TCHK(t, sift_scube_equal(&c, &rev), "known case %zu: the cube depends on the order of the rows", i);
	}
}

void
scope_v6_scenario_f8(TCheck *t)
{
	int64		exact = 0;
	int			rounds = 16 * F8_NMODES;

	f8_known(t);
	for (int round = 0; round < rounds; round++)
		f8_round(t, round, round % F8_NMODES, &exact);
	OUTL(t, "f8 rounds=%d exact_subtracts=%lld", rounds, (long long) exact);
}

/* ---------------------------------------------------------------------------
 * monthtz: the month cells of a timestamptz under a session zone
 * ------------------------------------------------------------------------- */

#define SECS_1970_TO_2000	946684800
#define NZONES				16

static const char *const zone_names[NZONES - 1] = {"UTC", "GMT", "Etc/UTC", "Etc/GMT", "Etc/GMT+5", "Etc/GMT-14", "America/New_York", "America/Sao_Paulo",
	"Asia/Kolkata", "Pacific/Chatham", "Europe/London", "Africa/Abidjan", "Asia/Kathmandu", "Australia/Lord_Howe", "PST8PDT"};

static int32
local_month(const pg_tz *tz, int64 ts)
{
	pg_time_t	s = ts / USECS_PER_SEC - (ts % USECS_PER_SEC < 0) + SECS_1970_TO_2000;
	struct pg_tm *tm = pg_localtime(&s, tz);

	return (int32) ((tm->tm_year + 1900 - 2000) * 12 + tm->tm_mon);
}

/* The first instant of local month l: the local month never goes down along the instants, the offset is under two days */
static int64
local_month_start(const pg_tz *tz, int32 l)
{
	int64		lo = sift_scope_month_start(l) - 2 * USECS_PER_DAY,
				hi = sift_scope_month_start(l) + 2 * USECS_PER_DAY;

	while (hi - lo > 1)
	{
		int64		mid = lo + (hi - lo) / 2;

		if (local_month(tz, mid) >= l)
			hi = mid;
		else
			lo = mid;
	}
	return hi;
}

/* Does the zone read UTC at every instant from 1786 to 2201? (every five days: a zone that differs for less is not a zone here) */
static bool
zone_always_utc(const pg_tz *tz)
{
	for (pg_time_t s = -INT64CONST(5800000000); s < INT64CONST(7300000000); s += 5 * 86400)
	{
		struct pg_tm *tm = pg_localtime(&s, tz);

		if (tm->tm_gmtoff != 0 || tm->tm_isdst)
			return false;
	}
	return true;
}

static int64
rows_between(const int64 *ts, uint32 n, int64 lo, int64 hi)
{
	int64		c = 0;

	for (uint32 i = 0; i < n; i++)
		c += ts[i] >= lo && ts[i] < hi;
	return c;
}

/* The rows of local month l counted as a reader does: from the cells when the zone is safe, else the interior cells and a scan of the rest */
static int64
month_via_cells(TCheck *t, const pg_tz *tz, const SiftScopeMonthTable *mt, const int64 *ts, uint32 n, int32 l)
{
	int64		lo = local_month_start(tz, l),
				hi = local_month_start(tz, l + 1),
				c = 0;
	int32		first,
				last;

	if (sift_scope_monthmap_tz_safe(true, zone_is_utc(tz)))
	{
		const SiftScopeCubeAcc *cell = sift_smonth_find(mt, l);

		TCHK(t, lo == sift_scope_month_start(l) && hi == sift_scope_month_end(l), "a UTC zone has a local month that is not a cell");
		return cell != NULL ? cell->row_count : 0;
	}
	if (!sift_scope_months_inside(lo, hi, &first, &last))
		return rows_between(ts, n, lo, hi);
	TCHK(t, sift_scope_month_start(first) >= lo && sift_scope_month_end(last) <= hi, "months %d..%d of [%lld, %lld) are not inside it", first, last, (long long) lo, (long long) hi);
	TCHK(t, sift_scope_month_start(first - 1) < lo && sift_scope_month_end(last + 1) > hi, "months %d..%d of [%lld, %lld) leave a month out", first, last, (long long) lo, (long long) hi);
	for (int32 m = first; m <= last; m++)
	{
		const SiftScopeCubeAcc *cell = sift_smonth_find(mt, m);

		c += cell != NULL ? cell->row_count : 0;
	}
	return c + rows_between(ts, n, lo, sift_scope_month_start(first)) + rows_between(ts, n, sift_scope_month_end(last), hi);
}

/* Instants of the months around l, with the hours around every boundary of a cell and of a local month: where a cell and a local month differ */
static uint32
month_instants(TCheck *t, const pg_tz *tz, int32 l, int64 *ts, uint32 max)
{
	static const int64 around[] = {-3600000000, -1, 0, 1, 3600000000};
	int64		from = sift_scope_month_start(l - 1),
				to = sift_scope_month_end(l + 1);
	uint32		n = 0;

	for (; n < 200; n++)
		ts[n] = from + (int64) (pg_prng_uint64(&t->rng) % (uint64) (to - from));
	for (int32 m = l - 1; m <= l + 2 && n + 2 * lengthof(around) <= max; m++)
		for (Size i = 0; i < lengthof(around); i++)
		{
			ts[n++] = sift_scope_month_start(m) + around[i];
			ts[n++] = local_month_start(tz, m) + around[i];
		}
	return n;
}

static void
monthtz_zone(TCheck *t, const pg_tz *tz, const char *name, int64 *wrong_naive, int64 *answers)
{
	bool		safe = sift_scope_monthmap_tz_safe(true, zone_is_utc(tz));
	int64		naive_wrong = 0;

	TCHK(t, zone_is_utc(tz) == zone_always_utc(tz), "zone %s: zone_is_utc says %d", name, zone_is_utc(tz));
	for (int i = 0; i < 12; i++)
	{
		int32		l = sift_scope_month_of(INT64CONST(86400000000) * 5479) + (int32) TRAND(t, 240);
		int64		ts[300];
		SiftScopeShape shape = {0};
		SiftScopeRows r = {0};
		SiftScopeMonthTable mt;
		uint32		n = month_instants(t, tz, l, ts, lengthof(ts));
		int64		truth = 0;
		const SiftScopeCubeAcc *naive;

		for (uint32 j = 0; j < n; j++)
			truth += local_month(tz, ts[j]) == l;
		r.n = n;
		r.time = ts;
		sift_smonth_init(&mt);
		sift_smonth_add_rows(&mt, shape, &r, NULL, n, NULL);
		TCHK(t, month_via_cells(t, tz, &mt, ts, n, l) == truth, "zone %s month %d: the cells and the scans do not count the rows of the local month", name, l);
		naive = sift_smonth_find(&mt, l);
		naive_wrong += (naive != NULL ? (int64) naive->row_count : 0) != truth;
		sift_smonth_free(&mt);
		(*answers)++;
	}
	TCHK(t, !safe || naive_wrong == 0, "zone %s: %lld local months differ from the cell of the same number", name, (long long) naive_wrong);
	*wrong_naive += naive_wrong;
}

static void
monthtz_inside(TCheck *t)
{
	int32		m = sift_scope_month_of(INT64CONST(86400000000) * 8825);
	int32		first,
				last;

	TCHK(t, sift_scope_months_inside(sift_scope_month_start(m), sift_scope_month_end(m), &first, &last) && first == m && last == m, "one whole month is inside itself");
	TCHK(t, !sift_scope_months_inside(sift_scope_month_start(m) + 1, sift_scope_month_end(m), &first, &last), "a month from its second microsecond is not inside");
	TCHK(t, !sift_scope_months_inside(sift_scope_month_start(m), sift_scope_month_end(m) - 1, &first, &last), "a month short of its last microsecond is not inside");
	TCHK(t, sift_scope_months_inside(sift_scope_month_start(m) - 1, sift_scope_month_end(m + 1) + 1, &first, &last) && first == m && last == m + 1, "two whole months inside a wider range");
	TCHK(t, !sift_scope_months_inside(sift_scope_month_start(m), sift_scope_month_start(m), &first, &last), "an empty range holds no month");
	TCHK(t, !sift_scope_months_inside(sift_scope_month_end(m), sift_scope_month_start(m), &first, &last), "a reversed range holds no month");
	TCHK(t, sift_scope_months_inside(MIN_TIMESTAMP, END_TIMESTAMP, &first, &last) && first == sift_scope_month_of(MIN_TIMESTAMP) + 1 &&
		 last == sift_scope_month_of(END_TIMESTAMP) - 1, "the whole range holds every month but its ends");
	TCHK(t, sift_scope_monthmap_tz_safe(false, false) && sift_scope_monthmap_tz_safe(false, true) && sift_scope_monthmap_tz_safe(true, true) &&
		 !sift_scope_monthmap_tz_safe(true, false), "the month cells are safe for timestamp and date, and for timestamptz in UTC only");
}

void
scope_v6_scenario_monthtz(TCheck *t)
{
	int64		wrong_naive = 0,
				answers = 0;
	int			nutc = 0;

	monthtz_inside(t);
	for (int i = 0; i < NZONES; i++)
	{
		const char *name = i < NZONES - 1 ? zone_names[i] : pg_get_timezone_name(session_timezone);
		const pg_tz *tz = i < NZONES - 1 ? pg_tzset(name) : session_timezone;

		TCHK(t, tz != NULL, "the time zone %s is unknown", name);
		if (tz == NULL)
			continue;
		nutc += zone_is_utc(tz);
		monthtz_zone(t, tz, name, &wrong_naive, &answers);
	}
	TCHK(t, wrong_naive > 0, "no zone shows a cell that differs from a local month");
	OUTL(t, "monthtz zones=%d utc_zones=%d answers=%lld", NZONES, nutc, (long long) answers);
}

#endif							/* SIFT_TESTING */
