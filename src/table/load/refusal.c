/*
 * table/load/refusal.c - which tables keep the write buffers instead of taking a bulk load, and which cannot use the ring of workers
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/readfuncs.h"
#include "optimizer/paths.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "table/load/load_int.h"

/* Does the expression call a function (or operator) a user defined? Those can read the table; the built-in ones cannot */
static bool
calls_user_function(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid >= FirstNormalObjectId)
		return true;
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		set_opfuncid(op);
		if (op->opfuncid >= FirstNormalObjectId)
			return true;
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) node;

		set_sa_opfuncid(op);
		if (op->opfuncid >= FirstNormalObjectId)
			return true;
	}
	return expression_tree_walker(node, calls_user_function, context);
}

/* Does the expression call a function that must not run in parallel mode (nextval is one)? */
static bool
calls_parallel_unsafe(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && func_parallel(((FuncExpr *) node)->funcid) == PROPARALLEL_UNSAFE)
		return true;
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		set_opfuncid(op);
		if (func_parallel(op->opfuncid) == PROPARALLEL_UNSAFE)
			return true;
	}
	return expression_tree_walker(node, calls_parallel_unsafe, context);
}

/*
 * The backend that reads the rows of a COPY is in parallel mode while the workers of the ring write, so what it runs for each row must not start a
 * command, a subtransaction or a new xid: a default, a generated column or an identity column that calls nextval, a trigger (the checks of a
 * foreign key are triggers), an index that is not a plain b-tree (the maintenance of a sift index merges in a subtransaction, which failed here and
 * left the index half written). Such a table has its groups written one run at a time, without the ring.
 */
bool
load_row_work_unsafe_in_parallel_mode(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	TupleConstr *constr = desc->constr;

	if (rel->rd_rel->relhastriggers)
		return true;
	foreach_oid(indexoid, RelationGetIndexList(rel))
	{
		Relation	index = index_open(indexoid, NoLock);
		bool		plain = index->rd_rel->relam == BTREE_AM_OID && RelationGetIndexExpressions(index) == NIL && RelationGetIndexPredicate(index) == NIL;

		index_close(index, NoLock);
		if (!plain)
			return true;
	}
	for (int i = 0; i < desc->natts; i++)
		if (TupleDescAttr(desc, i)->attidentity != '\0')
			return true;
	if (constr == NULL)
		return false;
	for (int i = 0; i < constr->num_defval; i++)
		if (calls_parallel_unsafe(stringToNode(constr->defval[i].adbin), NULL))
			return true;
	for (int i = 0; i < constr->num_check; i++)
		if (calls_parallel_unsafe(stringToNode(constr->check[i].ccbin), NULL))
			return true;
	return false;
}

/* The expressions the executor evaluates for every row of the table (defaults, generated columns, CHECK constraints) */
static bool
row_expressions_call_user_function(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	TupleConstr *constr = desc->constr;

	for (int i = 0; i < desc->natts; i++)
		if (!TupleDescAttr(desc, i)->attisdropped && get_typtype(TupleDescAttr(desc, i)->atttypid) == TYPTYPE_DOMAIN)
			return true;
	if (constr == NULL)
		return false;
	for (int i = 0; i < constr->num_defval; i++)
		if (calls_user_function(stringToNode(constr->defval[i].adbin), NULL))
			return true;
	for (int i = 0; i < constr->num_check; i++)
		if (calls_user_function(stringToNode(constr->check[i].ccbin), NULL))
			return true;
	return false;
}

/*
 * The rows of a load wait in the backend until a group is written, so nothing may read them in the middle of the command: a unique or
 * exclusion index checks the rows inserted before it, a trigger (a foreign key is one) can read the table, and so can a function of the
 * table's own expressions (a default, a generated column, a CHECK, a domain, an index expression or predicate) when the user wrote it: it runs
 * for every row, and a volatile one sees the rows inserted before it by the same command. The load of such a table keeps the write buffers.
 * A published table keeps them too: its changes are logged row by row, as they are inserted.
 */
const char *
sift_tbl_bulk_refusal(Relation rel)
{
	const char *reason = NULL;

	if (rel->trigdesc != NULL)
		return "a trigger or a foreign key can read the table";
	if (sift_tbl_published(rel))
		return "the table is published for logical decoding";
	if (row_expressions_call_user_function(rel))
		return "a default, a check or a domain of the table calls a function of the user";
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
		if (reason == NULL)
		{
			Relation	index = index_open(indexoid, AccessShareLock);

			if (calls_user_function((Node *) RelationGetIndexExpressions(index), NULL) ||
				calls_user_function((Node *) RelationGetIndexPredicate(index), NULL))
				reason = "an index expression or predicate calls a function of the user";
			index_close(index, AccessShareLock);
		}
		if (reason)
			break;
	}
	return reason;
}
