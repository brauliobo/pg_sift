/*
 * core/dispatch.c - the one entry of the library at each hook point a plain statement passes, and its one callback of the ends of transactions and subtransactions.
 *
 * The hooks of the subsystems chain as PostgreSQL chains them, behind one entry a point; the callbacks of the transactions are a list of this file. In a database the catalog cache says
 * has no access method of the library there is no relation of it, so nothing to plan, run or end: the entry goes straight to what was installed before the library and the ends of the
 * transactions call nothing. A plain statement then runs a few instructions of the library on one page of its code, not the ~60 functions on ~50 pages of the subsystems, which evicted
 * the server's own code from the instruction caches and TLB of the CPU (core/oltp_hook_zero_tax: +0.8% instructions, +13% cycles).
 */
#include "postgres.h"

#include "executor/executor.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"

#include "core/catalog.h"
#include "core/compat.h"
#include "core/dispatch.h"

typedef struct Hooks
{
	planner_hook_type planner;
#if PG_VERSION_NUM < 190000
	get_relation_info_hook_type rel_info;
#else
	build_simple_rel_hook_type simple_rel;
#endif
	set_rel_pathlist_hook_type rel_paths;
	set_join_pathlist_hook_type join_paths;
	create_upper_paths_hook_type upper_paths;
	ExecutorStart_hook_type start;
	ExecutorRun_hook_type run;
	ExecutorFinish_hook_type finish;
} Hooks;

/* what was installed before the library, and the chains of its subsystems */
static Hooks before,
			sift;

typedef struct Listener
{
	XactCallback xact;
	SubXactCallback sub;
	void	   *arg;
} Listener;

/* every site of the library that listens does so once a backend: 45 of them */
#define SIFT_XACT_LISTENERS	64
static Listener listeners[SIFT_XACT_LISTENERS];
static int	nlisteners;

static PlannedStmt *
entry_planner(SIFT_PLANNER_PARAMS)
{
	if (sift_db_has_extension())
		return sift.planner(SIFT_PLANNER_ARGS(parse));
	return before.planner ? before.planner(SIFT_PLANNER_ARGS(parse)) : standard_planner(SIFT_PLANNER_ARGS(parse));
}

#if PG_VERSION_NUM < 190000
static void
entry_rel_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (sift_db_has_extension())
		sift.rel_info(root, relid, inhparent, rel);
	else if (before.rel_info)
		before.rel_info(root, relid, inhparent, rel);
}
#else
static void
entry_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (sift_db_has_extension())
		sift.simple_rel(root, rel, rte);
	else if (before.simple_rel)
		before.simple_rel(root, rel, rte);
}
#endif

static void
entry_rel_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (sift_db_has_extension())
		sift.rel_paths(root, rel, rti, rte);
	else if (before.rel_paths)
		before.rel_paths(root, rel, rti, rte);
}

static void
entry_join_paths(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	if (sift_db_has_extension())
		sift.join_paths(root, joinrel, outerrel, innerrel, jointype, extra);
	else if (before.join_paths)
		before.join_paths(root, joinrel, outerrel, innerrel, jointype, extra);
}

static void
entry_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input, RelOptInfo *output, void *extra)
{
	if (sift_db_has_extension())
		sift.upper_paths(root, stage, input, output, extra);
	else if (before.upper_paths)
		before.upper_paths(root, stage, input, output, extra);
}

static void
entry_start(QueryDesc *qd, int eflags)
{
	if (sift_db_has_extension())
		sift.start(qd, eflags);
	else if (before.start)
		before.start(qd, eflags);
	else
		standard_ExecutorStart(qd, eflags);
}

static void
entry_run(SIFT_RUN_PARAMS)
{
	if (sift_db_has_extension())
		sift.run(SIFT_RUN_ARGS);
	else if (before.run)
		before.run(SIFT_RUN_ARGS);
	else
		standard_ExecutorRun(SIFT_RUN_ARGS);
}

static void
entry_finish(QueryDesc *qd)
{
	if (sift_db_has_extension())
		sift.finish(qd);
	else if (before.finish)
		before.finish(qd);
	else
		standard_ExecutorFinish(qd);
}

/* The callbacks in the order PostgreSQL calls its own: the last registered first. A transaction can end in a failed state, so the catalog is not read: only its cache is asked */
static void
xact_entry(XactEvent event, void *arg)
{
	if (sift_catalog_absent())
		return;
	for (int i = nlisteners - 1; i >= 0; i--)
		if (listeners[i].xact)
			listeners[i].xact(event, listeners[i].arg);
}

static void
subxact_entry(SubXactEvent event, SubTransactionId sub, SubTransactionId parent, void *arg)
{
	if (sift_catalog_absent())
		return;
	for (int i = nlisteners - 1; i >= 0; i--)
		if (listeners[i].sub)
			listeners[i].sub(event, sub, parent, listeners[i].arg);
}

static void
listen(Listener l)
{
	SIFT_INVARIANT(nlisteners < SIFT_XACT_LISTENERS, "more callbacks of the ends of transactions than the %d sites of the library", SIFT_XACT_LISTENERS);
	listeners[nlisteners++] = l;
}

void
sift_xact_callback(XactCallback fn, void *arg)
{
	listen((Listener) {.xact = fn,.arg = arg});
}

void
sift_subxact_callback(SubXactCallback fn, void *arg)
{
	listen((Listener) {.sub = fn,.arg = arg});
}

void
sift_dispatch_begin(void)
{
	before = (Hooks) {
		.planner = planner_hook,
#if PG_VERSION_NUM < 190000
		.rel_info = get_relation_info_hook,
#else
		.simple_rel = build_simple_rel_hook,
#endif
		.rel_paths = set_rel_pathlist_hook,
		.join_paths = set_join_pathlist_hook,
		.upper_paths = create_upper_paths_hook,
		.start = ExecutorStart_hook,
		.run = ExecutorRun_hook,
		.finish = ExecutorFinish_hook,
	};
	RegisterXactCallback(xact_entry, NULL);
	RegisterSubXactCallback(subxact_entry, NULL);
}

/* A point whose hook a subsystem took gets the entry; the chain of the subsystems stays where the entry calls it */
#define TAKE(hook, field, entry) \
	do { sift.field = (hook); if ((hook) != before.field) (hook) = (entry); } while (0)

void
sift_dispatch_end(void)
{
	TAKE(planner_hook, planner, entry_planner);
#if PG_VERSION_NUM < 190000
	TAKE(get_relation_info_hook, rel_info, entry_rel_info);
#else
	TAKE(build_simple_rel_hook, simple_rel, entry_simple_rel);
#endif
	TAKE(set_rel_pathlist_hook, rel_paths, entry_rel_paths);
	TAKE(set_join_pathlist_hook, join_paths, entry_join_paths);
	TAKE(create_upper_paths_hook, upper_paths, entry_upper_paths);
	TAKE(ExecutorStart_hook, start, entry_start);
	TAKE(ExecutorRun_hook, run, entry_run);
	TAKE(ExecutorFinish_hook, finish, entry_finish);
}
