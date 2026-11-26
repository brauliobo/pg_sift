/*
 * col/proj.c - what the named projections of sift.projection_index ask of the server (sql/5.0/06-projections.sql, docs/INDEX-RECIPES.md)
 *
 * sift.projection_filter_served(source, filter, index): can the index answer the rows of a projection's filter? It keeps every row of the filter (the filter implies the
 * predicate of the index, which has none or a weaker one) and either keeps no other row (the predicate implies the filter) or holds the columns the filter reads, so that the
 * scan tests it on the rows the index keeps. The filter is parsed against the table as a CREATE INDEX .. WHERE would, and the implications are the planner's own
 * (predicate_implied_by, the proof the aggregate scan makes for a query against a partial index).
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/sysattr.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "core/catname.h"
#include "core/inspect.h"

PG_FUNCTION_INFO_V1(sift_col_projection_filter_served);

/* The filter as the implicit-AND list of its simplified conjuncts, in terms of the columns of heap (range table entry 1) */
static List *
filter_conjuncts(Relation heap, const char *filter)
{
	StringInfoData sql;
	SelectStmt *select;
	ParseState *pstate = make_parsestate(NULL);
	Node	   *expr;

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT FROM t WHERE %s", filter);
	select = (SelectStmt *) ((RawStmt *) linitial(raw_parser(sql.data, RAW_PARSE_DEFAULT)))->stmt;
	pstate->p_sourcetext = sql.data;
	addNSItemToQuery(pstate, addRangeTableEntryForRelation(pstate, heap, AccessShareLock, NULL, false, true), true, true, true);
	expr = transformWhereClause(pstate, select->whereClause, EXPR_KIND_INDEX_PREDICATE, "WHERE");
	assign_expr_collations(pstate, expr);
	expr = eval_const_expressions(NULL, expr);
	expr = (Node *) canonicalize_qual((Expr *) expr, true);
	free_parsestate(pstate);
	return make_ands_implicit((Expr *) expr);
}

/* Every column the conjuncts read is a plain column of the index */
static bool
index_holds_columns(Relation index, List *conjuncts)
{
	Bitmapset  *attrs = NULL;
	int			x = -1;

	pull_varattnos((Node *) conjuncts, 1, &attrs);
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	att = x + FirstLowInvalidHeapAttributeNumber;
		bool		held = false;

		for (int i = 0; i < index->rd_index->indnatts; i++)
			held |= index->rd_index->indkey.values[i] == att;
		if (!held)
			return false;
	}
	return true;
}

/* The index is one of the table, and the caller may look at the table: a table or an index of another table is no answer for a filter */
static void
require_index_of(Relation heap, Relation index)
{
	if (index->rd_rel->relkind != RELKIND_INDEX || index->rd_index->indrelid != RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not an index of \"%s\"", RelationGetRelationName(index), RelationGetRelationName(heap))));
	if (!sift_may_inspect_as(GetOuterUserId(), RelationGetRelid(heap)))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(heap));
}

/* The filter is the text of a role: the functions it names run with the rights of the owner of the table, as those of an index predicate do, never with those of the caller */
static bool
served(Relation heap, Relation index, const char *text)
{
	Oid			uid;
	int			sec;
	List	   *pred;
	List	   *filter;
	bool		served;

	GetUserIdAndSecContext(&uid, &sec);
	SetUserIdAndSecContext(heap->rd_rel->relowner, sec | SECURITY_RESTRICTED_OPERATION);
	pred = RelationGetIndexPredicate(index);
	filter = filter_conjuncts(heap, text);
	served = pred == NIL ? index_holds_columns(index, filter) :
		predicate_implied_by(pred, filter, false) && (predicate_implied_by(filter, pred, false) || index_holds_columns(index, filter));
	SetUserIdAndSecContext(uid, sec);
	return served;
}

Datum
sift_col_projection_filter_served(PG_FUNCTION_ARGS)
{
	Relation	heap = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	Relation	index = relation_open(sift_rel_must_exist(PG_GETARG_OID(2)), AccessShareLock);
	bool		result;

	require_index_of(heap, index);
	result = served(heap, index, text_to_cstring(PG_GETARG_TEXT_PP(1)));
	relation_close(index, AccessShareLock);
	relation_close(heap, AccessShareLock);
	PG_RETURN_BOOL(result);
}
