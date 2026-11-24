/*
 * agg/v6/v6_explain.c - the EXPLAIN lines of SiftAggV6 that name what the scan reads: its Filters, its Search and the relations of its Lookup Joins, labelled as the plan of SiftAgg
 * labels them (agg/exec/explain.c), so that one claim of docs/coverage.tsv holds for both engines.
 */
#include "postgres.h"

#include "commands/explain.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "agg/v6/v6_explain.h"
#include "agg/v6/v6_topn.h"
#include "core/compat.h"

/* The qual of an index scan reads the columns of the index and its parameters from the executor: as the statement wrote it, it reads those of the heap and prints its parameters as $n */
static Node *
as_written(Node *n, Relation index)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Param))
	{
		Param	   *p = copyObject((Param *) n);

		p->paramkind = PARAM_EXTERN;
		return (Node *) p;
	}
	if (IsA(n, Var) && ((Var *) n)->varno == INDEX_VAR)
	{
		Var		   *v = copyObject((Var *) n);

		if (index != NULL)
			v->varattno = index->rd_index->indkey.values[v->varattno - 1];
		v->varno = v->varnosyn = 1;
		v->varattnosyn = v->varattno;
		return (Node *) v;
	}
	return expression_tree_mutator(n, as_written, index);
}

static void
explain_filters(const SiftV6Spec *spec, Relation heap, Relation index, ExplainState *es)
{
	List	   *dpcontext = deparse_context_for(RelationGetRelationName(heap), RelationGetRelid(heap));
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_ptr(Node, q, list_concat_copy(spec->indexqual, spec->vqual))
		appendStringInfo(&buf, "%s%s", buf.len ? " AND " : "", deparse_expression(as_written(q, index), dpcontext, false, false));
	if (buf.len)
		ExplainPropertyText("Filters", buf.data, es);
}

/* the text queries that keep the rows: one, or the arms of an OR */
static char *
search_desc(const SiftV6Spec *spec)
{
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_ptr(Node, q, spec->text_queries)
		appendStringInfo(&buf, "%s%s @@> %s", buf.len > 0 ? " OR " : "", get_rel_name(spec->text_indexoid), deparse_expression(q, NIL, false, false));
	return buf.data;
}

void
sift_v6_explain_reads(const SiftV6Spec *spec, Relation heap, Relation index, ExplainState *es)
{
	explain_filters(spec, heap, index, es);
	if (OidIsValid(spec->text_indexoid))
		ExplainPropertyText("Search", search_desc(spec), es);
}

/* the relations a dimension plan reads: a table by its name, a sub-select by its alias (its own scans are its own) */
static bool
dimension_rels(PlanState *ps, Bitmapset **rels)
{
	const Plan *plan = ps->plan;
	const bool	scan = IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan) || IsA(plan, CustomScan);

	if ((scan || IsA(plan, SubqueryScan)) && ((Scan *) plan)->scanrelid > 0)
		*rels = bms_add_member(*rels, ((Scan *) plan)->scanrelid);
	return !IsA(plan, SubqueryScan) && planstate_tree_walker(ps, dimension_rels, rels);
}

static char *
dimension_name(const RangeTblEntry *rte)
{
	return rte->rtekind == RTE_SUBQUERY ? psprintf("%s (subquery)", rte->eref->aliasname) : get_rel_name(rte->relid);
}

static const char *
join_tag(SiftV6JoinMode mode)
{
	switch (mode)
	{
		case SV6J_LEFT_EQUI:
			return " (left)";
		case SV6J_SEMI_EQUI:
			return " (semi)";
		case SV6J_ANTI_EQUI:
			return " (anti)";
		case SV6J_RIGHT_EQUI:
			return " (preserved)";
		case SV6J_FULL_EQUI:
			return " (full)";
		default:
			return "";
	}
}

void
sift_v6_explain_lookup_joins(CustomScanState *css, const SiftV6Ext *ext, ExplainState *es)
{
	List	   *names = NIL;

	for (int d = 0; d < ext->ndims; d++)
	{
		Bitmapset  *rels = NULL;
		int			rti = -1;

		dimension_rels(list_nth(css->custom_ps, d), &rels);
		while ((rti = bms_next_member(rels, rti)) >= 0)
			names = lappend(names, psprintf("%s%s", dimension_name(rt_fetch(rti, es->rtable)), join_tag(ext->dim[d].mode)));
	}
	if (names != NIL)
		ExplainPropertyList("Lookup Joins", names, es);
}

/* a column of the scan that the statement names as it is, by the name of the table: derived, jsonb, joined and index expression columns have no name, and print no line rather than a made-up one */
static const char *
col_name(Oid relid, bool is_table, const SiftV6Col *c)
{
	Oid			heapoid = relid;
	AttrNumber	att = c->attno;

	if (c->src != SV6SRC_ATTR || c->attno <= 0 || c->jleaf != 0 || c->derive != 0)
		return NULL;
	if (!is_table)
	{
		Relation	index = index_open(relid, AccessShareLock);

		heapoid = index->rd_index->indrelid;
		att = index->rd_index->indkey.values[c->attno - 1];
		index_close(index, AccessShareLock);
	}
	return att > 0 ? get_attname(heapoid, att, false) : NULL;
}

void
sift_v6_explain_group_keys(const SiftV6Spec *spec, ExplainState *es)
{
	StringInfoData buf;

	if (spec->nsets > 0 || spec->affine || spec->nkeys == 0)
		return;
	initStringInfo(&buf);
	for (int i = 0; i < spec->nkeys; i++)
	{
		const char *name = col_name(spec->relid, spec->is_table, &spec->key[i]);

		if (name == NULL)
			return;
		appendStringInfo(&buf, "%s%s", i ? ", " : "", name);
	}
	ExplainPropertyText("Group Keys", buf.data, es);
}

typedef struct TopNKey
{
	SiftV6Col	col;
	bool		desc;
	bool		nulls_first;
} TopNKey;

static void
explain_topn(Oid relid, bool is_table, const TopNKey *keys, int nkeys, const char *limit, ExplainState *es)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = 0; i < nkeys; i++)
	{
		const char *name = col_name(relid, is_table, &keys[i].col);

		if (name == NULL)
			return;
		appendStringInfo(&buf, "%s%s%s%s", i ? ", " : "", name, keys[i].desc ? " DESC" : "",
						 keys[i].desc == keys[i].nulls_first ? "" : keys[i].nulls_first ? " NULLS FIRST" : " NULLS LAST");
	}
	ExplainPropertyText("Top-N", psprintf("%s (limit %s)", buf.data, limit), es);
}

void
sift_v6_explain_topn_groups(const SiftV6Spec *spec, ExplainState *es)
{
	TopNKey		keys[SIFT_V6_MAX_TOPK_ITEMS];

	for (int i = 0; i < spec->ntopk; i++)
		if (spec->topk_item[i].pos >= spec->nkeys)
			return;
	if (spec->ntopk == 0)
		return;
	for (int i = 0; i < spec->ntopk; i++)
	{
		Oid			opfamily,
					opcintype;
		SiftCmpStrategy cmptype;

		get_ordering_op_properties(spec->topk_item[i].sortop, &opfamily, &opcintype, &cmptype);
		keys[i] = (TopNKey) {.col = spec->key[spec->topk_item[i].pos],.desc = cmptype == SIFT_CMP_GT,.nulls_first = spec->topk_item[i].nulls_first};
	}
	explain_topn(spec->relid, spec->is_table, keys, spec->ntopk, psprintf("%u", spec->topk), es);
}

/* the LIMIT of the Top-N scan: an int8 Const (NULL: all) or an external parameter */
static char *
limit_text(const Node *limit)
{
	const Const *c = (const Const *) limit;

	if (!IsA(limit, Const))
		return deparse_expression((Node *) limit, NIL, false, false);
	return c->constisnull ? "ALL" : psprintf(INT64_FORMAT, DatumGetInt64(c->constvalue));
}

void
sift_v6_explain_topn_rows(const SiftV6TopNSpec *spec, ExplainState *es)
{
	TopNKey		keys[SIFT_TOURN_MAX_KEYS];

	for (int i = 0; i < spec->nkeys; i++)
		keys[i] = (TopNKey) {.col = spec->key[i],.desc = spec->order[i].desc,.nulls_first = spec->order[i].nulls_first};
	explain_topn(spec->indexoid, false, keys, spec->nkeys, limit_text(spec->limit), es);
}
