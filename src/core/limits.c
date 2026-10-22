/*
 * core/limits.c - test-only (SIFT_TESTING): the defaults of the limit registry (core/limits.h), sift.test_limits and sift._test_limits().
 * A release build compiles none of it: sift_limit() is the default macro there.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "agg/part.h"
#include "agg/spec.h"
#include "agg/topn.h"
#include "bm25/query.h"
#include "col/col.h"
#include "core/limits.h"
#include "core/sift.h"

const int64_t sift_limit_default[SIFT_LIM_COUNT] = {
#define X(id, macro, file, value, unit, what, behaviour, setting, hooked) [SIFT_LIM_##id] = (value),
	SIFT_LIMITS(X)
#undef X
};

typedef struct SiftLimitRow
{
	const char *id;
	const char *macro;
	bool		hooked;
} SiftLimitRow;

static const SiftLimitRow limit_rows[SIFT_LIM_COUNT] = {
#define X(id, macro, file, value, unit, what, behaviour, setting, hooked) [SIFT_LIM_##id] = {#id, macro, (hooked) != 0},
	SIFT_LIMITS(X)
#undef X
};

int64_t		sift_limit_eff[SIFT_LIM_COUNT];
static char *test_limits;

/* 'id=value,...' into a per-limit array, -1 for a limit that is not named; a value is between 0 and the default (the setting only lowers) */
static bool
limits_check(char **newval, void **extra, GucSource source)
{
	int64	   *ov;
	char	   *copy = pstrdup(*newval);
	char	   *save = NULL;

	ov = guc_malloc(LOG, sizeof(int64) * SIFT_LIM_COUNT);
	if (ov == NULL)
		return false;
	memset(ov, 0xff, sizeof(int64) * SIFT_LIM_COUNT);
	for (char *item = strtok_r(copy, ",", &save); item != NULL; item = strtok_r(NULL, ",", &save))
	{
		char	   *eq;
		char	   *end;
		int64		v;
		int			i;

		while (*item == ' ')
			item++;
		eq = strchr(item, '=');
		if (eq == NULL || eq == item)
		{
			GUC_check_errmsg("invalid item \"%s\" in sift.test_limits", item);
			GUC_check_errhint("Use id=value, separated by commas.");
			goto bad;
		}
		*eq = '\0';
		for (end = eq - 1; end > item && *end == ' '; end--)
			*end = '\0';
		for (i = 0; i < SIFT_LIM_COUNT; i++)
			if (strcmp(limit_rows[i].id, item) == 0)
				break;
		if (i == SIFT_LIM_COUNT)
		{
			GUC_check_errmsg("unknown limit \"%s\" in sift.test_limits", item);
			GUC_check_errhint("The ids are the first column of SIFT_LIMITS in src/core/limits.h.");
			goto bad;
		}
		if (!limit_rows[i].hooked)
		{
			GUC_check_errmsg("limit \"%s\" cannot be lowered by sift.test_limits: no code reads it through sift_limit() yet", item);
			GUC_check_errhint("Wire the site and set hooked to 1 in its row of src/core/limits.h.");
			goto bad;
		}
		errno = 0;
		v = strtoll(eq + 1, &end, 10);
		while (*end == ' ')
			end++;
		if (errno != 0 || end == eq + 1 || *end != '\0' || v < 0 || v > sift_limit_default[i])
		{
			GUC_check_errmsg("invalid value \"%s\" for limit \"%s\"", eq + 1, item);
			GUC_check_errhint("A value is a number from 0 to the default, " INT64_FORMAT ".", sift_limit_default[i]);
			goto bad;
		}
		ov[i] = v;
	}
	pfree(copy);
	*extra = ov;
	return true;

bad:
	pfree(copy);
	guc_free(ov);
	return false;
}

static void
limits_assign(const char *newval, void *extra)
{
	const int64 *ov = extra;

	for (int i = 0; i < SIFT_LIM_COUNT; i++)
		sift_limit_eff[i] = ov[i] >= 0 ? ov[i] : sift_limit_default[i];
}

void
sift_limits_init(void)
{
	memcpy(sift_limit_eff, sift_limit_default, sizeof(sift_limit_eff));
	DefineCustomStringVariable("sift.test_limits",
							   "Testing: lowers fixed limits, 'id=value,...' (ids: src/core/limits.h; sift._test_limits() lists them).",
							   NULL, &test_limits, "", PGC_USERSET, GUC_NOT_IN_SAMPLE, limits_check, limits_assign, NULL);
}

PG_FUNCTION_INFO_V1(sift_test_limits);

/* id, macro, default, effective, hooked of every limit of the registry */
Datum
sift_test_limits(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < SIFT_LIM_COUNT; i++)
	{
		Datum		vals[5];
		bool		nulls[5] = {false};

		vals[0] = CStringGetTextDatum(limit_rows[i].id);
		vals[1] = CStringGetTextDatum(limit_rows[i].macro);
		vals[2] = Int64GetDatum(sift_limit_default[i]);
		vals[3] = Int64GetDatum(sift_limit_eff[i]);
		vals[4] = BoolGetDatum(limit_rows[i].hooked);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, vals, nulls);
	}
	return (Datum) 0;
}

#endif
