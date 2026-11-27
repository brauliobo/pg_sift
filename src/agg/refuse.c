/*
 * agg/refuse.c - sift.explain_refusals and sift.explain_why: why a query kept the normal plan.
 *
 * sift.explain_why plans a SELECT without running it and reports, as the rows of the registry (refuse.h), whether the plan reads a sift_col index and, if not, why. With
 * sift.explain_refusals on the reasons are reported as a notice, and with sift.count_refusals they are counted in sift.stats(); EXPLAIN (SIFT) keeps them for the report of its plan.
 */
#include "postgres.h"

#include <math.h>

#include "access/tupdesc.h"
#include "access/table.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/extensible.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "commands/defrem.h"
#include "commands/prepare.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "utils/plancache.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "agg/refuse.h"
#include "agg/stats.h"
#include "core/limits.h"
#include "core/sift.h"

PG_FUNCTION_INFO_V1(sift_agg_explain_why);
PG_FUNCTION_INFO_V1(sift_agg_refusal_slugs);

/* sift_explain_refusals: the reasons are noted (the notice, sift.count_refusals or sift.explain_why asked for them) */
static bool sift_explain_refusals = false;
static bool notice_refusals = false;
static bool count_refusals = false;
static bool explaining = false;			/* EXPLAIN (SIFT): the statement being explained is planned, its refusals are kept for the plan's report */
static List *explained;
static MemoryContext explain_cx;			/* the context of the EXPLAIN statement: it holds explained until the plan is printed */
static Tuplestorestate *why_store;
static TupleDesc why_desc;
static MemoryContext why_cx;				/* the context of the sift.explain_why call: it holds the arms of the statement it plans */

/* an arm of the aggregate that priced a path for the statement sift.explain_why plans: the cheapest of its paths, of the normal plan beside it, and whether the plan takes it */
typedef struct ArmPrice
{
	const char *name;
	double		scan;
	double		normal;
	bool		used;
	AggRefusal	why;
	const char *detail;			/* what the arm refused: an expression of the statement, a capability; NULL: none named */
} ArmPrice;

static List *arms;

#define AGG_REFUSAL_ROW(id, slug, text, limit, by_design, action) {slug, text, limit, by_design, action},
static const struct
{
	const char *slug;
	const char *text;
	bool		limit;
	bool		by_design;
	const char *action;
}			refusals[] = {{NULL, NULL, false, false, NULL}, SIFT_AGG_REFUSALS(AGG_REFUSAL_ROW)};

/* the ids of core/limits.h: the name sift.stats counts a refusal by a limit under */
static const char *const limit_names[] = {
#define LIMIT_NAME(id, macro, file, value, unit, what, behaviour, setting, hooked) #id,
	SIFT_LIMITS(LIMIT_NAME)
#undef LIMIT_NAME
};


static void
explain_note(const char *arm, const char *slug, const char *reason)
{
	MemoryContext old = MemoryContextSwitchTo(explain_cx);
	SiftRefusalNote *n = palloc(sizeof(SiftRefusalNote));

	*n = (SiftRefusalNote) {pstrdup(arm), slug, pstrdup(reason)};
	explained = lappend(explained, n);
	MemoryContextSwitchTo(old);
}

static void
emit(const char *name, AggRefusal why, const char *detail, bool used, double scan, double normal)
{
	const char *slug = refusals[why].slug;
	const char *action = refusals[why].action;
	const char *reason = why == AGG_REFUSE_NONE ? NULL : detail ? psprintf("%s (%s)", refusals[why].text, detail) : refusals[why].text;

	if (why_store != NULL)
	{
		Datum		v[7];
		bool		n[7] = {false, false, used, used, used, scan == 0, scan == 0 || normal < 0};

		v[0] = CStringGetTextDatum(name);
		v[1] = BoolGetDatum(used);
		v[2] = used ? (Datum) 0 : CStringGetTextDatum(slug);
		v[3] = used ? (Datum) 0 : CStringGetTextDatum(reason);
		v[4] = used ? (Datum) 0 : CStringGetTextDatum(action);
		v[5] = Float8GetDatum(scan);
		v[6] = Float8GetDatum(normal);
		tuplestore_putvalues(why_store, why_desc, v, n);
	}
	else if (!used && explaining)
		explain_note(name, slug, reason);
	else if (!used && notice_refusals)
		ereport(NOTICE, (errmsg("SiftAgg not used: %s: %s", slug, reason), errdetail("arm: %s", name), errhint("%s", action)));
}

/* a decision about the plan of the statement as a whole */
static void
agg_report(const char *name, AggRefusal why, const char *detail, bool used, double scan, double normal)
{
	if (!sift_explain_refusals)
		return;
	if (!used && why != AGG_REFUSE_NONE)
		sift_stats_refusal(why, -1);
	emit(name, why, detail, used, scan, normal);
}

void
sift_agg_refuse(const char *arm, AggRefusal why, const char *detail, double scan, double normal)
{
	agg_report(arm, why, detail, false, scan, normal);
}

static ArmPrice *
arm_of(const char *name)
{
	MemoryContext old;
	ArmPrice   *a;

	foreach_ptr(ArmPrice, x, arms)
		if (strcmp(x->name, name) == 0)
			return x;
	old = MemoryContextSwitchTo(why_cx);
	a = palloc(sizeof(ArmPrice));
	*a = (ArmPrice) {name, get_float8_infinity(), -1, false, AGG_REFUSE_NONE, NULL};
	arms = lappend(arms, a);
	MemoryContextSwitchTo(old);
	return a;
}

void
sift_agg_arm_priced(const char *arm, double scan, double normal)
{
	ArmPrice   *a;

	if (why_store == NULL)
		return;
	a = arm_of(arm);
	a->scan = Min(a->scan, scan);
	if (normal >= 0)
		a->normal = a->normal < 0 ? normal : Min(a->normal, normal);
}

void
sift_agg_arm_used(const char *arm)
{
	if (why_store != NULL)
		arm_of(arm)->used = true;
}

void
sift_agg_arm_refused_by(const char *arm, AggRefusal why, const char *detail)
{
	ArmPrice   *a;

	if (why_store == NULL)
		return;
	a = arm_of(arm);
	if (a->why != AGG_REFUSE_NONE)
		return;
	a->why = why;
	a->detail = detail != NULL ? MemoryContextStrdup(why_cx, detail) : NULL;
}

void
sift_agg_arm_refused(const char *arm, AggRefusal why)
{
	sift_agg_arm_refused_by(arm, why, NULL);
}

/* sift.explain_why is planning the statement: the rows of its arms are kept */
bool
sift_agg_arms_noted(void)
{
	return why_store != NULL;
}

static AggRefusal
arm_why(const ArmPrice *a)
{
	if (a->used)
		return AGG_REFUSE_NONE;
	return isinf(a->scan) ? a->why : AGG_REFUSE_COST;
}

/* the rows of the arms that priced a path for the statement just planned; false: none did */
static bool
why_arms(void)
{
	foreach_ptr(ArmPrice, a, arms)
		agg_report(a->name, arm_why(a), arm_why(a) == a->why ? a->detail : NULL, a->used, isinf(a->scan) ? 0 : a->scan, a->normal);
	return arms != NIL;
}

static void
set_explain_refusals(void)
{
	sift_explain_refusals = notice_refusals || count_refusals || explaining;
}

/* the statement EXPLAIN (SIFT) was given to is planned: its refusals are kept until the plan is printed */
void
agg_explain_begin(void)
{
	explaining = true;
	explained = NIL;
	explain_cx = CurrentMemoryContext;
	set_explain_refusals();
}

/* the refusals of the plan for its report; the next statement starts with none */
List *
agg_explain_take(void)
{
	List	   *notes = explained;

	explained = NIL;
	return notes;
}

/* an error in the middle of a plan leaves the list in a context that is gone */
static void
forget_arms(void)
{
	explained = NIL;
	explaining = false;
	arms = NIL;
	set_explain_refusals();
}

static void
forget_arms_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT)
		forget_arms();
}

static void
forget_arms_subxact(SubXactEvent event, SubTransactionId my_subid, SubTransactionId parent_subid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		forget_arms();
}

/* the nodes that fold the groups from a sift_col index: SiftAggV6 and its variants, SiftHybridAgg */
static bool
is_agg_scan(const CustomScan *cs)
{
	const char *name = cs->methods->CustomName;

	return strncmp(name, "SiftAggV6", strlen("SiftAggV6")) == 0 || strcmp(name, "SiftHybridAgg") == 0;
}

/* does the plan read a sift_col index (an index, index-only or bitmap scan of the access method, or a node that aggregates from one) anywhere, in a child, a subquery or an initplan? */
static bool
plan_reads_sift_index(const Plan *p, const List *subplans, Oid amoid)
{
	List	   *kids = NIL;

	if (p == NULL)
		return false;
	switch (nodeTag(p))
	{
		case T_IndexScan:
			if (get_rel_relam(((const IndexScan *) p)->indexid) == amoid)
				return true;
			break;
		case T_IndexOnlyScan:
			if (get_rel_relam(((const IndexOnlyScan *) p)->indexid) == amoid)
				return true;
			break;
		case T_BitmapIndexScan:
			if (get_rel_relam(((const BitmapIndexScan *) p)->indexid) == amoid)
				return true;
			break;
		case T_Append:
			kids = ((const Append *) p)->appendplans;
			break;
		case T_MergeAppend:
			kids = ((const MergeAppend *) p)->mergeplans;
			break;
		case T_BitmapAnd:
			kids = ((const BitmapAnd *) p)->bitmapplans;
			break;
		case T_BitmapOr:
			kids = ((const BitmapOr *) p)->bitmapplans;
			break;
		case T_SubqueryScan:
			if (plan_reads_sift_index(((const SubqueryScan *) p)->subplan, subplans, amoid))
				return true;
			break;
		case T_CustomScan:
			if (is_agg_scan((const CustomScan *) p))
				return true;
			kids = ((const CustomScan *) p)->custom_plans;
			break;
		default:
			break;
	}
	foreach_ptr(Plan, k, kids)
		if (plan_reads_sift_index(k, subplans, amoid))
			return true;
	foreach_node(SubPlan, sp, p->initPlan)
		if (plan_reads_sift_index(list_nth(subplans, sp->plan_id - 1), subplans, amoid))
			return true;
	return plan_reads_sift_index(p->lefttree, subplans, amoid) || plan_reads_sift_index(p->righttree, subplans, amoid);
}

static bool
plan_stmt_reads_sift_index(const PlannedStmt *ps, Oid amoid)
{
	return plan_reads_sift_index(ps->planTree, ps->subplans, amoid);
}

/* is a table of the statement a sift_table, or has it a sift_col index? */
static bool
tables_have_source(const PlannedStmt *ps, Oid amoid)
{
	foreach_node(RangeTblEntry, rte, ps->rtable)
	{
		Relation	rel;
		bool		found;

		if (rte->rtekind != RTE_RELATION)
			continue;
		rel = table_open(rte->relid, AccessShareLock);
		found = rel->rd_rel->relam == sift_catalog()->tbl_am;
		foreach_oid(ix, RelationGetIndexList(rel))
			found |= get_rel_relam(ix) == amoid;
		table_close(rel, AccessShareLock);
		if (found)
			return true;
	}
	return false;
}

/*
 * The statement has no aggregate, so no arm of the pushdown looked at it: the row "index" says whether the plan reads a sift_col index, and if not why. The statement is
 * planned again with the sequential scan off: a plan with a sift_col index then is the one the normal plan was estimated cheaper than (cost, with both costs);
 * none means no table is a sift_table or has such an index (no_source), or no condition of the statement is one the index answers (where).
 */
static void
why_index_arm(RawStmt *raw, const char *sql, const PlannedStmt *ps, const Oid *ptypes, int nparams, ParamListInfo params)
{
	Oid			amoid = sift_catalog()->col_am;
	int			nest;
	PlannedStmt *again = NULL;

	if (!OidIsValid(amoid) || ps->commandType != CMD_SELECT)
		return;
	if (plan_stmt_reads_sift_index(ps, amoid))
	{
		agg_report("index", AGG_REFUSE_NONE, NULL, true, ps->planTree->total_cost, -1);
		return;
	}
	if (!tables_have_source(ps, amoid))
	{
		agg_report("index", AGG_REFUSE_NO_SOURCE, NULL, false, 0, -1);
		return;
	}
	nest = NewGUCNestLevel();
	PG_TRY();
	{
		(void) set_config_option("enable_seqscan", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
		foreach_ptr(PlannedStmt, p2, pg_plan_queries(pg_analyze_and_rewrite_fixedparams(copyObject(raw), sql, ptypes, nparams, NULL), sql, CURSOR_OPT_PARALLEL_OK, params))
			if (p2->commandType == CMD_SELECT)
				again = p2;
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(false, nest);
	}
	PG_END_TRY();
	if (again != NULL && plan_stmt_reads_sift_index(again, amoid))
		agg_report("index", AGG_REFUSE_COST, NULL, false, again->planTree->total_cost, ps->planTree->total_cost);
	else
		agg_report("index", AGG_REFUSE_WHERE, NULL, false, 0, -1);
}

/* The values of the EXECUTE for the parameters of the prepared statement, as commands/prepare.c evaluates them (that function is not exported) */
static ParamListInfo
why_params(ParseState *pstate, const PreparedStatement *entry, List *given, EState *estate)
{
	const Oid  *types = entry->plansource->param_types;
	List	   *exprs = copyObject(given);
	ParamListInfo params;
	ExprContext *econtext;
	int			i = 0;
	ListCell   *lc;

	if (list_length(exprs) != entry->plansource->num_params)
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("wrong number of parameters for prepared statement \"%s\"", entry->stmt_name),
						errdetail("Expected %d parameters but got %d.", entry->plansource->num_params, list_length(exprs))));
	params = makeParamList(entry->plansource->num_params);
	foreach(lc, exprs)
	{
		Node	   *expr = transformExpr(pstate, lfirst(lc), EXPR_KIND_EXECUTE_PARAMETER);
		Oid			given_type = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, given_type, types[i], -1, COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
		if (expr == NULL)
			ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("parameter $%d of type %s cannot be coerced to the expected type %s", i + 1,
							format_type_be(given_type), format_type_be(types[i])), parser_errposition(pstate, exprLocation(lfirst(lc)))));
		assign_expr_collations(pstate, expr);
		lfirst(lc) = ExecPrepareExpr((Expr *) expr, estate);
		i++;
	}
	econtext = GetPerTupleExprContext(estate);
	i = 0;
	foreach(lc, exprs)
	{
		bool		isnull;

		params->params[i].value = ExecEvalExprSwitchContext((ExprState *) lfirst(lc), econtext, &isnull);
		params->params[i].isnull = isnull;
		params->params[i].pflags = PARAM_FLAG_CONST;
		params->params[i].ptype = types[i];
		i++;
	}
	return params;
}

/*
 * EXECUTE of a prepared SELECT: planned for the values it is given (a custom plan, so that the planner runs and the reasons are recorded, whatever plan_cache_mode says), the way the
 * statement runs on its next execution.
 */
static void
why_execute(const ExecuteStmt *es, const char *sql)
{
	PreparedStatement *entry = FetchPreparedStatement(es->name, true);
	CachedPlanSource *psrc = entry->plansource;
	ParseState *pstate = make_parsestate(NULL);
	EState	   *estate = CreateExecutorState();
	ParamListInfo params = NULL;
	CachedPlan *cplan;
	int			nest;

	if (psrc->commandTag != CMDTAG_SELECT)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift.explain_why takes the EXECUTE of a prepared SELECT"),
						errhint("The prepared statement \"%s\" is not a SELECT.", es->name)));
	pstate->p_sourcetext = sql;
	if (psrc->num_params > 0 || es->params != NIL)
		params = why_params(pstate, entry, es->params, estate);
	nest = NewGUCNestLevel();
	PG_TRY();
	{
		(void) set_config_option("plan_cache_mode", "force_custom_plan", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
		sift_explain_refusals = true;
		cplan = GetCachedPlan(psrc, params, CurrentResourceOwner, NULL);
		foreach_ptr(PlannedStmt, ps, cplan->stmt_list)
			if (ps->commandType == CMD_SELECT)
			{
				ExecCheckPermissions(ps->rtable, ps->permInfos, true);
				if (!why_arms() && tuplestore_tuple_count(why_store) == 0)
					why_index_arm(copyObject(psrc->raw_parse_tree), psrc->query_string, ps, psrc->param_types, psrc->num_params, params);
			}
		ReleaseCachedPlan(cplan, CurrentResourceOwner);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(false, nest);
	}
	PG_END_TRY();
	FreeExecutorState(estate);
}

Datum
sift_agg_explain_why(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		saved = sift_explain_refusals;
	List	   *trees = pg_parse_query(sql),
			   *queries;
	RawStmt    *raw,
			   *again_raw;

	if (list_length(trees) != 1 || !(IsA((raw = linitial_node(RawStmt, trees))->stmt, SelectStmt) || IsA(raw->stmt, ExecuteStmt)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift.explain_why takes one SELECT statement or the EXECUTE of a prepared one"),
						errhint("Pass the text of a single SELECT, without a second statement; the SELECT of an INSERT, CREATE TABLE AS or a DELETE is explained by itself.")));
	InitMaterializedSRF(fcinfo, 0);
	PG_TRY();
	{
		why_store = rsi->setResult;
		why_desc = rsi->setDesc;
		why_cx = CurrentMemoryContext;
		arms = NIL;
		sift_explain_refusals = true;
		if (IsA(raw->stmt, ExecuteStmt))
			why_execute((const ExecuteStmt *) raw->stmt, sql);
		else
		{
			again_raw = copyObject(raw);	/* analysis rewrites the sub-selects of the tree in place */
			queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
			foreach_ptr(PlannedStmt, ps, pg_plan_queries(queries, sql, CURSOR_OPT_PARALLEL_OK, NULL))
				if (ps->commandType == CMD_SELECT)
				{
					ExecCheckPermissions(ps->rtable, ps->permInfos, true);
					if (!why_arms() && tuplestore_tuple_count(why_store) == 0)
						why_index_arm(again_raw, sql, ps, NULL, 0, NULL);
				}
		}
	}
	PG_FINALLY();
	{
		sift_explain_refusals = saved;
		why_store = NULL;
		forget_arms();
	}
	PG_END_TRY();
	return (Datum) 0;
}

static void
assign_notice(bool newval, void *extra)
{
	sift_explain_refusals = newval || count_refusals || explaining;
}

static void
assign_count(bool newval, void *extra)
{
	sift_explain_refusals = newval || notice_refusals || explaining;
}

void
sift_agg_refuse_init(void)
{
	RegisterXactCallback(forget_arms_xact, NULL);
	RegisterSubXactCallback(forget_arms_subxact, NULL);
	DefineCustomBoolVariable("sift.explain_refusals",
							 "Reports as a notice why a query kept the normal plan instead of Custom Scan (SiftAgg): the first reason each candidate path gave up for.",
							 NULL, &notice_refusals, false,
							 PGC_USERSET, 0, NULL, assign_notice, NULL);
	DefineCustomBoolVariable("sift.count_refusals",
							 "Counts in sift.stats() why queries kept the normal plan (refused.<slug>, limit.<id>), without the notice of sift.explain_refusals.",
							 NULL, &count_refusals, false,
							 PGC_SUSET, 0, NULL, assign_count, NULL);
}

const char *
sift_agg_limit_name(int lim)
{
	return limit_names[lim];
}

/* The row i (from 1) of the registry for sift.help(); false past the last */
bool
sift_agg_refusal_row(int i, const char **slug, const char **text, const char **action, bool *limit)
{
	if (i < 1 || i >= AGG_REFUSE_COUNT)
		return false;
	*slug = refusals[i].slug;
	*text = refusals[i].text;
	*action = refusals[i].action;
	*limit = refusals[i].limit;
	return true;
}

/* sift.refusal_slugs(): the registry of refuse.h, for the tools that read sift.stats() or sift.explain_why() */
Datum
sift_agg_refusal_slugs(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 1; i < AGG_REFUSE_COUNT; i++)
	{
		Datum		v[4] = {CStringGetTextDatum(refusals[i].slug), CStringGetTextDatum(refusals[i].limit ? "limit" : "semantic"),
							CStringGetTextDatum(refusals[i].text), CStringGetTextDatum(refusals[i].action)};
		bool		n[4] = {false, false, false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
	}
	return (Datum) 0;
}
