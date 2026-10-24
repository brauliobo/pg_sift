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
#include "access/xact.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

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
static bool stage_volatile;			/* the query of the stage calls a volatile function */
static int	stage_level;			/* query_level of the planner root of the stage: a sub-select is planned in its own */
static Tuplestorestate *why_store;
static TupleDesc why_desc;

static bool
is_siftagg(const Path *p)
{
	return IsA(p, CustomPath) && strncmp(((const CustomPath *) p)->methods->CustomName, "SiftAgg", 7) == 0;
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
			a->used |= list_member_ptr(a->rel->pathlist, p);
		a->used &= a->cheaper;
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

/* the plans of a statement at another size (cte.c) report nothing: the arms of the statement are those of the plan at its own */
bool
agg_quiet_reports(bool quiet)
{
	bool		was = quiet_reports;

	quiet_reports = quiet;
	return was;
}

static void
report(const ArmNote *a)
{
	const char *slug = refusals[a->why].slug;
	const char *reason = a->why == AGG_REFUSE_NONE ? NULL :
		a->detail ? psprintf("%s (%s)", refusals[a->why].text, a->detail) : refusals[a->why].text;

	if (quiet_reports)
		return;
	if (!a->used && a->why != AGG_REFUSE_NONE)
		sift_stats_refusal(slug, a->limit);
	if (why_store != NULL)
	{
		Datum		v[6];
		bool		n[6] = {false, false, a->used, a->used, a->paths == NIL && a->scan == 0, a->paths == NIL && a->scan == 0};

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

/* an error in the middle of a plan leaves the arm and its list in a context that is gone */
static void
forget_arms(void)
{
	arm = NULL;
	done = NIL;
	agg_vet_forget();
	agg_variant = NULL;
	quiet_reports = false;
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

void
agg_stage_end(AggStage *st)
{
	if (!st->on)
		return;
	foreach_ptr(ArmNote, a, done)
		if (pick_of(a->name) == a)
			report(a);
	arm = st->outer_arm;
	done = st->outer_done;
	stage_level = st->outer_level;
	stage_volatile = st->outer_volatile;
}

Datum
sift_agg_explain_why(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		saved = sift_explain_refusals;
	List	   *trees = pg_parse_query(sql),
			   *queries;
	RawStmt    *raw;

	if (list_length(trees) != 1 || !IsA((raw = linitial_node(RawStmt, trees))->stmt, SelectStmt))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift.explain_why takes one SELECT statement"),
						errhint("Pass the text of a single SELECT, without a second statement.")));
	InitMaterializedSRF(fcinfo, 0);
	queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
	PG_TRY();
	{
		why_store = rsi->setResult;
		why_desc = rsi->setDesc;
		sift_explain_refusals = true;
		foreach_ptr(PlannedStmt, ps, pg_plan_queries(queries, sql, CURSOR_OPT_PARALLEL_OK, NULL))
			if (ps->commandType == CMD_SELECT)
				ExecCheckPermissions(ps->rtable, ps->permInfos, true);
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
