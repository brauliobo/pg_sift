/*
 * core/follow.c - the pairs of a followed table are refreshed as their owner.
 *
 * The statement trigger of a followed table (sift.scope_follow_trg) fires for every role that writes the table, and reads sift.scope_follows, which no role may read: it
 * runs as the owner of the extension. What it refreshes runs the view, the triggers, the checks and the expression indexes of the pair table, which belong to a role, so
 * sift._scope_refresh_as_owner runs that part with the rights of the owner of the pair table (sift_owner_sql) and never with those of the owner of the extension.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "core/catname.h"
#include "core/owner_sql.h"

PG_FUNCTION_INFO_V1(sift_scope_refresh_as_owner);
PG_FUNCTION_INFO_V1(sift_in_follow_refresh);

static bool refreshing;

/* sift._scope_refresh_as_owner(follow sift.scope_follows, ids bigint[], keys bigint[]): sift._scope_refresh_for(follow, ids, keys) with the rights of the owner of the pairs */
Datum
sift_scope_refresh_as_owner(PG_FUNCTION_ARGS)
{
	bool		isnull;
	Datum		pairs;
	Oid			types[3] = {get_fn_expr_argtype(fcinfo->flinfo, 0), INT8ARRAYOID, INT8ARRAYOID};
	Datum		args[3] = {PG_GETARG_DATUM(0), PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1), PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2)};
	char		nulls[3] = {' ', PG_ARGISNULL(1) ? 'n' : ' ', PG_ARGISNULL(2) ? 'n' : ' '};
	SiftOwnerSql q = {.sql = "SELECT sift._scope_refresh_for($1, $2, $3)", .nargs = 3, .types = types, .values = args, .nulls = nulls, .expect = SPI_OK_SELECT};
	bool		prev = refreshing;

	if (PG_ARGISNULL(0))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("scope follow record must not be null")));
	pairs = GetAttributeByNum(PG_GETARG_HEAPTUPLEHEADER(0), 1, &isnull);
	refreshing = true;
	PG_TRY();
	{
		sift_owner_sql(sift_rel_must_exist(isnull ? InvalidOid : DatumGetObjectId(pairs)), &q);
	}
	PG_FINALLY();
	{
		refreshing = prev;
	}
	PG_END_TRY();
	PG_RETURN_VOID();
}

/* sift._in_follow_refresh(): the refresh of the pairs of a followed table runs, for a role that is not the one that wrote the table */
Datum
sift_in_follow_refresh(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(refreshing);
}
