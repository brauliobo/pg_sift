/*
 * agg/osa/round.c - how this server rounds the interpolation of percentile_cont
 * The final functions of PostgreSQL find the proportion `p * (n - 1) - first` and the result `lo + prop * (hi - lo)` in float8. A server built with contraction of
 * floating-point expressions (the default of gcc and clang on aarch64, or any build for a CPU with FMA) rounds each of them once, a build without it twice, and the
 * scalar function and the one for an array of fractions are compiled apart, so each may differ. The scan is built with -ffp-contract=off and cannot see the flags
 * of the server: it runs percentile_cont once on inputs where the two roundings give other bits and does what the server did. The same holds for `x * N - Sx` of the
 * update of float8_accum (the moments of variance and stddev): the server's own function is run once on such inputs.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"

#include "core/host.h"
#include "agg/group/ext.h"
#include "agg/osa/round.h"

/* 34 values, the first 19 zero and the rest one: the result is the proportion, 0.5659... * 33 - 18 */
#define PROP_ROWS		34
#define PROP_ZEROS		19
#define PROP_FRAC		0.56593902927640549
/* two values and a fraction where lo + p * (hi - lo) is another float8 when p * (hi - lo) is rounded first */
#define LERP_FRAC		0.518606283111404
#define LERP_LO			118.1387346305022
#define LERP_HI			265.62691770481342

double
osa_prop(double frac, int64 n, int64 first, bool fused)
{
	return fused ? fma(frac, (double) (n - 1), -(double) first) : (frac * (double) (n - 1)) - (double) first;
}

double
osa_lerp(double lo, double hi, double prop, bool fused)
{
	return fused ? fma(prop, hi - lo, lo) : lo + (prop * (hi - lo));
}

/* the scalar percentile_cont or the one of an array of one fraction, over the values of from */
static void
probe_column(StringInfo sql, bool array, double frac, const char *from)
{
	appendStringInfo(sql, array ? "(SELECT (percentile_cont(ARRAY[%.17g::float8]) WITHIN GROUP (ORDER BY v))[1] FROM %s t(v))" :
					 "(SELECT percentile_cont(%.17g::float8) WITHIN GROUP (ORDER BY v) FROM %s t(v))", frac, from);
}

/* the results of the server's two functions on the proportion probe and on the interpolation probe: scalar, array, scalar, array */
static void
probe_server(double res[4])
{
	const char *prop = psprintf("(SELECT CASE WHEN i > %d THEN 1::float8 ELSE 0::float8 END FROM generate_series(1, %d) i)", PROP_ZEROS, PROP_ROWS),
			   *lerp = psprintf("(VALUES (%.17g::float8), (%.17g::float8))", LERP_LO, LERP_HI);
	int			nest = NewGUCNestLevel();
	StringInfoData sql;

	/* the probe has no sift table, and its array of fractions must reach the server's function as it is */
	set_config_option("sift.enable_agg", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, ERROR, false);
	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
	for (int i = 0; i < 4; i++)
	{
		probe_column(&sql, i % 2, i < 2 ? PROP_FRAC : LERP_FRAC, i < 2 ? prop : lerp);
		appendStringInfoString(&sql, i < 3 ? ", " : "");
	}
	SIFT_INVARIANT(SPI_connect() == SPI_OK_CONNECT, "sift: SPI_connect failed");
	SIFT_INVARIANT(SPI_execute(sql.data, true, 1) == SPI_OK_SELECT && SPI_processed == 1, "sift: the percentile probe failed");
	for (int i = 0; i < 4; i++)
	{
		bool		isnull;

		res[i] = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, i + 1, &isnull));
		SIFT_INVARIANT(!isnull, "sift: the percentile probe gave NULL");
	}
	SPI_finish();
	AtEOXact_GUC(true, nest);
}

static bool
fused_as(double got, double fused, double plain)
{
	SIFT_INVARIANT(got == fused || got == plain, "sift: the percentile interpolation of the server is neither fused nor plain");
	return got == fused && got != plain;
}

const OsaRound *
osa_round(bool array)
{
	static OsaRound known[2];
	static bool probed;

	if (!probed)
	{
		double		res[4];

		probe_server(res);
		for (int a = 0; a < 2; a++)
		{
			known[a].prop = fused_as(res[a], osa_prop(PROP_FRAC, PROP_ROWS, PROP_ZEROS - 1, true), osa_prop(PROP_FRAC, PROP_ROWS, PROP_ZEROS - 1, false));
			known[a].lerp = fused_as(res[2 + a], osa_lerp(LERP_LO, LERP_HI, LERP_FRAC, true), osa_lerp(LERP_LO, LERP_HI, LERP_FRAC, false));
		}
		probed = true;
	}
	return &known[array];
}

/* a state of float8_accum and a value where `x * N - Sx` rounded once and rounded twice give other bits */
#define ACCUM_N			2.0
#define ACCUM_SX		2000000000.014
#define ACCUM_X			1000000000.001

bool
osa_accum_fused(void)
{
	static int	known = -1;

	if (known < 0)
	{
		Datum		state[3] = {Float8GetDatum(ACCUM_N), Float8GetDatum(ACCUM_SX), Float8GetDatum(0.0)};
		ArrayType  *out = DatumGetArrayTypeP(DirectFunctionCall2(float8_accum, PointerGetDatum(construct_array_builtin(state, 3, FLOAT8OID)), Float8GetDatum(ACCUM_X)));
		const double sxx = ((const double *) ARR_DATA_PTR(out))[2],
					fused = sift_mom_tmp(ACCUM_X, ACCUM_N + 1.0, ACCUM_SX + ACCUM_X, true),
					plain = sift_mom_tmp(ACCUM_X, ACCUM_N + 1.0, ACCUM_SX + ACCUM_X, false);

		SIFT_INVARIANT(fused != plain, "sift: the probe of float8_accum rounds the same either way");
		known = fused_as(sxx, fused * fused / ((ACCUM_N + 1.0) * ACCUM_N), plain * plain / ((ACCUM_N + 1.0) * ACCUM_N));
	}
	return known;
}
