/*
 * sift_stubs.c - weak placeholders for entry points whose modules have not
 * landed yet. A module defining the real (strong) symbol overrides its stub
 * at link time; its owner then deletes the stub here. See the design notes MODULE
 * MAP, contract 1.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "utils/selfuncs.h"

#include "sift.h"
#include "sift_am.h"
#include "sift_sql.h"

#define WEAK __attribute__((weak))

#define NOT_IMPLEMENTED(what) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("sift: %s is not implemented yet", what)))

WEAK void
sift_planner_init(void)
{
}

/* No simplification and no index condition */
WEAK Datum
sift_match_support(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(NULL);
}

WEAK Datum
sift_score(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.score()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_max_score(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.max_score()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_highlight(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.highlight()");
	PG_RETURN_NULL();
}

