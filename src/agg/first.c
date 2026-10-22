/*
 * agg/first.c - a correlated child that takes the first row of a table by a key the table holds once is read as a lookup table
 *
 * `LEFT JOIN LATERAL (SELECT cols FROM t WHERE t.a = x AND t.b = y LIMIT 1) ON true` has no ORDER BY: the row it returns is the first one its
 * plan meets. With a unique index on the columns it compared there is no other row, whatever the plan (index, bitmap or sequential scan), so the
 * rows of t hashed by key give the same answer as running the child for every distinct input, without a search of the index for each.
 * The planner has found the form and priced both ways (SiftAggFirst); the executor decides again from the catalog, so a plan made before a
 * unique index was dropped runs its child.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_index.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/agg_int.h"
#include "agg/init_int.h"

/* does a usable unique index of the table hold only columns among atts? Then a row is the only one with its values of them. */
bool
sift_agg_unique_key(Oid relid, int nkey, const int *atts)
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

				while (k < nkey && atts[k] != ix->indkey.values[n])
					k++;
				if (k == nkey)
					break;
			}
			found = n == ix->indnkeyatts;
		}
		ReleaseSysCache(tup);
		if (found)
			break;
	}
	list_free(indexes);
	table_close(rel, AccessShareLock);
	return found;
}

/* does the join read only columns the table has? */
static bool
first_cols_ok(const SiftAggJoin *j)
{
	for (int c = 0; c < j->ncol; c++)
		if (j->colatt[c] < 1 || j->colatt[c] > j->first.ncol || j->first.col[j->colatt[c] - 1] <= 0)
			return false;
	return true;
}

/*
 * The joins the planner priced as lookups of a table become that: the table's columns, the keys the parameters were compared with (the
 * expressions the scan evaluated for the child, per distinct input) and no parameters. The child's plan stays in the statement, never run.
 */
void
join_first_rows(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->njoin; i++)
	{
		SiftAggJoin *j = &s->join[i];
		const SiftAggFirst *f = &j->first;

		if (!f->ok || !f->use || j->params == NIL || j->scoped || j->nidx > 0 || sift_aggjoin_preserves(j->kind) || !first_cols_ok(j) ||
			list_length(j->args) <= 0 || !sift_agg_unique_key(f->relid, f->nkey, f->keyatt))
			continue;
		for (int c = 0; c < j->ncol; c++)
			j->colatt[c] = f->col[j->colatt[c] - 1];
		j->relid = f->relid;
		j->child = -1;
		j->nkey = f->nkey;
		for (int k = 0; k < f->nkey; k++)
		{
			j->keyatt[k] = f->keyatt[k];
			j->keytype[k] = f->keytype[k];
			j->keypos[k] = -1;
			j->keyexpr[k] = list_nth(j->args, f->keyarg[k]);
		}
		j->params = NIL;
		j->args = NIL;
		j->sig = NULL;
		ex->nfirst++;
	}
}
