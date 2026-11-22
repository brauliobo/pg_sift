/*
 * col/am/key_attrs.c - what the catalog says of a column of a table, for the choices of the build that both formats make (keycols of format major 1, key_order = 'auto' of format major 2).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"

bool
sift_attr_is_fk(Relation heap, AttrNumber attno)
{
	ListCell   *lc;

	foreach(lc, RelationGetFKeyList(heap))
	{
		const ForeignKeyCacheInfo *fk = lfirst(lc);

		for (int i = 0; i < fk->nkeys; i++)
			if (fk->conkey[i] == attno)
				return true;
	}
	return false;
}

/* the column alone is unique (a primary key, a unique constraint or index of it, not a partial one): its count(DISTINCT) is its count */
bool
sift_attr_is_unique(Relation heap, AttrNumber attno)
{
	bool		found = false;
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(heap))
	{
		HeapTuple	t = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index ix;

		if (!HeapTupleIsValid(t))
			continue;
		ix = (Form_pg_index) GETSTRUCT(t);
		found |= ix->indisunique && ix->indnatts == 1 && ix->indkey.values[0] == attno && heap_attisnull(t, Anum_pg_index_indpred, NULL);
		ReleaseSysCache(t);
	}
	return found;
}
