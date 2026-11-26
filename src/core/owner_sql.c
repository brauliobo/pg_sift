/*
 * core/owner_sql.c - a statement on a catalog table of the extension as its owner: see owner_sql.h
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "core/owner_sql.h"
#include "core/sift.h"

static Oid
rel_owner(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			owner;

	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for relation %u", relid);
	owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
	ReleaseSysCache(tup);
	return owner;
}

/* A VACUUM that reads the catalog may have no snapshot set, which SPI needs */
void
sift_owner_sql(Oid catalog, const SiftOwnerSql *q)
{
	Oid			uid;
	int			sec;
	int			nest;
	bool		pushed = !ActiveSnapshotSet();

	GetUserIdAndSecContext(&uid, &sec);
	SetUserIdAndSecContext(rel_owner(catalog), sec | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
	nest = NewGUCNestLevel();
	RestrictSearchPath();
	if (pushed)
		PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		int			rc;

		SPI_connect();
		rc = SPI_execute_with_args(q->sql, q->nargs, (Oid *) q->types, (Datum *) q->values, q->nulls, q->read_only, q->limit);
		SIFT_INVARIANT(rc == q->expect, "sift: could not run a statement on the catalogs of the extension: %s", q->sql);
		if (q->rows != NULL)
			q->rows(q->out);
	}
	PG_FINALLY();
	{
		SPI_finish();
		if (pushed)
			PopActiveSnapshot();
		AtEOXact_GUC(false, nest);
		SetUserIdAndSecContext(uid, sec);
	}
	PG_END_TRY();
}
