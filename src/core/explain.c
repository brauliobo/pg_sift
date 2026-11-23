/*
 * core/explain.c - the counter lines of the custom scans in EXPLAIN (core/explain.h)
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "nodes/plannodes.h"
#include "utils/rel.h"

#include "agg/refuse.h"
#include "col/am.h"
#include "col/pg.h"
#include "core/explain.h"
#include "core/sift.h"

void
sift_explain_begin(SiftExplainLine *ln, ExplainState *es, const char *label)
{
	ln->es = es;
	ln->label = label;
	ln->open = false;
	initStringInfo(&ln->buf);
}

static void
text_item(SiftExplainLine *ln, const char *num, const char *unit, const char *words, int flags)
{
	StringInfo	b = &ln->buf;

	if ((flags & SIFT_XI_SUB) && b->len > 0)
	{
		appendStringInfoString(b, ln->open ? ", " : " (");
		ln->open = true;
	}
	else
	{
		if (ln->open)
			appendStringInfoChar(b, ')');
		ln->open = false;
		if (b->len > 0)
			appendStringInfoString(b, ", ");
	}
	appendStringInfoString(b, num);
	if (unit)
		appendStringInfo(b, " %s", unit);
	if (words)
		appendStringInfo(b, " %s", words);
}

void
sift_explain_int(SiftExplainLine *ln, const char *prop, const char *unit, const char *words, int64 v, int flags)
{
	if (ln->es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (v != 0 || !(flags & SIFT_XI_OPT))
			ExplainPropertyInteger(prop, unit, v, ln->es);
	}
	else if (v != 0 || ln->es->verbose || (flags & SIFT_XI_KEEP))
		text_item(ln, psprintf(INT64_FORMAT, v), unit, words, flags);
}

void
sift_explain_ms(SiftExplainLine *ln, const char *prop, const char *words, double ms, int flags)
{
	if (ln->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyFloat(prop, "ms", ms, 3, ln->es);
	else if (ms != 0 || ln->es->verbose || (flags & SIFT_XI_KEEP))
		text_item(ln, psprintf("%.3f", ms), "ms", words, flags);
}

void
sift_explain_pct(SiftExplainLine *ln, const char *prop, const char *words, double pct, int flags)
{
	if (ln->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyFloat(prop, "%", pct, 1, ln->es);
	else
		text_item(ln, psprintf("%.1f%%", pct), NULL, words, flags);
}

void
sift_explain_end(SiftExplainLine *ln)
{
	if (ln->open)
		appendStringInfoChar(&ln->buf, ')');
	if (ln->buf.len > 0)
		ExplainPropertyText(ln->label, ln->buf.data, ln->es);
	pfree(ln->buf.data);
}

#if PG_VERSION_NUM >= 180000
static int	explain_ext_id;
static explain_per_plan_hook_type prev_per_plan;
static explain_per_node_hook_type prev_per_node;
static const char option_on;	/* the state of an EXPLAIN that was given the option: this address */

static void
option_sift(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	if (!defGetBoolean(opt))
		return;
	SetExplainExtensionState(es, explain_ext_id, (void *) &option_on);
	agg_explain_begin();
}

static bool
wanted(ExplainState *es)
{
	return GetExplainExtensionState(es, explain_ext_id) != NULL;
}

static void
print_refusals(ExplainState *es)
{
	List	   *notes = agg_explain_take();

	if (es->format == EXPLAIN_FORMAT_TEXT)
		foreach_ptr(SiftRefusalNote, n, notes)
			ExplainPropertyText("Sift", psprintf("not used: %s: %s (arm %s)", n->slug, n->reason, n->arm), es);
	else if (notes != NIL)
	{
		ExplainOpenGroup("Sift Refusals", "Sift Refusals", false, es);
		foreach_ptr(SiftRefusalNote, n, notes)
		{
			ExplainOpenGroup("Sift Refusal", NULL, true, es);
			ExplainPropertyText("Arm", n->arm, es);
			ExplainPropertyText("Slug", n->slug, es);
			ExplainPropertyText("Reason", n->reason, es);
			ExplainCloseGroup("Sift Refusal", NULL, true, es);
		}
		ExplainCloseGroup("Sift Refusals", "Sift Refusals", false, es);
	}
}

static void
per_plan(PlannedStmt *pstmt, IntoClause *into, ExplainState *es, const char *query, ParamListInfo params, QueryEnvironment *env)
{
	if (prev_per_plan)
		prev_per_plan(pstmt, into, es, query, params, env);
	if (wanted(es))
		print_refusals(es);
}

/* the scans of the access methods; the custom scans of the library print it where they print their index */
static void
per_node(PlanState *ps, List *ancestors, const char *relationship, const char *plan_name, ExplainState *es)
{
	const Plan *plan = ps->plan;

	if (prev_per_node)
		prev_per_node(ps, ancestors, relationship, plan_name, es);
	if (!wanted(es))
		return;
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			sift_explain_index(es, ((const IndexScan *) plan)->indexid);
			break;
		case T_IndexOnlyScan:
			sift_explain_index(es, ((const IndexOnlyScan *) plan)->indexid);
			break;
		case T_BitmapIndexScan:
			sift_explain_index(es, ((const BitmapIndexScan *) plan)->indexid);
			break;
		default:
			break;
	}
}

void
sift_explain_index(ExplainState *es, Oid indexoid)
{
	Relation	index;
	SiftIndexStats st = {0};
	SiftExplainLine ln;

	if (!wanted(es))
		return;
	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam == sift_catalog()->am)
		sift_bm25_stats(index, &st);
	else if (sift_col_is_index(index) && sift_col_format_major(index) == SIFT_FORMAT2_MAJOR)
		sift_f2_index_stats(index, &st);
	else if (sift_col_is_index(index))
		sift_col_stats(index, &st);
	else
	{
		index_close(index, NoLock);
		return;
	}
	index_close(index, NoLock);
	sift_explain_begin(&ln, es, "Sift Index State");
	sift_explain_int(&ln, "Sift Segments", NULL, "segments", st.segments, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Sift Buffered Rows", NULL, "buffered rows", st.buffered_rows, SIFT_XI_KEEP);
	sift_explain_pct(&ln, "Sift Dead Percent", "dead", st.rows ? 100.0 * st.dead_rows / st.rows : 0, 0);
	sift_explain_end(&ln);
}
#endif

void
sift_explain_init(void)
{
#if PG_VERSION_NUM >= 180000
	explain_ext_id = GetExplainExtensionId("pg_sift");
#if PG_VERSION_NUM >= 190000
	RegisterExtensionExplainOption("sift", option_sift, GUCCheckBooleanExplainOption);
#else
	RegisterExtensionExplainOption("sift", option_sift);
#endif
	prev_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = per_plan;
	prev_per_node = explain_per_node_hook;
	explain_per_node_hook = per_node;
#endif
}
