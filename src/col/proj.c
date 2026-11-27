/*
 * col/proj.c - what the named projections of sift.projection_index ask of the server (sql/5.0/06-projections.sql, docs/INDEX-RECIPES.md)
 *
 * sift.projection_filter_served(source, filter, index): can the index answer the rows of a projection's filter? It keeps every row of the filter (the filter implies the
 * predicate of the index, which has none or a weaker one) and either keeps no other row (the predicate implies the filter) or holds the columns the filter reads, so that the
 * scan tests it on the rows the index keeps. The filter is parsed against the table as a CREATE INDEX .. WHERE would, and the implications are the planner's own
 * (predicate_implied_by, the proof the aggregate scan makes for a query against a partial index).
 *
 * sift._projection_index_sql(index, source, leaves, options, filter, concurrently): the CREATE INDEX of projections. Each column is one index element and the filter one expression, and the
 * statement they make is one CREATE INDEX of the table: the definitions are text a role stored, and a superuser runs the statement.
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
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "core/catalog.h"
#include "core/catname.h"
#include "core/host.h"
#include "core/inspect.h"

PG_FUNCTION_INFO_V1(sift_col_projection_filter_served);
PG_FUNCTION_INFO_V1(sift_col_projection_index_sql);

/* The text is one CREATE INDEX statement of nleaves index elements, with options and a predicate only as said, and nothing else */
static IndexStmt *
index_statement(const char *sql, int nleaves, bool with, bool filtered)
{
	List	   *raw = raw_parser(sql, RAW_PARSE_DEFAULT);
	IndexStmt  *stmt = list_length(raw) == 1 ? (IndexStmt *) ((RawStmt *) linitial(raw))->stmt : NULL;

	if (stmt == NULL || !IsA(stmt, IndexStmt) || list_length(stmt->indexParams) != nleaves || (stmt->options != NIL) != with || (stmt->whereClause != NULL) != filtered ||
		stmt->indexIncludingParams != NIL || stmt->tableSpace != NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("a projection is not one CREATE INDEX statement: %s", sql),
						errdetail("Each column of a projection is one index element and its filter one expression.")));
	return stmt;
}

/* The filter as the implicit-AND list of its simplified conjuncts, in terms of the columns of heap (range table entry 1) */
static List *
filter_conjuncts(Relation heap, const char *filter)
{
	char	   *sql = psprintf("CREATE INDEX ON t USING sift_col (x) WHERE %s", filter);
	ParseState *pstate = make_parsestate(NULL);
	Node	   *expr;

	pstate->p_sourcetext = sql;
	addNSItemToQuery(pstate, addRangeTableEntryForRelation(pstate, heap, AccessShareLock, NULL, false, true), true, true, true);
	expr = transformWhereClause(pstate, index_statement(sql, 1, false, true)->whereClause, EXPR_KIND_INDEX_PREDICATE, "WHERE");
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

/* The index is a sift_col index of the table, and the caller may look at the table: another relation is no answer for a filter */
static void
require_index_of(Relation heap, Relation index)
{
	if (index->rd_rel->relkind != RELKIND_INDEX || index->rd_rel->relam != sift_catalog()->col_am || index->rd_index->indrelid != RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index of \"%s\"", RelationGetRelationName(index), RelationGetRelationName(heap))));
	if (!sift_may_inspect_as(GetOuterUserId(), RelationGetRelid(heap)))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(heap));
}

/* The filter is the text the owner of the table stored in sift.projections: its functions run with the rights of the owner and the search_path of a maintenance command,
 * as those of an index predicate do. No role but the owner of the extension may call this function (sql/6.0/36-security-hardening.sql). */
static bool
served(Relation heap, Relation index, const char *text)
{
	Oid			uid;
	int			sec;
	int			nest;
	List	   *pred;
	List	   *filter;
	bool		served;

	GetUserIdAndSecContext(&uid, &sec);
	SetUserIdAndSecContext(heap->rd_rel->relowner, sec | SECURITY_RESTRICTED_OPERATION);
	nest = NewGUCNestLevel();
	RestrictSearchPath();
	pred = RelationGetIndexPredicate(index);
	filter = filter_conjuncts(heap, text);
	served = pred == NIL ? index_holds_columns(index, filter) :
		predicate_implied_by(pred, filter, false) && (predicate_implied_by(filter, pred, false) || index_holds_columns(index, filter));
	AtEOXact_GUC(false, nest);
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

Datum
sift_col_projection_index_sql(PG_FUNCTION_ARGS)
{
	Oid			source;
	char	   *filter = PG_ARGISNULL(4) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(4));
	Datum	   *leaves;
	bool	   *nulls;
	int			n;
	StringInfoData sql;

	SIFT_ARG_REQUIRED(0, "sift._projection_index_sql", "index");
	SIFT_ARG_REQUIRED(1, "sift._projection_index_sql", "source");
	SIFT_ARG_REQUIRED(2, "sift._projection_index_sql", "leaves");
	SIFT_ARG_REQUIRED(5, "sift._projection_index_sql", "conc");
	source = sift_rel_must_exist(PG_GETARG_OID(1));
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(2), TEXTOID, &leaves, &nulls, &n);
	initStringInfo(&sql);
	appendStringInfo(&sql, "CREATE INDEX %s%s ON %s USING sift_col (", PG_GETARG_BOOL(5) ? "CONCURRENTLY " : "", quote_identifier(text_to_cstring(PG_GETARG_TEXT_PP(0))),
					 quote_qualified_identifier(sift_namespace_name(get_rel_namespace(source)), sift_rel_name(source)));
	for (int i = 0; i < n; i++)
	{
		char	   *leaf = nulls[i] ? "" : TextDatumGetCString(leaves[i]);

		index_statement(psprintf("CREATE INDEX ON t USING sift_col (%s)", leaf), 1, false, false);
		appendStringInfo(&sql, "%s%s", i > 0 ? ", " : "", leaf);
	}
	appendStringInfoChar(&sql, ')');
	if (!PG_ARGISNULL(3))
		appendStringInfo(&sql, " WITH (%s)", text_to_cstring(PG_GETARG_TEXT_PP(3)));
	if (filter != NULL)
		appendStringInfo(&sql, " WHERE %s", filter);
	index_statement(sql.data, n, !PG_ARGISNULL(3), filter != NULL);
	PG_RETURN_TEXT_P(cstring_to_text(sql.data));
}
