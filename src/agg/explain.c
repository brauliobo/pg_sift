/*
 * agg/explain.c - the EXPLAIN lines of SiftAggV6 that name what the scan reads: its Filters, its Search and the relations of its Lookup Joins, labelled as the plan of SiftAgg
 * labels them (agg/exec/explain.c), so that one claim of docs/coverage.tsv holds for both engines.
 */
#include "postgres.h"

#include "catalog/index.h"
#include "commands/explain.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "agg/explain.h"
#include "agg/expr/num.h"
#include "agg/qual/bqual.h"
#include "agg/qual/step.h"
#include "agg/topn/topn.h"
#include "core/compat.h"
#include "core/explain.h"

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

/* a qual over jsonb_typeof(doc -> 'key') reads a virtual attribute of the scan the statement has no name for */
static bool
reads_json_type(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varattno >= SIFT_V6_JTYPE_VATT;
	return expression_tree_walker(n, reads_json_type, context);
}

static void
explain_filters(const SiftV6Spec *spec, Relation heap, Relation index, ExplainState *es)
{
	List	   *dpcontext = deparse_context_for(RelationGetRelationName(heap), RelationGetRelid(heap));
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_ptr(Node, q, list_concat_copy(spec->indexqual, spec->vqual))
		if (!reads_json_type(q, NULL))
		{
			const int64 step = list_member_ptr(spec->vqual, q) ? sift_v6_bq_step((const Expr *) q) : 0;

			appendStringInfo(&buf, "%s%s%s", buf.len ? " AND " : "", deparse_expression(as_written(q, index), dpcontext, false, false), step > 0 ? sift_v6_step_text(step) : "");
		}
	if (buf.len)
		ExplainPropertyText("Filters", buf.data, es);
}

/* the search that keeps the rows: one query, or the arms of an OR, an arm the AND of its queries (in parentheses beside other arms) */
static char *
search_desc(const SiftV6Spec *spec)
{
	const char *index = get_rel_name(spec->text_indexoid);
	const bool	nested = list_length(spec->text_queries) > 1;
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_node(List, arm, spec->text_queries)
	{
		const bool	paren = nested && list_length(arm) > 1;

		appendStringInfoString(&buf, buf.len > 0 ? " OR " : "");
		appendStringInfoString(&buf, paren ? "(" : "");
		foreach_ptr(Node, q, arm)
			appendStringInfo(&buf, "%s%s @@> %s", foreach_current_index(q) > 0 ? " AND " : "", index, deparse_expression(q, NIL, false, false));
		appendStringInfoString(&buf, paren ? ")" : "");
	}
	return buf.data;
}

char *
sift_v6_deparse_planning(PlannerInfo *root, Node *e, bool prefix)
{
	PlannedStmt stmt = {.type = T_PlannedStmt,.rtable = root->parse->rtable};
	List	   *names = select_rtable_names_for_explain(stmt.rtable, bms_add_range(NULL, 1, list_length(stmt.rtable)));

	return deparse_expression(e, deparse_context_for_plan_tree(&stmt, names), prefix, false);
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
	const bool	scan = IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan) || IsA(plan, CustomScan) || IsA(plan, FunctionScan);

	if ((scan || IsA(plan, SubqueryScan)) && ((Scan *) plan)->scanrelid > 0)
		*rels = bms_add_member(*rels, ((Scan *) plan)->scanrelid);
	return !IsA(plan, SubqueryScan) && planstate_tree_walker(ps, dimension_rels, rels);
}

static char *
dimension_name(const RangeTblEntry *rte)
{
	if (rte->rtekind == RTE_FUNCTION)
		return psprintf("%s (function)", rte->eref->aliasname);
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
		case SV6J_RANGE_SEMI:
			return " (semi)";
		case SV6J_ANTI_EQUI:
		case SV6J_RANGE_ANTI:
			return " (anti)";
		case SV6J_RIGHT_EQUI:
			return " (preserved)";
		case SV6J_FULL_EQUI:
			return " (full)";
		default:
			return "";
	}
}

typedef struct LookupReads
{
	double		read;
	double		loaded;
	double		index_scans;
} LookupReads;

/* the rows the scans of the lookup plans read, the rows they passed on and the index scans they ran, as the Lookup line of SiftAgg counts them */
static bool
lookup_reads(PlanState *ps, LookupReads *r)
{
	const Plan *plan = ps->plan;
	SiftNodeInstr *in = ps->instrument;

	if (in != NULL)
	{
		InstrEndLoop(in);
		if (IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan))
		{
			r->read += in->ntuples + in->nfiltered1 + in->nfiltered2;
			r->loaded += in->ntuples;
		}
		if (IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapIndexScan))
			r->index_scans += in->nloops;
	}
	return planstate_tree_walker(ps, lookup_reads, r);
}

static void
explain_lookup_reads(CustomScanState *css, const SiftV6Ext *ext, uint64 rows_shared, ExplainState *es)
{
	LookupReads r = {0};
	SiftExplainLine ln;

	if (!es->analyze)
		return;
	for (int d = 0; d < ext->ndims; d++)
		lookup_reads(list_nth(css->custom_ps, d), &r);
	sift_explain_begin(&ln, es, "Lookup");
	sift_explain_int(&ln, "Lookup Rows Read", NULL, "rows read", (int64) r.read, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Lookup Rows Loaded", NULL, "rows loaded", (int64) r.loaded, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Lookup Index Scans", NULL, "index scans", (int64) r.index_scans, SIFT_XI_OPT);
	sift_explain_int(&ln, "Lookup Rows Shared", NULL, "rows shared", (int64) rows_shared, SIFT_XI_OPT);
	sift_explain_end(&ln);
}

void
sift_v6_explain_lookup_joins(CustomScanState *css, const SiftV6Ext *ext, uint64 rows_shared, ExplainState *es)
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
	explain_lookup_reads(css, ext, rows_shared, es);
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

/* a key the scan computes from a column of the relation: `column: expression`, and the step of a calendar key (agg/qual/step.h), whose value the scan takes once for each step */
static void
computed_key(StringInfo buf, Expr *e, List *dpcontext)
{
	List	   *vars = pull_var_clause((Node *) e, 0);
	const Var  *col;
	bool		local;
	const int64 step = sift_v6_calendar_step((const Node *) e, ((const Var *) linitial(vars))->varno, &col, &local);

	appendStringInfo(buf, "%s: %s%s", deparse_expression(linitial(vars), dpcontext, false, false), deparse_expression((Node *) e, dpcontext, true, false),
					 step > 0 ? sift_v6_step_text(step) : "");
}

static bool
key_computed(const SiftV6Spec *spec, int i)
{
	for (int b = 0; b < spec->nbins; b++)
		if (spec->bin[b].key == i)
			return true;
	return spec->key[i].derive > 0;
}

void
sift_v6_explain_group_keys(CustomScanState *css, List *ancestors, const SiftV6Spec *spec, ExplainState *es)
{
	const List *tlist = ((CustomScan *) css->ss.ps.plan)->custom_scan_tlist;
	List	   *dpcontext = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
	StringInfoData buf;

	if (spec->nsets > 0 || spec->affine || spec->nkeys == 0 || list_length(tlist) < spec->nkeys)
		return;
	initStringInfo(&buf);
	for (int i = 0; i < spec->nkeys; i++)
	{
		Expr	   *e = ((TargetEntry *) list_nth(tlist, i))->expr;
		const char *name = col_name(sift_v6_spec_part_oid(spec, 0), sift_v6_spec_part_is_table(spec, 0), &spec->key[i]);

		appendStringInfoString(&buf, i ? ", " : "");
		if (key_computed(spec, i) && spec->ext == NULL && !IsA(e, Var) && pull_var_clause((Node *) e, 0) != NIL)
			computed_key(&buf, e, dpcontext);
		else if (name != NULL)
			appendStringInfoString(&buf, name);
		else
			return;
	}
	ExplainPropertyText("Group Keys", buf.data, es);
}

void
sift_v6_explain_stream_keys(const List *ints, const List *keys, ExplainState *es)
{
	const Oid	relid = (Oid) linitial_int(ints);
	const Oid	heapoid = list_nth_int(ints, 2) ? relid : IndexGetRelation(relid, false);
	List	   *dpcontext = deparse_context_for(get_rel_name(heapoid), heapoid);
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_ptr(Expr, key, keys)
	{
		Expr	   *e = copyObject(key);

		ChangeVarNodes((Node *) e, list_nth_int(ints, 1), 1, 0);
		appendStringInfoString(&buf, buf.len ? ", " : "");
		if (IsA(e, Var))
			appendStringInfoString(&buf, deparse_expression((Node *) e, dpcontext, false, false));
		else
			computed_key(&buf, e, dpcontext);
	}
	ExplainPropertyText("Group Keys", buf.data, es);
}

static bool
collect_aggs(Node *n, List **aggs)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
	{
		*aggs = lappend(*aggs, n);
		return false;
	}
	return expression_tree_walker(n, collect_aggs, aggs);
}

void
sift_v6_explain_pushed_aggs(CustomScanState *css, List *ancestors, ExplainState *es)
{
	List	   *dpcontext = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
	List	   *aggs = NIL;
	StringInfoData buf;

	collect_aggs(sift_vn_rewrite_aggs((Node *) ((CustomScan *) css->ss.ps.plan)->custom_scan_tlist), &aggs);
	initStringInfo(&buf);
	foreach_ptr(Node, a, aggs)
		appendStringInfo(&buf, "%s%s", buf.len ? ", " : "", deparse_expression(a, dpcontext, true, false));
	if (buf.len)
		ExplainPropertyText("Pushed Aggregates", buf.data, es);
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
	explain_topn(sift_v6_spec_part_oid(spec, 0), sift_v6_spec_part_is_table(spec, 0), keys, spec->ntopk, psprintf("%u", spec->topk), es);
}

/*
 * Format 2.0 EXPLAIN (ANALYZE): at most six lines of text, one group of structured properties for each, from the counters of SiftExplainStatsV6 that a run
 * collects in a struct of its own per worker and merges once at the end. A zero is left out of the text unless the line keeps it or EXPLAIN (VERBOSE); the
 * structured formats print every counter under the property named in the call. Lines: Parts, Granules, one of SIP Filter, Hybrid and Scope, Exec, I/O & Mem, Fallback.
 */
typedef struct XLine
{
	ExplainState *es;
	const char *label;
	StringInfoData buf;
} XLine;

static void
xl_begin(XLine *l, ExplainState *es, const char *label)
{
	l->es = es;
	l->label = label;
	initStringInfo(&l->buf);
	if (es->format != EXPLAIN_FORMAT_TEXT)
		ExplainOpenGroup(label, label, true, es);
}

static void
xl_item(XLine *l, const char *fmt, ...) pg_attribute_printf(2, 3);

static void
xl_item(XLine *l, const char *fmt, ...)
{
	va_list		ap;

	if (l->buf.len > 0)
		appendStringInfoChar(&l->buf, ' ');
	va_start(ap, fmt);
	appendStringInfoVA(&l->buf, fmt, ap);
	va_end(ap);
}

static void
xl_int(XLine *l, const char *prop, const char *key, int64 v, bool keep)
{
	if (l->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyInteger(prop, NULL, v, l->es);
	else if (v != 0 || keep || l->es->verbose)
		xl_item(l, "%s=" INT64_FORMAT, key, v);
}

static void
xl_pct(XLine *l, const char *prop, const char *key, double pct)
{
	if (l->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyFloat(prop, "%", pct, 1, l->es);
	else
		xl_item(l, "%s=%.1f%%", key, pct);
}

static void
xl_text(XLine *l, const char *prop, const char *key, const char *v)
{
	if (l->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText(prop, v, l->es);
	else
		xl_item(l, "%s=%s", key, v);
}

static void
size_text(StringInfo s, int64 bytes)
{
	if (bytes >= 1024 * 1024)
		appendStringInfo(s, "%.1fMB", (double) bytes / (1024 * 1024));
	else
		appendStringInfo(s, "%.0fKB", (double) bytes / 1024);
}

static void
xl_bytes(XLine *l, const char *prop, const char *key, int64 bytes, bool keep)
{
	StringInfoData t;

	if (l->es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyInteger(prop, "bytes", bytes, l->es);
		return;
	}
	if (bytes == 0 && !keep && !l->es->verbose)
		return;
	initStringInfo(&t);
	size_text(&t, bytes);
	xl_item(l, "%s=%s", key, t.data);
	pfree(t.data);
}

static void
xl_end(XLine *l)
{
	if (l->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainCloseGroup(l->label, l->label, true, l->es);
	else if (l->buf.len > 0)
		ExplainPropertyText(l->label, l->buf.data, l->es);
	pfree(l->buf.data);
}

static double
pct_of(uint64 part, uint64 whole)
{
	return whole ? 100.0 * (double) part / (double) whole : 0.0;
}

void
sift_agg_explain_v6(ExplainState *es, const SiftExplainStatsV6 *st, const char *simd, int workers_launched, int workers_planned)
{
	XLine		l;
	const bool	hybrid = st->bm25_hits > 0 || st->hybrid_col_grans > 0,
				scope = st->scope_l2_hits > 0 || st->scope_topk_ring_rows > 0 || st->heap_vet_rows > 0;

	ExplainOpenGroup("Sift", "Sift", true, es);
	if (st->parts_scanned + st->parts_pruned_key + st->parts_pruned_time + st->l0_slabs_read > 0)
	{
		xl_begin(&l, es, "Parts");
		xl_int(&l, "Scanned", "scanned", st->parts_scanned, true);
		xl_int(&l, "Pruned By Key", "pruned_key", st->parts_pruned_key, true);
		xl_int(&l, "Pruned By Time", "pruned_time", st->parts_pruned_time, true);
		xl_int(&l, "L0 Slabs Read", "l0_slabs", st->l0_slabs_read, false);
		xl_end(&l);
	}
	if (st->grans_scanned + st->grans_pruned_zone + st->grans_pruned_skip + st->grans_pruned_dict + st->grans_preagg > 0)
	{
		xl_begin(&l, es, "Granules");
		xl_int(&l, "Scanned", "scanned", st->grans_scanned, true);
		xl_int(&l, "Pruned By Zone Map", "pruned_zone", st->grans_pruned_zone, true);
		xl_int(&l, "Pruned By Skip Index", "pruned_skip", st->grans_pruned_skip, true);
		xl_int(&l, "Pruned By Dictionary", "pruned_dict", st->grans_pruned_dict, false);
		xl_int(&l, "Answered By Pre-aggregate", "preagg", st->grans_preagg, false);
		xl_int(&l, "Corrected For Dead Rows", "preagg_delta", st->grans_preagg_delta, false);
		if (es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyUInteger("Filters In Source", NULL, st->src_filters, es);
		else if (st->src_filters > 0)
			xl_item(&l, "(" UINT64_FORMAT " filters in source)", st->src_filters);
		if (es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyUInteger("Rows From Point Directory", NULL, st->point_rows, es);
		else if (st->point_rows > 0)
			xl_item(&l, "from point directory (" UINT64_FORMAT " rows)", st->point_rows);
		xl_end(&l);
	}
	if (st->sip_probes > 0)
	{
		xl_begin(&l, es, "SIP Filter");
		xl_bytes(&l, "Filter Size", "sbbf", st->sip_bytes, true);
		xl_pct(&l, "Pass Rate", "pass", pct_of(st->sip_passed, st->sip_probes));
		if (es->format == EXPLAIN_FORMAT_TEXT)
			xl_item(&l, "(" UINT64_FORMAT "/" UINT64_FORMAT " rows)", st->sip_passed, st->sip_probes);
		else
		{
			ExplainPropertyUInteger("Rows Passed", NULL, st->sip_passed, es);
			ExplainPropertyUInteger("Rows Probed", NULL, st->sip_probes, es);
		}
		xl_int(&l, "Granules Pruned", "granules_pruned", st->sip_grans_pruned, true);
		xl_end(&l);
	}
	if (hybrid)
	{
		xl_begin(&l, es, "Hybrid");
		xl_int(&l, "BM25 Hits", "bm25_hits", st->bm25_hits, true);
		xl_int(&l, "Column Granules", "col_granules", st->hybrid_col_grans, true);
		xl_end(&l);
	}
	if (scope)
	{
		xl_begin(&l, es, "Scope Exec");
		xl_int(&l, "L2 Hits", "L2_hit", st->scope_l2_hits, true);
		xl_int(&l, "Top-N Ring Rows", "topk_ring", st->scope_topk_ring_rows, true);
		xl_int(&l, "Top-N Spill Rows", "spills", st->scope_topk_spill_rows, true);
		xl_int(&l, "Heap Vet Rows", "heap_vet", st->heap_vet_rows, true);
		xl_end(&l);
	}
	xl_begin(&l, es, "Exec");
	xl_text(&l, "SIMD", "SIMD", simd);
	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyInteger("Workers Launched", NULL, workers_launched, es);
		ExplainPropertyInteger("Workers Planned", NULL, workers_planned, es);
	}
	else if (workers_planned > 0)
		xl_item(&l, "workers=%d/%d", workers_launched, workers_planned);
	if (st->cfilt_grans_total > 0)
	{
		xl_pct(&l, "Compressed Filter Hit Rate", "cfilt", pct_of(st->cfilt_grans_hit, st->cfilt_grans_total));
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			XLine		b;
			const struct
			{
				const char *key;
				uint64		n;
			}			enc[] = {{"dict", st->cfilt_dict}, {"rle", st->cfilt_rle}, {"forbit", st->cfilt_forbit},
								 {"delta", st->cfilt_delta}, {"fsst", st->cfilt_fsst}, {"alp", st->cfilt_alp}};

			b.es = es;
			initStringInfo(&b.buf);
			for (int i = 0; i < lengthof(enc); i++)
				if (enc[i].n > 0)
					xl_item(&b, "%s=" UINT64_FORMAT, enc[i].key, enc[i].n);
			if (b.buf.len > 0)
				xl_item(&l, "(%s)", b.buf.data);
			pfree(b.buf.data);
		}
		else
		{
			ExplainPropertyUInteger("Compressed Filter Dictionary Granules", NULL, st->cfilt_dict, es);
			ExplainPropertyUInteger("Compressed Filter RLE Granules", NULL, st->cfilt_rle, es);
			ExplainPropertyUInteger("Compressed Filter FORBIT Granules", NULL, st->cfilt_forbit, es);
			ExplainPropertyUInteger("Compressed Filter Delta Granules", NULL, st->cfilt_delta, es);
			ExplainPropertyUInteger("Compressed Filter FSST Granules", NULL, st->cfilt_fsst, es);
			ExplainPropertyUInteger("Compressed Filter ALP Granules", NULL, st->cfilt_alp, es);
		}
	}
	xl_bytes(&l, "Late Materialization Saved", "late_mat_saved", st->late_mat_saved_bytes, false);
	xl_int(&l, "Late Materialization Early Exits", "late_mat_exits", st->late_mat_early_exits, false);
	xl_int(&l, "Sparse Decodes", "sparse_decodes", st->sparse_decodes, false);
	xl_int(&l, "Adaptive Reorders", "adapt_reorders", st->adapt_reorders, false);
	if (st->group_mode)
		xl_text(&l, "Group Mode", "group", st->group_mode);
	xl_end(&l);
	if (st->par_strategy != NULL)
	{
		xl_begin(&l, es, "Parallel");
		xl_text(&l, "Parallel Strategy", "strategy", st->par_strategy);
		if (st->par_ran)
		{
			xl_int(&l, "Morsels Stolen", "morsels_stolen", st->par_morsels_stolen, true);
			xl_int(&l, "Radix Partitions", "radix_parts", st->par_radix_parts, st->par_radix_parts > 0 || st->par_escalated);
			xl_int(&l, "Partial Merge", "partial_merge_recs", st->par_partial_recs, true);
			xl_bytes(&l, "Partial Merge Size", "partial_merge", st->par_partial_bytes, true);
		}
		xl_end(&l);
	}
	xl_begin(&l, es, "I/O & Mem");
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		StringInfoData t;

		initStringInfo(&t);
		appendStringInfo(&t, UINT64_FORMAT " (" UINT64_FORMAT " blks)", st->aio_batches, st->aio_blocks);
		xl_text(&l, "", "aio_batches", t.data);
		resetStringInfo(&t);
		size_text(&t, st->peak_mem_bytes);
		appendStringInfoChar(&t, '/');
		size_text(&t, st->budget_mem_bytes);
		xl_text(&l, "", "peak_mem", t.data);
		resetStringInfo(&t);
		if (st->spill_count > 0 || es->verbose)
		{
			appendStringInfo(&t, UINT64_FORMAT " (", st->spill_count);
			size_text(&t, st->spill_bytes);
			appendStringInfoChar(&t, ')');
			xl_text(&l, "", "spills", t.data);
		}
		pfree(t.data);
	}
	else
	{
		ExplainPropertyUInteger("AIO Batches", NULL, st->aio_batches, es);
		ExplainPropertyUInteger("AIO Blocks", NULL, st->aio_blocks, es);
		ExplainPropertyUInteger("Peak Memory", "bytes", st->peak_mem_bytes, es);
		ExplainPropertyUInteger("Memory Budget", "bytes", st->budget_mem_bytes, es);
		ExplainPropertyUInteger("Spills", NULL, st->spill_count, es);
		ExplainPropertyUInteger("Spill", "bytes", st->spill_bytes, es);
	}
	xl_end(&l);
	if (st->fallback_reason)
		ExplainPropertyText("Fallback", st->fallback_reason, es);
	if (es->verbose && st->dict_shared_hits + st->dict_shared_misses > 0)
		ExplainPropertyText("Shared Dictionaries", psprintf("hits " INT64_FORMAT ", misses " INT64_FORMAT, st->dict_shared_hits, st->dict_shared_misses), es);
	ExplainCloseGroup("Sift", "Sift", true, es);
}
