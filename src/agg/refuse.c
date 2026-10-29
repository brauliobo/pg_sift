/*
 * agg/refuse.c - sift.explain_refusals and sift.explain_why: why a query kept the normal plan.
 *
 * The planner of agg/plan*.c gives up silently, by design, for most shapes. With sift.explain_refusals on, the sites of the registry
 * (refuse.h) record the first reason each arm (a candidate path: single, distinct, two_level, rows, topn) gave up for; an arm that did add a
 * path is compared with the cheapest normal path of its stage. The reasons of a call of the planner hook are reported when it returns:
 * as a notice, or as a row of sift.explain_why, which plans a SELECT without running it. Nothing is recorded, and no plan changes, with the setting off.
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "access/table.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "commands/defrem.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "agg/agg.h"
#include "agg/refuse.h"
#include "agg/stats.h"
#include "core/limits.h"

PG_FUNCTION_INFO_V1(sift_agg_explain_why);
PG_FUNCTION_INFO_V1(sift_agg_refusal_slugs);

/* sift_explain_refusals: the sites note their reasons (the notice, sift.count_refusals or sift.explain_why asked for them); the notice is for the first */
bool		sift_explain_refusals = false;
static bool notice_refusals = false;
static bool count_refusals = false;
const char *agg_variant = NULL;

#define AGG_REFUSAL_ROW(id, slug, text, limit, by_design) {slug, text, limit, by_design},
static const struct
{
	const char *slug;
	const char *text;
	bool		limit;
	bool		by_design;
}			refusals[] = {{NULL, NULL, false, false}, SIFT_AGG_REFUSALS(AGG_REFUSAL_ROW)};

/* the ids of core/limits.h: the name sift.stats counts a refusal by a limit under */
static const char *const limit_names[] = {
#define LIMIT_NAME(id, macro, file, value, unit, what, behaviour, setting, hooked) #id,
	SIFT_LIMITS(LIMIT_NAME)
#undef LIMIT_NAME
};

typedef struct ArmNote
{
	const char *name;
	AggRefusal	why;
	const char *detail;
	const char *limit;			/* the id of the limit of core/limits.h the refusal is for */
	bool		candidate;
	bool		used;
	List	   *paths;
	RelOptInfo *rel;
	Path		best;			/* the cheapest normal path of the upper relation when the arm added its first */
	bool		has_best;
	bool		limited;		/* the top-N arm: a LIMIT above it is costed by the fraction of the rows it reads (agg_final_paths) */
	bool		cheaper;		/* a path of the arm is no dearer than best, as add_path compares them (enable_* off counts) */
	double		scan;
	double		normal;
	double		spill;			/* of scan, what the cheapest path pays for the groups that outgrow the hash memory */
	const char *spill_detail;
	double		vet;			/* of scan, what the cheapest path pays to check the heap for the pages that are not all-visible */
	double		vet_pct;
} ArmNote;

typedef struct PathVet
{
	double		cost;
	double		vet;
	double		pct;
} PathVet;

static ArmNote *arm;
static double path_spill;			/* of the path agg_cost is pricing: taken by agg_arm_path */
static const char *path_spill_detail;
static double path_vet;				/* of the path agg_cost is pricing: the heap check of the rows on pages that are not all-visible, and the share of those pages */
static double path_vet_pct;
static List *path_vets;			/* PathVet of each path the arms priced, for agg_vet_of: in vet_cx, for the plans of a statement are planned in contexts that are deleted before the choice between them is made */
static MemoryContext vet_cx;		/* kept for the backend, reset after each statement's plans (the leak report allows the names "sift cache ...") */
static List *done;
static MemoryContext final_cx;		/* the context final_wait lives in */
static List *final_wait;			/* (root, ArmNote) of the arms that were used before the LIMIT of the query was costed: reported when the final paths are known */
static bool stage_volatile;			/* the query of the stage calls a volatile function */
static int	stage_level;			/* query_level of the planner root of the stage: a sub-select is planned in its own */
static Tuplestorestate *why_store;
static TupleDesc why_desc;

static bool
is_siftagg(const Path *p)
{
	return IsA(p, CustomPath) && strncmp(((const CustomPath *) p)->methods->CustomName, "SiftAgg", 7) == 0;
}

/* is p in the list as it is, or under the (incremental) Sort a top-N puts over a scan that orders only the first of its keys? */
static bool
path_listed(const List *paths, const Path *p)
{
	foreach_ptr(Path, o, paths)
		if (o == p || ((IsA(o, SortPath) || IsA(o, IncrementalSortPath)) && ((SortPath *) o)->subpath == p))
			return true;
	return false;
}

bool
agg_refuse_note(AggRefusal why, const char *detail)
{
	if (arm != NULL && arm->why == AGG_REFUSE_NONE)
	{
		arm->why = why;
		arm->detail = detail ? pstrdup(detail) : NULL;
		arm->candidate = true;
	}
	return false;
}

bool
agg_refuse_limit_note(AggRefusal why, int lim, const char *detail)
{
	if (arm != NULL && arm->why == AGG_REFUSE_NONE)
		arm->limit = limit_names[lim];
	return agg_refuse_note(why, detail);
}

/* the arm tries another table as the scanned one: a first table that had no column index, or whose FROM tree the other order reads, is not the reason of the arm */
void
agg_refuse_retry(void)
{
	if (arm != NULL && (arm->why == AGG_REFUSE_NO_SOURCE || arm->why == AGG_REFUSE_FROM_SHAPE))
		arm->why = AGG_REFUSE_NONE;
}

/* [variant:][L<level>:]name: the arms of a sub-select and of a variant of the statement are told from the top level's */
static const char *
arm_name(const char *name)
{
	if (stage_level > 1)
		name = psprintf("L%d:%s", stage_level, name);
	return agg_variant ? psprintf("%s:%s", agg_variant, name) : name;
}

/* candidate: the arm applies to the query already (a refusal that follows is reported); else it does until a site names a reason */
void
agg_arm_begin(const char *name, bool candidate)
{
	if (!sift_explain_refusals)
		return;
	arm = palloc0(sizeof(ArmNote));
	arm->name = arm_name(name);
	arm->limited = strcmp(name, "topn") == 0;
	arm->candidate = candidate;
	arm->scan = arm->normal = get_float8_infinity();
}

void
agg_arm_candidate(void)
{
	if (arm != NULL)
		arm->candidate = true;
}

/* the spill agg_cost charged the path it prices (0: none), for the path agg_arm_path is given next */
void
agg_arm_spill(double cost, double groups, double table)
{
	if (arm == NULL)
		return;
	path_spill = cost;
	path_spill_detail = cost > 0 ?
		psprintf("about %.0f groups need %.0f kB, a hash aggregate gets %.0f kB, and the spill costs the scan %.2f", groups, table / 1024.0,
				 (double) get_hash_memory_limit() / 1024.0, cost) : NULL;
}

void
agg_arm_vet(double cost, double pct)
{
	if (arm == NULL)
		return;
	path_vet = cost;
	path_vet_pct = pct;
}

/* the heap check inside the cost of a SiftAgg plan node, and the share of the pages it is for (the node's total cost is its path's) */
double
agg_vet_of(double total_cost, double *pct)
{
	foreach_ptr(PathVet, v, path_vets)
		if (v->cost == total_cost)
		{
			*pct = v->pct;
			return v->vet;
		}
	return 0;
}

void
agg_vet_forget(void)
{
	path_vets = NIL;
	if (vet_cx != NULL)
		MemoryContextReset(vet_cx);
}

/* a path the arm is about to add to its upper relation: add_path may free it, so its cost is taken now */
void
agg_arm_path(Path *p)
{
	if (arm == NULL)
		return;
	if (arm->paths == NIL)
	{
		foreach_ptr(Path, o, p->parent->pathlist)
			if (!is_siftagg(o) && (!arm->has_best || compare_path_costs(o, &arm->best, TOTAL_COST) < 0))
			{
				arm->best = *o;
				arm->has_best = true;
			}
		if (arm->has_best)
			arm->normal = arm->best.total_cost;
	}
	if (path_vet > 0)
	{
		MemoryContext old;
		PathVet    *v;

		if (vet_cx == NULL)
			vet_cx = AllocSetContextCreate(TopMemoryContext, "sift cache path vets", ALLOCSET_SMALL_SIZES);
		old = MemoryContextSwitchTo(vet_cx);
		v = palloc(sizeof(PathVet));
		*v = (PathVet) {p->total_cost, path_vet, path_vet_pct};
		path_vets = lappend(path_vets, v);
		MemoryContextSwitchTo(old);
	}
	arm->paths = lappend(arm->paths, p);
	arm->rel = p->parent;
	if (p->total_cost < arm->scan)
	{
		arm->spill = path_spill;
		arm->spill_detail = path_spill_detail;
		arm->vet = path_vet;
		arm->vet_pct = path_vet_pct;
	}
	arm->scan = Min(arm->scan, p->total_cost);
	arm->cheaper |= !arm->has_best || compare_path_costs(p, &arm->best, TOTAL_COST) <= 0;
}

void
agg_arm_end(void)
{
	ArmNote    *a = arm;

	arm = NULL;
	if (a == NULL)
		return;
	if (a->paths != NIL)
	{
		foreach_ptr(Path, p, a->paths)
			a->used |= path_listed(a->rel->pathlist, p);
		a->used &= a->cheaper || sift_agg_force;
		if (!a->used)
		{
			/* the memory decided it when the scan, without the spill, is no dearer than the normal plan */
			bool		memory = a->spill > 0 && a->has_best && a->scan - a->spill <= a->normal;

			/* the visibility map decided it when the scan, without the heap check, is no dearer than the normal plan (a rewrite leaves the map empty) */
			bool		vm = !memory && a->vet > 0 && a->has_best && a->scan - a->vet <= a->normal;

			a->why = memory ? AGG_REFUSE_GROUP_MEMORY : AGG_REFUSE_COST;
			a->detail = psprintf("%s%sthe scan costs %.2f, the normal plan %.2f", memory ? a->spill_detail : "", memory ? ": " : "", a->scan, a->normal);
			if (vm)
				a->detail = psprintf("%s; %.0f%% of the heap pages are not all-visible, which adds %.2f to the scan: VACUUM (ANALYZE) the table", a->detail, a->vet_pct, a->vet);
		}
	}
	else if (!a->candidate)
		return;
	else if (a->why == AGG_REFUSE_NONE)
		a->why = AGG_REFUSE_OTHER;
	/* a volatile function is the reason of whatever shape site it was met at, unless the refusal is by design anyway */
	if (stage_volatile && a->why != AGG_REFUSE_NONE && !refusals[a->why].by_design)
		a->why = AGG_REFUSE_VOLATILE;
	done = lappend(done, a);
}

static bool quiet_reports = false;
static MemoryContext hold_cx;	/* set while the used arms of the inline variant are held (agg_variant_hold) */
static List *held;

/* the plans of a statement at another size (cte.c) report nothing: the arms of the statement are those of the plan at its own */
bool
agg_quiet_reports(bool quiet)
{
	bool		was = quiet_reports;

	quiet_reports = quiet;
	return was;
}

static void
emit(const ArmNote *a)
{
	const char *slug = refusals[a->why].slug;
	const char *reason = a->why == AGG_REFUSE_NONE ? NULL :
		a->detail ? psprintf("%s (%s)", refusals[a->why].text, a->detail) : refusals[a->why].text;

	if (why_store != NULL)
	{
		Datum		v[6];
		bool		n[6] = {false, false, a->used, a->used, a->paths == NIL && a->scan == 0, (a->paths == NIL && a->scan == 0) || a->normal < 0};

		v[0] = CStringGetTextDatum(a->name);
		v[1] = BoolGetDatum(a->used);
		v[2] = a->used ? (Datum) 0 : CStringGetTextDatum(slug);
		v[3] = a->used ? (Datum) 0 : CStringGetTextDatum(reason);
		v[4] = Float8GetDatum(a->scan);
		v[5] = Float8GetDatum(a->normal);
		tuplestore_putvalues(why_store, why_desc, v, n);
	}
	else if (!a->used && notice_refusals)
		ereport(NOTICE, (errmsg("SiftAgg not used: %s: %s", slug, reason), errdetail("arm: %s", a->name)));
}

static void
report(const ArmNote *a)
{
	if (quiet_reports)
		return;
	if (!a->used && a->why != AGG_REFUSE_NONE)
		sift_stats_refusal(refusals[a->why].slug, a->limit);
	if (hold_cx != NULL && a->used && agg_variant != NULL && why_store != NULL)
	{
		ArmNote    *h = MemoryContextAlloc(hold_cx, sizeof(ArmNote));

		*h = *a;
		h->name = MemoryContextStrdup(hold_cx, a->name);
		h->paths = NIL;
		held = lappend(held, h);
		return;
	}
	emit(a);
}

/*
 * The arms of the plan with the CTE inlined that it used wait for the choice between that plan and the shared one (agg_variant_settle): where the shared
 * plan is chosen they are not in the plan of the statement, whatever the plan of their own variant said.
 */
void
agg_variant_hold(void)
{
	if (sift_explain_refusals)
		hold_cx = CurrentMemoryContext;
}

void
agg_variant_settle(bool kept, const char *detail)
{
	List	   *arms = held;

	held = NIL;
	hold_cx = NULL;
	foreach_ptr(ArmNote, a, arms)
	{
		if (!kept)
		{
			a->used = false;
			a->why = AGG_REFUSE_CTE_COST;
			a->detail = detail;
		}
		emit(a);
	}
}

/* a decision outside the arms of the planner hook (the plan of the statement as a whole) */
void
agg_report(const char *name, AggRefusal why, const char *detail, bool used, double scan, double normal)
{
	ArmNote		a = {.why = why, .detail = detail, .used = used, .paths = NIL, .scan = scan, .normal = normal};

	if (!sift_explain_refusals)
		return;
	a.name = arm_name(name);
	report(&a);
}

/* plans the statement in a variant of itself (inline, flat): its arms are named after it */
const char *
agg_variant_push(const char *v)
{
	const char *outer = agg_variant;

	if (sift_explain_refusals)
		agg_variant = outer ? psprintf("%s+%s", outer, v) : v;
	return outer;
}

/* the context of the list of waiting arms went away (a plan that never costed its final paths, like the LIMIT 1 sub-select a min() lost with, leaves the list behind) */
static void
final_wait_gone(void *arg)
{
	final_wait = NIL;
	final_cx = NULL;
}

/* an error in the middle of a plan leaves the arm and its list in a context that is gone */
static void
forget_arms(void)
{
	arm = NULL;
	done = NIL;
	final_wait = NIL;
	agg_vet_forget();
	agg_variant = NULL;
	quiet_reports = false;
	held = NIL;
	hold_cx = NULL;
	stage_level = 0;
	stage_volatile = false;
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

void
agg_stage_begin(AggStage *st, PlannerInfo *root)
{
	static bool registered;
	int			level = root->query_level;

	st->on = sift_explain_refusals;
	st->root = root;
	if (!st->on)
		return;
	if (!registered)
	{
		RegisterXactCallback(forget_arms_xact, NULL);
		RegisterSubXactCallback(forget_arms_subxact, NULL);
		registered = true;
	}
	st->outer_arm = arm;
	st->outer_done = done;
	st->outer_level = stage_level;
	st->outer_volatile = stage_volatile;
	arm = NULL;
	done = NIL;
	stage_level = level;
	stage_volatile = contain_volatile_functions((Node *) root->parse);
}

/* the pick of a column index that reports for the arm: the one that used it, else the first */
static const ArmNote *
pick_of(const char *name)
{
	const ArmNote *first = NULL;

	foreach_ptr(ArmNote, b, done)
		if (strcmp(b->name, name) == 0)
		{
			if (b->used)
				return b;
			if (first == NULL)
				first = b;
		}
	return first;
}

/* does the path, or what it only limits, projects or sorts, run a SiftAgg scan? */
static bool
final_runs_siftagg(const Path *p)
{
	for (;; )
	{
		if (is_siftagg(p))
			return true;
		switch (nodeTag(p))
		{
			case T_LimitPath:
				p = ((const LimitPath *) p)->subpath;
				break;
			case T_ProjectionPath:
				p = ((const ProjectionPath *) p)->subpath;
				break;
			case T_SortPath:
			case T_IncrementalSortPath:
				p = ((const SortPath *) p)->subpath;
				break;
			default:
				return false;
		}
	}
}

/*
 * The arm for ORDER BY .. LIMIT compares its path with the whole cost of the ordered paths; the LIMIT is costed after it, and an index that
 * delivers the order reads only the rows asked for. The final paths say which won, and at what cost.
 */
void
agg_final_paths(PlannerInfo *root, RelOptInfo *final_rel)
{
	List	   *rest = NIL;

	foreach_ptr(List, w, final_wait)
	{
		ArmNote    *a = lsecond(w);

		if (linitial(w) != root)
		{
			rest = lappend(rest, w);
			continue;
		}
		a->used = false;
		foreach_ptr(Path, p, final_rel->pathlist)
			a->used |= final_runs_siftagg(p);
		if (!a->used)
		{
			a->normal = get_float8_infinity();
			foreach_ptr(Path, p, final_rel->pathlist)
				a->normal = Min(a->normal, p->total_cost);
			a->why = AGG_REFUSE_COST;
			a->detail = psprintf("the scan costs %.2f, the normal plan with the LIMIT %.2f", a->scan, a->normal);
		}
		report(a);
	}
	final_wait = rest;
}

void
agg_stage_end(AggStage *st)
{
	if (!st->on)
		return;
	foreach_ptr(ArmNote, a, done)
		if (pick_of(a->name) != a)
			continue;
		else if (a->used && a->limited && st->root->parse->limitCount != NULL)
		{
			if (final_cx != CurrentMemoryContext)
			{
				MemoryContextCallback *gone = palloc0(sizeof(MemoryContextCallback));

				gone->func = final_wait_gone;
				final_cx = CurrentMemoryContext;
				MemoryContextRegisterResetCallback(final_cx, gone);
			}
			final_wait = lappend(final_wait, list_make2(st->root, a));
		}
		else
			report(a);
	arm = st->outer_arm;
	done = st->outer_done;
	stage_level = st->outer_level;
	stage_volatile = st->outer_volatile;
}

/* does the plan read a sift_col index (an index, index-only or bitmap scan of the access method) anywhere, in a child, a subquery or an initplan? */
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

/* does a table of the statement have a sift_col index? */
static bool
tables_have_sift_index(const PlannedStmt *ps, Oid amoid)
{
	foreach_node(RangeTblEntry, rte, ps->rtable)
	{
		Relation	rel;
		bool		found = false;

		if (rte->rtekind != RTE_RELATION)
			continue;
		rel = table_open(rte->relid, AccessShareLock);
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
 * none means the table has no such index (no_source) or no condition of the statement is one the index answers (where).
 */
static void
why_index_arm(RawStmt *raw, const char *sql, const PlannedStmt *ps)
{
	Oid			amoid = get_am_oid("sift_col", true);
	int			nest;
	PlannedStmt *again = NULL;

	if (!OidIsValid(amoid) || ps->commandType != CMD_SELECT)
		return;
	if (plan_stmt_reads_sift_index(ps, amoid))
	{
		agg_report("index", AGG_REFUSE_NONE, NULL, true, ps->planTree->total_cost, -1);
		return;
	}
	if (!tables_have_sift_index(ps, amoid))
	{
		agg_report("index", AGG_REFUSE_NO_SOURCE, NULL, false, 0, -1);
		return;
	}
	nest = NewGUCNestLevel();
	PG_TRY();
	{
		(void) set_config_option("enable_seqscan", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
		foreach_ptr(PlannedStmt, p2, pg_plan_queries(pg_analyze_and_rewrite_fixedparams(copyObject(raw), sql, NULL, 0, NULL), sql, CURSOR_OPT_PARALLEL_OK, NULL))
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

	if (list_length(trees) != 1 || !IsA((raw = linitial_node(RawStmt, trees))->stmt, SelectStmt))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift.explain_why takes one SELECT statement"),
						errhint("Pass the text of a single SELECT, without a second statement.")));
	InitMaterializedSRF(fcinfo, 0);
	again_raw = copyObject(raw);	/* analysis rewrites the sub-selects of the tree in place */
	queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
	PG_TRY();
	{
		why_store = rsi->setResult;
		why_desc = rsi->setDesc;
		sift_explain_refusals = true;
		foreach_ptr(PlannedStmt, ps, pg_plan_queries(queries, sql, CURSOR_OPT_PARALLEL_OK, NULL))
			if (ps->commandType == CMD_SELECT)
			{
				ExecCheckPermissions(ps->rtable, ps->permInfos, true);
				if (tuplestore_tuple_count(why_store) == 0)
					why_index_arm(again_raw, sql, ps);
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
	sift_explain_refusals = newval || count_refusals;
}

static void
assign_count(bool newval, void *extra)
{
	sift_explain_refusals = newval || notice_refusals;
}

void
sift_agg_refuse_init(void)
{
	DefineCustomBoolVariable("sift.explain_refusals",
							 "Reports as a notice why a query kept the normal plan instead of Custom Scan (SiftAgg): the first reason each candidate path gave up for.",
							 NULL, &notice_refusals, false,
							 PGC_USERSET, 0, NULL, assign_notice, NULL);
	DefineCustomBoolVariable("sift.count_refusals",
							 "Counts in sift.stats() why queries kept the normal plan (refused.<slug>, limit.<id>), without the notice of sift.explain_refusals.",
							 NULL, &count_refusals, false,
							 PGC_USERSET, 0, NULL, assign_count, NULL);
}

/* sift.refusal_slugs(): the registry of refuse.h, for the tools that read sift.stats() or sift.explain_why() */
Datum
sift_agg_refusal_slugs(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 1; i < AGG_REFUSE_COUNT; i++)
	{
		Datum		v[3] = {CStringGetTextDatum(refusals[i].slug), CStringGetTextDatum(refusals[i].limit ? "limit" : "semantic"),
							CStringGetTextDatum(refusals[i].text)};
		bool		n[3] = {false, false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
	}
	return (Datum) 0;
}
