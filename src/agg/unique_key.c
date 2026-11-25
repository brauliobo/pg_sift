/*
 * agg/unique_key.c - whether a unique index of a table holds only given columns (agg/unique_key.h)
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "agg/unique_key.h"

static bool
att_not_null(Oid relid, int att)
{
	HeapTuple	tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(att));
	bool		notnull = HeapTupleIsValid(tup) && ((Form_pg_attribute) GETSTRUCT(tup))->attnotnull;

	if (HeapTupleIsValid(tup))
		ReleaseSysCache(tup);
	return notnull;
}

bool
sift_v6_unique_key(Oid relid, int nkey, int nord, const int *atts, List **cols)
{
	Relation	rel = table_open(relid, AccessShareLock);
	List	   *indexes = RelationGetIndexList(rel);
	bool		found = false;

	foreach_oid(indexoid, indexes)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		Form_pg_index ix;
		int			n;

		if (!HeapTupleIsValid(tup))
			continue;
		ix = (Form_pg_index) GETSTRUCT(tup);
		/* as the planner's own use of an index (get_relation_info): one that is not valid yet, is deferrable, partial or on an expression proves nothing */
		if (ix->indisunique && ix->indimmediate && ix->indisvalid && ix->indislive && ix->indnkeyatts > 0 &&
			heap_attisnull(tup, Anum_pg_index_indpred, NULL) && heap_attisnull(tup, Anum_pg_index_indexprs, NULL) &&
			(!ix->indcheckxmin || TransactionIdPrecedes(HeapTupleHeaderGetXmin(tup->t_data), TransactionXmin)))
		{
			for (n = 0; n < ix->indnkeyatts; n++)
			{
				int			k = 0;

				while (k < nkey + nord && atts[k] != ix->indkey.values[n])
					k++;
				if (k == nkey + nord || (k >= nkey && !ix->indnullsnotdistinct && !att_not_null(relid, atts[k])))
					break;
			}
			found = n == ix->indnkeyatts;
			for (n = 0; found && cols != NULL && n < ix->indnkeyatts; n++)
				*cols = lappend_int(*cols, ix->indkey.values[n]);
		}
		ReleaseSysCache(tup);
		if (found)
			break;
	}
	list_free(indexes);
	table_close(rel, AccessShareLock);
	return found;
}
