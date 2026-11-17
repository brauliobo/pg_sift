/*
 * core/restrict_test.c - test-only SQL function sift._test_catalog_write: a writer of core/restrict.c run as a lazy VACUUM would run it.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#include "core/restrict.h"
#include "core/host.h"

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_catalog_write);

static void
set_in_vacuum(bool on)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	if (on)
		MyProc->statusFlags |= PROC_IN_VACUUM;
	else
		MyProc->statusFlags &= ~PROC_IN_VACUUM;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}

/*
 * sift._test_catalog_write(kind, as_vacuum, a, b): the writer `kind` (update, insert, delete, depend, undepend, trigger, drop) as a VACUUM runs it (as_vacuum: the process flag is
 * set for the call) or as a session does. An update writes the pg_class row of a as it is; depend and undepend write and delete a normal dependency of a on b. In a parallel
 * worker or a VACUUM every one of them is an error. The others are asked only to refuse.
 */
Datum
sift_test_catalog_write(PG_FUNCTION_ARGS)
{
	char	   *kind;
	bool		as_vacuum;

	SIFT_ARG_REQUIRED(0, "sift._test_catalog_write", "kind");
	SIFT_ARG_REQUIRED(1, "sift._test_catalog_write", "as_vacuum");
	SIFT_ARG_REQUIRED(2, "sift._test_catalog_write", "a");
	SIFT_ARG_REQUIRED(3, "sift._test_catalog_write", "b");
	kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	as_vacuum = PG_GETARG_BOOL(1);
	Oid			a = PG_GETARG_OID(2);
	Oid			b = PG_GETARG_OID(3);
	ObjectAddress from,
				to;

	ObjectAddressSet(from, RelationRelationId, a);
	ObjectAddressSet(to, RelationRelationId, b);
	if (as_vacuum)
		set_in_vacuum(true);
	PG_TRY();
	{
		if (strcmp(kind, "update") == 0)
		{
			Relation	pgclass = table_open(RelationRelationId, RowExclusiveLock);
			HeapTuple	tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(a));

			sift_cat_update(pgclass, tup);
			table_close(pgclass, RowExclusiveLock);
		}
		else if (strcmp(kind, "insert") == 0)
			sift_cat_insert(NULL, NULL);
		else if (strcmp(kind, "delete") == 0)
			sift_cat_delete(NULL, NULL);
		else if (strcmp(kind, "depend") == 0)
			sift_depend(&from, &to, DEPENDENCY_NORMAL);
		else if (strcmp(kind, "undepend") == 0)
			sift_undepend(RelationRelationId, a, DEPENDENCY_NORMAL, RelationRelationId, b);
		else if (strcmp(kind, "trigger") == 0)
			(void) sift_create_trigger(NULL, a, b);
		else if (strcmp(kind, "drop") == 0)
			sift_drop_object(&from);
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift._test_catalog_write: unknown kind \"%s\"", kind)));
	}
	PG_FINALLY();
	{
		if (as_vacuum)
			set_in_vacuum(false);
	}
	PG_END_TRY();
	PG_RETURN_VOID();
}

#endif
