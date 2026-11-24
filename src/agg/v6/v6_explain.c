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

void
sift_v6_explain_reads(const SiftV6Spec *spec, Relation heap, Relation index, ExplainState *es)
{
	explain_filters(spec, heap, index, es);
	if (OidIsValid(spec->text_indexoid))
		ExplainPropertyText("Search", psprintf("%s @@> %s", get_rel_name(spec->text_indexoid), deparse_expression((Node *) spec->text_query, NIL, false, false)), es);
}

/* the relations a dimension plan reads: a table by its name, a sub-select by its alias (its own scans are its own) */
static bool
dimension_rels(PlanState *ps, Bitmapset **rels)
{
	switch (nodeTag(ps->plan))
	{
		case T_SubqueryScan:
			*rels = bms_add_member(*rels, ((Scan *) ps->plan)->scanrelid);
			return false;
		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_CustomScan:
			if (((Scan *) ps->plan)->scanrelid > 0)
				*rels = bms_add_member(*rels, ((Scan *) ps->plan)->scanrelid);
			break;
		default:
			break;
	}
	return planstate_tree_walker(ps, dimension_rels, rels);
}

static char *
dimension_name(const RangeTblEntry *rte)
{
	return rte->rtekind == RTE_SUBQUERY ? psprintf("%s (subquery)", rte->eref->aliasname) : get_rel_name(rte->relid);
}

void
sift_v6_explain_lookup_joins(CustomScanState *css, ExplainState *es)
{
	List	   *names = NIL;

	foreach_ptr(PlanState, ps, css->custom_ps)
	{
		Bitmapset  *rels = NULL;
		int			rti = -1;

		dimension_rels(ps, &rels);
		while ((rti = bms_next_member(rels, rti)) >= 0)
			names = lappend(names, dimension_name(rt_fetch(rti, es->rtable)));
	}
	if (names != NIL)
		ExplainPropertyList("Lookup Joins", names, es);
}

/* the column of a scan key, by the name of the table: attno is the position in the index, or the attribute of the table */
static const char *
key_name(Oid relid, bool is_table, int attno)
{
	Relation	index;
	AttrNumber	att = attno;
	Oid			heapoid = relid;

	if (!is_table)
	{
		index = index_open(relid, AccessShareLock);
		heapoid = index->rd_index->indrelid;
		att = index->rd_index->indkey.values[attno - 1];
		index_close(index, AccessShareLock);
	}
	return get_attname(heapoid, att, false);
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
		if (keys[i].col.src != SV6SRC_ATTR || keys[i].col.jleaf != 0 || keys[i].col.derive != 0)
			return;
	for (int i = 0; i < nkeys; i++)
		appendStringInfo(&buf, "%s%s%s%s", i ? ", " : "", key_name(relid, is_table, keys[i].col.attno), keys[i].desc ? " DESC" : "",
						 keys[i].desc == keys[i].nulls_first ? "" : keys[i].nulls_first ? " NULLS FIRST" : " NULLS LAST");
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
