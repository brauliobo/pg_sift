/*
 * core/extver.c - whether the SQL of the database is as new as the library (see core/ops.h)
 *
 * A new library can run over the SQL objects of an older release until ALTER EXTENSION UPDATE: a feature that looks up an object of the newer SQL
 * (the access method of the shared value store) finds nothing and fails with a bare catalog error. Such a feature asks here first and stays off
 * until the update. The version is read from pg_extension at each call, so an update in the same session takes effect at once; the callers are build paths.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_extension.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

#include "core/ops.h"

/* The extension version of pg_sift in this database, as text, NULL when it is not installed */
static char *
installed_version(void)
{
	Relation	rel = table_open(ExtensionRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *version = NULL;

	ScanKeyInit(&key, Anum_pg_extension_extname, BTEqualStrategyNumber, F_NAMEEQ, CStringGetDatum("pg_sift"));
	scan = systable_beginscan(rel, ExtensionNameIndexId, true, NULL, 1, &key);
	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;

		version = text_to_cstring(DatumGetTextPP(heap_getattr(tup, Anum_pg_extension_extversion, RelationGetDescr(rel), &isnull)));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return version;
}

/* a before b: -1, equal: 0, after: 1; "5.0" is before "5.0.1" */
static int
version_cmp(const char *a, const char *b)
{
	for (;;)
	{
		long		x = strtol(a, (char **) &a, 10),
					y = strtol(b, (char **) &b, 10);

		if (x != y)
			return x < y ? -1 : 1;
		if (*a != '.' && *b != '.')
			return 0;
		a += *a == '.';
		b += *b == '.';
	}
}

bool
sift_extension_at_least(const char *since)
{
	char	   *version = installed_version();

	return version != NULL && version_cmp(version, since) >= 0;
}
