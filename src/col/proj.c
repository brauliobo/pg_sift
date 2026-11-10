/*
 * col/proj.c - what the named projections of sift.projection_index ask of the server (sql/pg_sift--4.3--4.4.sql, docs/INDEX-RECIPES.md)
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
#include "utils/builtins.h"
#include "utils/rel.h"

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

Datum
sift_col_projection_filter_served(PG_FUNCTION_ARGS)
{
	Relation	heap = relation_open(PG_GETARG_OID(0), AccessShareLock);
	Relation	index = relation_open(PG_GETARG_OID(2), AccessShareLock);
	List	   *pred = RelationGetIndexPredicate(index);
	List	   *filter = filter_conjuncts(heap, text_to_cstring(PG_GETARG_TEXT_PP(1)));
	bool		served = pred == NIL ? index_holds_columns(index, filter) :
		predicate_implied_by(pred, filter, false) && (predicate_implied_by(filter, pred, false) || index_holds_columns(index, filter));

	relation_close(index, AccessShareLock);
	relation_close(heap, AccessShareLock);
	PG_RETURN_BOOL(served);
}
