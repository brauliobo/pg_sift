/*
 * table/load.c - the rows of a bulk load (COPY, CREATE TABLE AS, REFRESH MATERIALIZED VIEW) written straight into column groups
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "table/table.h"

/*
 * The rows of a load wait in the backend until a group is written, so nothing may read them in the middle of the command: a unique or
 * exclusion index checks the rows inserted before it, and a trigger (a foreign key is one) can read the table. The load of such a table
 * keeps the write buffers. A published table keeps them too: its changes are logged row by row, as they are inserted.
 */
const char *
sift_tbl_bulk_refusal(Relation rel)
{
	const char *reason = NULL;

	if (rel->trigdesc != NULL)
		return "a trigger or a foreign key can read the table";
	if (sift_tbl_published(rel))
		return "the table is published for logical decoding";
	foreach_oid(indexoid, RelationGetIndexList(rel))
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		Form_pg_index ix;

		if (!HeapTupleIsValid(tup))
			continue;
		ix = (Form_pg_index) GETSTRUCT(tup);
		if (ix->indisunique)
			reason = "a unique index checks the rows of the load";
		else if (ix->indisexclusion)
			reason = "an exclusion constraint checks the rows of the load";
		ReleaseSysCache(tup);
		if (reason)
			break;
	}
	return reason;
}
