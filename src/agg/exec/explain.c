/*
 * agg/exec/explain.c - EXPLAIN of a SiftAgg node
 * What the node prints: the source, the keys, the units read, the rows, the lookups, the
 * top-N and the counters of a run.
 */
#include "postgres.h"

#include "optimizer/cost.h"

#include "agg/wide.h"
#include "agg/rows.h"
#include "agg/stats.h"
#include "core/explain.h"
#include "agg/exec/int.h"

/* the name EXPLAIN gives a leaf of a rollup: the field of a list's element, or the column of the table that a leaf of the rows' rollup reads */
static const char *
pa_leaf_name(AggExec *ex, int leaf)
{
	const SiftSrcLeaf *lf = &ex->lay->leaf[leaf];
	AttrNumber	att;

	if (!ex->pa.rows || ex->index == NULL || lf->attno < 1 || lf->attno > ex->index->rd_index->indnatts)
		return lf->name;
	att = ex->index->rd_index->indkey.values[lf->attno - 1];
	return att > 0 ? get_attname(ex->lay->heapoid, att, false) : lf->name;
}

/* the rollup that answers the scan: its dims and measures, one line in text */
static void
explain_pa_rollup(AggExec *ex, ExplainState *es)
{
	StringInfoData d,
				m;

	initStringInfo(&d);
	initStringInfo(&m);
	for (int i = 0; i < ex->pa.ndims; i++)
		appendStringInfo(&d, "%s%s", i ? ", " : "", pa_leaf_name(ex, ex->pa.rdim[i]));
	for (int i = 0; i < ex->pa.nmeas; i++)
		appendStringInfo(&m, "%s%s", i ? ", " : "", pa_leaf_name(ex, ex->pa.rmeas[i]));
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		StringInfoData t;

		initStringInfo(&t);
		if (ex->pa.rows)
			appendStringInfoString(&t, "rows");
		if (d.len > 0)
			appendStringInfo(&t, "%sdims: %s", t.len ? "; " : "", d.data);
		if (m.len > 0)
			appendStringInfo(&t, "%smeasures: %s", t.len ? "; " : "", m.data);
		ExplainPropertyText("Pre-aggregate Rollup", t.data, es);
		pfree(t.data);
	}
	else
	{
		if (ex->pa.rows)
			ExplainPropertyText("Pre-aggregate Rollup", "rows", es);
		if (d.len > 0)
			ExplainPropertyText("Pre-aggregate Dims", d.data, es);
		if (m.len > 0)
			ExplainPropertyText("Pre-aggregate Measures", m.data, es);
	}
	pfree(d.data);
	pfree(m.data);
}

/* what the scan gives a correlated plan, as the output of an InitPlan: the deparser asserts on a Param that nothing generates */
static SubPlan *
lookup_inputs(List *params)
{
	SubPlan    *sp = makeNode(SubPlan);

	sift_initplan_named(sp, "input");
	sp->setParam = params;
	return sp;
}

static void
explain_source(SiftAggSpec *s, ExplainState *es)
{
	ExplainPropertyText("Index", s->nparts > 0 ? "(partitions)" : s->indexoid ? get_rel_name(s->indexoid) : "(table)", es);
	if (s->nparts > 0)
	{
		List	   *names = NIL;

		for (int i = 0; i < s->nparts; i++)
			names = lappend(names, get_rel_name(s->partheap[i]));
		ExplainPropertyList("Partitions", names, es);
	}
	if (s->desc_quals && s->desc_quals[0])
		ExplainPropertyText("Filters", s->desc_quals, es);
	if (s->search)
		ExplainPropertyText("Search", sift_search_describe(s->search), es);
	if (s->topn)
		ExplainPropertyText("Top-N", psprintf("%s (limit %s)", s->desc_topn, sift_topn_limit_text(s->topn)), es);
	if (es->verbose && max_parallel_workers_per_gather > 0 && s->nworkers == 0)
	{
		int			agg;
		const char *why = sift_aggspec_serial_reason(s, &agg);

		if (why == NULL)
			why = s->serial;
		if (why)
			ExplainPropertyText("Serial Because", agg >= 0 ? psprintf("%s: %s", s->agg[agg].desc, why) : why, es);
	}
}

static void
explain_keys(SiftAggSpec *s, ExplainState *es)
{
	StringInfoData buf;
	int			h1 = s->nkeys - (s->nsets > 0),	/* the hidden keys are between the outer ones and the set's */
				h0 = h1 - s->nhidden;

	initStringInfo(&buf);
	for (int j = 0; j < s->nkeys; j++)
		if (j < h0 || j >= h1)
			appendStringInfo(&buf, "%s%s", buf.len ? ", " : "", s->key[j].desc);
	if (buf.len)
		ExplainPropertyText("Group Keys", buf.data, es);
	resetStringInfo(&buf);
	for (int j = h0; j < h1; j++)
		appendStringInfo(&buf, "%s%s", buf.len ? ", " : "", s->key[j].desc);
	if (s->nhidden)
		ExplainPropertyText("Cell Keys", buf.data, es);
	resetStringInfo(&buf);
	for (int i = 0; i < s->naggs; i++)
		appendStringInfo(&buf, "%s%s", i ? ", " : "", s->agg[i].desc);
	if (s->naggs)
		ExplainPropertyText("Pushed Aggregates", buf.data, es);
	if (s->group >= 0)
		ExplainPropertyText("Unnests", "yes", es);
	if (s->njoin)
	{
		resetStringInfo(&buf);
		for (int i = 0; i < s->njoin; i++)
			appendStringInfo(&buf, "%s%s", i ? ", " : "", s->join[i].desc);
		ExplainPropertyText("Lookup Joins", buf.data, es);
	}
}

/* The plan of the lookup i that PostgreSQL plans per scanned row, nested in the scan's */
static void
explain_correlated(CustomScanState *node, int i, ExplainState *es)
{
	SiftAggSpec *s = ((AggExec *) node)->spec;
	EState     *estate = node->ss.ps.state;
	QueryDesc   query = {0};
	PlannedStmt stmt = *estate->es_plannedstmt;
	ExplainState child;
	List       *own;
	bool        correlated = s->join[i].params != NIL;
	char       *group = s->join[i].first.ok && s->join[i].first.use ? "First Row Lookup" : correlated ? "Correlated Lookup" : "Lookup Plan";
	char       *label = psprintf("%s %d", group, i + 1);

	ExplainOpenGroup(group, label, true, es);
	ExplainPropertyText("Name", s->join[i].desc, es);
	/* the statement's identifier and settings are printed once, at the top */
	stmt.queryId = UINT64CONST(0);
	query.plannedstmt = &stmt;
	query.planstate = list_nth(estate->es_subplanstates, s->join[i].subplan - 1);
	query.estate = estate;
	child = *es;
	child.settings = false;
	child.printed_subplans = NULL;
	own = query.planstate->plan->initPlan;
	if (correlated || s->join[i].first.params != NIL)
		query.planstate->plan->initPlan = lcons(lookup_inputs(correlated ? s->join[i].params : s->join[i].first.params), own);
	ExplainPrintPlan(&child, &query);
	query.planstate->plan->initPlan = own;
	es->indent = child.indent;
	es->grouping_stack = child.grouping_stack;
	ExplainCloseGroup(group, label, true, es);
}

/* The lookup tables a run loaded and the key filters and semi tables it built from them */
static void
explain_lookup(AggExec *c, SiftAggSpec *s, ExplainState *es)
{
	SiftExplainLine ln;

	sift_explain_begin(&ln, es, "Lookup");
	if (s->njoin > 0)
	{
		sift_explain_int(&ln, "Lookup Rows Read", NULL, "rows read", c->lookup_read, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Lookup Rows Loaded", NULL, "rows loaded", c->lookup_loaded, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Lookup Rows Shared", NULL, "rows shared", c->lookup_shared, SIFT_XI_OPT);
		sift_explain_int(&ln, "Lookup Index Scans", NULL, "index scans", c->lookup_index, SIFT_XI_OPT);
		sift_explain_int(&ln, "Lookup Column Loads", NULL, "column loads", c->lookup_cols, SIFT_XI_OPT);
		if (es->timing)
			sift_explain_ms(&ln, "Lookup Load Time", "to load", c->lookup_us / 1000.0, SIFT_XI_KEEP);
	}
	if (es->verbose)
	{
		int64		hits = sift_stat_v[SIFT_STAT_DICT_SHARED_HITS] - c->dict_shared0[0],
					misses = sift_stat_v[SIFT_STAT_DICT_SHARED_MISSES] - c->dict_shared0[1];

		if (hits + misses > 0)
			ExplainPropertyText("Shared Dictionaries", psprintf("hits " INT64_FORMAT ", misses " INT64_FORMAT, hits, misses), es);
	}
	sift_explain_int(&ln, "Child Blocks Prefetched", NULL, "child blocks prefetched", c->child_warmed, SIFT_XI_OPT);
	sift_explain_int(&ln, "Join Key Filters", NULL, "key filters", c->njfilter, SIFT_XI_OPT);
	sift_explain_int(&ln, "Join Key Passes", NULL, "key passes", c->njpass, SIFT_XI_OPT);
	sift_explain_int(&ln, "Shared Lookup Tables", NULL, "shared tables", c->nshared, SIFT_XI_OPT);
	sift_explain_int(&ln, "Cached Lookup Tables", NULL, "cached tables", c->ncached, SIFT_XI_OPT);
	sift_explain_int(&ln, "First Row Lookup Tables", NULL, "first row tables", c->nfirst, SIFT_XI_OPT);
	if (c->first_hit > 0 || c->first_differ > 0)
	{
		sift_explain_int(&ln, "First Row Lookups", NULL, "first row lookups", c->first_hit, SIFT_XI_KEEP);
		sift_explain_int(&ln, "First Row Keys Differ", NULL, "first row keys differ", c->first_differ, SIFT_XI_KEEP);
	}
	if (c->nsemi > 0)
	{
		sift_explain_int(&ln, "Semi Lookup Tables", NULL, "semi tables", c->nsemi, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Semi Lookup Table Size", "kB", NULL, (c->semi_bytes + 1023) / 1024, SIFT_XI_SUB | SIFT_XI_KEEP);
	}
	sift_explain_end(&ln);
}

/* The units a run read, pruned, and answered from the pre-aggregates */
static void
explain_units(AggExec *ex, AggExec *c, SiftSrcCounters *cc, ExplainState *es)
{
	SiftExplainLine ln;

	sift_aggbe_explain(c->be, c->be_state, es, c->n_units);
	sift_explain_begin(&ln, es, "Units");
	sift_explain_int(&ln, "Units", NULL, "read", c->n_units, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Partitions Removed", NULL, "partitions removed", c->parts_pruned, SIFT_XI_OPT);
	sift_explain_int(&ln, "Units Pruned", NULL, "pruned", cc->units_pruned, 0);
	sift_explain_int(&ln, "Units Pruned By Set Summary", NULL, "by set summary", cc->units_set_pruned, SIFT_XI_SUB | SIFT_XI_OPT);
	if (cc->point_units > 0)
	{
		sift_explain_int(&ln, "Units From Point Directory", NULL, "from point directory", cc->point_units, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Rows From Point Directory", NULL, "rows", cc->point_rows, SIFT_XI_SUB | SIFT_XI_KEEP);
	}
	sift_explain_int(&ln, "Units Filtered By Sorted Run", NULL, "filtered by sorted run", cc->run_windows, SIFT_XI_OPT);
	if (c->recheck)
		sift_explain_int(&ln, "Units Read Again From The Heap", NULL, "read again from the heap", cc->recheck_units, SIFT_XI_KEEP);
	for (int i = 0; i < ex->spec->naggs; i++)
		if (ex->spec->agg[i].kind == SAA_GEN && ex->spec->agg[i].gen->hll >= 0)
		{
			ExplainPropertyText("Approximate Distinct", "sift.approx_count_distinct: HyperLogLog, 0.81% relative standard error", es);
			sift_explain_int(&ln, "Units Without The Sketch", NULL, "without the sketch", c->n_units - c->n_pre_units, SIFT_XI_KEEP);
			break;
		}
	if (!c->pa_on && es->verbose)
		ExplainPropertyText("Pre-aggregates", sift_pawhy_name(c->pa_why), es);
	if (c->pa_on)
	{
		sift_explain_int(&ln, "Units From Pre-aggregates", NULL, "from pre-aggregates", c->n_pre_units, SIFT_XI_KEEP);
		if (c->n_exc_units > 0 || c->n_exc_scan > 0)
		{
			sift_explain_int(&ln, "Units Net Of Exceptions", NULL, "net of exceptions", c->n_exc_units, SIFT_XI_KEEP);
			sift_explain_int(&ln, "Units With Exceptions Scanned", NULL, "with exceptions scanned", c->n_exc_scan, SIFT_XI_KEEP);
		}
		if (c->n_dead_units > 0 || c->n_dead_scan > 0)
		{
			sift_explain_int(&ln, "Units Net Of Dead Rows", NULL, "net of dead rows", c->n_dead_units, SIFT_XI_KEEP);
			sift_explain_int(&ln, "Units With Dead Rows Scanned", NULL, "with dead rows scanned", c->n_dead_scan, SIFT_XI_KEEP);
		}
		if (c->super_on)
			sift_explain_int(&ln, "Superunits", NULL, "superunits", c->n_super, 0);
		if (PA_ROLLUP(ex->pa))
		{
			explain_pa_rollup(ex, es);
			sift_explain_int(&ln, "Units Without The Rollup", NULL, "without the rollup", c->n_rollup_miss, 0);
			sift_explain_int(&ln, "Units Scanned By Cost", NULL, "scanned by cost", c->n_rollup_cost, SIFT_XI_OPT);
			sift_explain_int(&ln, "Rollup Refresh Queued", NULL, "refreshes queued", c->n_rollup_queued, SIFT_XI_OPT);
		}
	}
	sift_explain_end(&ln);
}

static void
explain_rows(AggExec *ex, AggExec *c, SiftSrcCounters *cc, ExplainState *es)
{
	SiftExplainLine ln;

	sift_explain_begin(&ln, es, "Rows");
	sift_explain_int(&ln, "Rows Scanned", NULL, "scanned", c->n_rows, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Filters In Source", NULL, "filters in source", c->nfilter, SIFT_XI_SUB | SIFT_XI_OPT);
	sift_explain_int(&ln, "Type Filters In Source", NULL, "type filters in source", c->ntype, SIFT_XI_SUB | SIFT_XI_OPT);
	sift_explain_int(&ln, "Wide Filters In Source", NULL, "wide filters in source", c->nwide, SIFT_XI_SUB | SIFT_XI_OPT);
	sift_explain_int(&ln, "Tuples Aggregated", NULL, "aggregated", c->n_tuples, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Dense Chunks", NULL, "dense chunks", c->n_dense, 0);
	sift_explain_int(&ln, "Combo Caches Restarted", NULL, "combo caches restarted", c->n_combo_trim, SIFT_XI_OPT);
	sift_explain_int(&ln, "Rows Vetted By Heap", NULL, "vetted by heap", cc->rows_vetted, 0);
	sift_explain_int(&ln, "Heap Pages", NULL, "heap pages", cc->heap_pages, 0);
	if (c->recheck)
		sift_explain_int(&ln, "Rows Read Again From The Heap", NULL, "re-read from the heap", cc->rechecked, SIFT_XI_KEEP);
	if (ex->spec->search)
		sift_explain_int(&ln, "Search Matches", NULL, "search matches", (c->search ? sift_search_matches(c->search) : 0) + (c->par ? c->par->matches : 0), SIFT_XI_KEEP);
	sift_explain_int(&ln, "Groups", NULL, "groups", ex->n_groups, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Group Order Keys", NULL, "order keys", ex->spec->nord, SIFT_XI_SUB | SIFT_XI_OPT);
	if (ex->spec->gtopn_k > 0)
		sift_explain_int(&ln, "Groups Dropped By Top-N", NULL, "dropped by top-N", ex->n_gpruned, SIFT_XI_KEEP);
	if (part_flush_on(ex))
		ExplainPropertyText("Groups Finished", "per partition", es);
	sift_explain_int(&ln, "Ordered Sets Spilled", NULL, "ordered sets spilled", c->n_osa_spilled, SIFT_XI_OPT);
	sift_explain_int(&ln, "Group Table Spills", NULL, "group table spills", c->n_spills, SIFT_XI_OPT);
	sift_explain_int(&ln, "Distinct Sets Spilled", NULL, "distinct sets spilled", c->n_dist_spilled, SIFT_XI_OPT);
	sift_explain_int(&ln, "Distinct Chunks Merged", NULL, "distinct chunks merged", c->n_dist_chunks, SIFT_XI_OPT);
	if (c->ndsorder)
	{
		sift_explain_int(&ln, "Distinct Stream Superblocks", NULL, "distinct stream superblocks", c->ndsorder, SIFT_XI_KEEP);
		if (c->dsabort)
			ExplainPropertyBool("Distinct Stream Given Up", true, es);
	}
	sift_explain_end(&ln);
}

/* The top-N scan's skips and reads */
static void
explain_topn(AggExec *ex, ExplainState *es)
{
	SiftExplainLine ln;
	const SiftTopN *t = ex->topn;

	sift_explain_begin(&ln, es, "Top-N Run");
	sift_explain_int(&ln, "Superblocks Skipped By Top-N", NULL, "superblocks skipped", sift_topn_skipped(t), SIFT_XI_KEEP);
	sift_explain_int(&ln, "Top-N Candidates", NULL, "candidates", sift_topn_candidates(t), SIFT_XI_OPT);
	sift_explain_int(&ln, "Top-N Restarts", NULL, "restarts", ex->widen, SIFT_XI_OPT);
	sift_explain_int(&ln, "Units Read By Sorted Run", NULL, "units read by sorted run", sift_topn_runs_read(t), SIFT_XI_OPT);
	sift_explain_int(&ln, "Units Read Without Filters", NULL, "units read without filters", sift_topn_runs_lazy(t), SIFT_XI_OPT);
	sift_explain_int(&ln, "Units Answered By Run Head", NULL, "units answered by run head", sift_topn_heads_used(t), SIFT_XI_OPT);
	sift_explain_int(&ln, "Units Read From Their Ends", NULL, "units read from their ends", sift_topn_mono_used(t), SIFT_XI_OPT);
	if (sift_topn_ord_units(t, 0) + sift_topn_ord_units(t, 1) > 0)
		ExplainPropertyText("Top-N Units By Order Code", psprintf("exact " INT64_FORMAT ", prefix " INT64_FORMAT ", bytes " INT64_FORMAT, sift_topn_ord_units(t, 0), sift_topn_ord_units(t, 1), sift_topn_ord_units(t, 2)), es);
	if (sift_topn_gordered(t))
		ExplainPropertyBool("Global Order", true, es);
	sift_explain_int(&ln, "Heap Fetches", NULL, "heap fetches", sift_topn_fetched(t), SIFT_XI_KEEP);
	if (sift_topn_sorted(t) >= 0)
		sift_explain_int(&ln, "Top-N Sorted Rows", NULL, "sorted rows", sift_topn_sorted(t), SIFT_XI_KEEP);
	sift_explain_end(&ln);
}

/* EXPLAIN ANALYZE: what the run did; the arms of a fused run report on its one pass */
static void
explain_run(AggExec *ex, ExplainState *es)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcCounters cc;
	AggExec    *c = ex->fuse ? ex->fuse->F : ex;

	run_counters(c, &cc);
	if (ex->fuse)
		ExplainPropertyInteger("Arms In One Pass", NULL, ex->fuse->F->spec->nsets, es);
	ExplainPropertyText("SIMD", sift_simd_level_name(sift_cpu.active), es);
	if (ex->fold)
		ExplainPropertyInteger("Sets Summed From One", NULL, s->nsets - 1, es);
	if (s->nworkers > 0)
		ExplainPropertyInteger("Workers Launched", NULL, c->par ? c->par->nlaunched : 0, es);
	if (s->topn && ex->topn)
		explain_topn(ex, es);
	explain_lookup(c, s, es);
	explain_units(ex, c, &cc, es);
	explain_rows(ex, c, &cc, es);
}

void
agg_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	AggExec    *ex = (AggExec *) node;
	SiftAggSpec *s = ex->spec;

	explain_source(s, es);
	explain_keys(s, es);
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].subplan > 0)
			explain_correlated(node, i, es);
	if (s->nworkers > 0)
		ExplainPropertyInteger("Workers Planned", NULL, s->nworkers, es);
	if (s->wide_unit_rows > 0)
		ExplainPropertyText("Workers Priced For", "the rows the scan reads", es);
	if (es->analyze && ex->started)
		explain_run(ex, es);
}
