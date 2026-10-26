/*
 * agg/first.c - a correlated child that takes the first row of a table by a key the table holds once is read as a lookup table
 *
 * `LEFT JOIN LATERAL (SELECT cols FROM t WHERE t.a = x AND t.b = y LIMIT 1) ON true` has no ORDER BY: the row it returns is the first one its
 * plan meets. With a unique index on the columns it compared there is no other row, whatever the plan (index, bitmap or sequential scan), so the
 * rows of t hashed by key give the same answer as running the child for every distinct input, without a search of the index for each.
 * A key the table repeats is not read this way: without an ORDER BY the row is the first one the child's plan meets, and the index and the heap
 * disagree on it once a row was updated in place (the index keeps the old position, the heap order the new one), so no reading of the table
 * can tell which the child returns (test agg_first_row_lookup, table pick).
 *
 * An ORDER BY on columns a unique index holds with the key (NOT NULL, so that nothing ties) puts the key's rows in one order, whatever the plan:
 * the table is loaded whole and the first row of each key by that order is the one in the chains.
 *
 * A condition of the child on the row (a comparison with a constant, a null test) is tested when the table is loaded: the key's row is either
 * the one the child returns or none. The planner has found the form and priced both ways (SiftAggFirst); the executor decides again from the
 * catalog, so a plan made before a unique index was dropped runs its child.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "nodes/nodeFuncs.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/agg_int.h"
#include "agg/init_int.h"

static bool
att_not_null(Oid relid, int att)
{
	HeapTuple	tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(att));
	bool		notnull = HeapTupleIsValid(tup) && ((Form_pg_attribute) GETSTRUCT(tup))->attnotnull;

	if (HeapTupleIsValid(tup))
		ReleaseSysCache(tup);
	return notnull;
}

/*
 * does a usable unique index of the table hold only columns among atts (nkey compared with `=`, then nord of an ORDER BY, NOT NULL where the index
 * holds them: it takes NULLs for different)? Then a row is the only one with its values of them. *cols, when given, gets the columns of that index.
 */
bool
sift_agg_unique_key(Oid relid, int nkey, int nord, const int *atts, List **cols)
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

/* is the key (and the order columns) held once? */
static bool
first_unique(const SiftAggFirst *f)
{
	int			atts[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_FORD];

	memcpy(atts, f->keyatt, f->nkey * sizeof(int));
	memcpy(atts + f->nkey, f->ordatt, f->nord * sizeof(int));
	return sift_agg_unique_key(f->relid, f->nkey, f->nord, atts, NULL);
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

/* the join as the lookup of the table its first-row form reads: its columns, the keys the parameters were compared with, no parameters */
static SiftAggJoin *
first_table_join(const SiftAggJoin *j)
{
	SiftAggJoin *t = palloc(sizeof(SiftAggJoin));
	const SiftAggFirst *f = &j->first;

	*t = *j;
	for (int c = 0; c < t->ncol; c++)
		t->colatt[c] = f->col[j->colatt[c] - 1];
	t->relid = f->relid;
	t->child = -1;
	t->nkey = f->nkey;
	for (int k = 0; k < f->nkey; k++)
	{
		t->keyatt[k] = f->keyatt[k];
		t->keytype[k] = f->keytype[k];
		t->keypos[k] = -1;
		t->keyexpr[k] = list_nth(j->args, f->keyarg[k]);
		t->keynull[k] = false;
	}
	t->first.params = j->params;
	t->params = NIL;
	t->args = NIL;
	t->sig = NULL;
	return t;
}

typedef struct ParamArgs
{
	const SiftAggJoin *j;
} ParamArgs;

/* the child's parameters in an expression are what the scan gives for them: the expressions over the combo's leaves */
static Node *
args_for_params(Node *n, ParamArgs *x)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		for (int at = 0; at < list_length(x->j->params); at++)
			if (list_nth_int(x->j->params, at) == ((Param *) n)->paramid)
				return copyObject(list_nth(x->j->args, at));
	return expression_tree_mutator(n, args_for_params, x);
}

Node *
first_param_expr(const SiftAggJoin *j)
{
	ParamArgs	x = {j};

	return args_for_params(j->first.pqual, &x);
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
			list_length(j->args) <= 0)
			continue;
		if (f->hybrid)
		{
			if (ex->fjoin == NULL)
				ex->fjoin = palloc0(Max(s->njoin, 1) * sizeof(SiftAggJoin *));
			ex->fjoin[i] = first_table_join(j);
			ex->nfirst++;
			continue;
		}
		if (!first_unique(f))
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
			j->keynull[k] = false;
		}
		j->first.params = j->params;
		j->params = NIL;
		j->args = NIL;
		j->sig = NULL;
		ex->nfirst++;
	}
}
