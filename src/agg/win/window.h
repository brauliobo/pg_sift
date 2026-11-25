/*
 * agg/win/window.h - window functions (UPPERREL_WINDOW) and DISTINCT ON (UPPERREL_DISTINCT) of one table over the sorted parts of an index of format major 2, as a SiftAggV6Window node: when
 * the partition keys and then the order keys of the one window are a prefix of the sort key of the parts, agg/window.h computes the functions in the memory of one partition, with no sort and
 * no tuplestore of the input; when only the partition keys are, each partition is sorted by the order of the window in memory (any direction, NULLs first or last, int, float, numeric or C text keys); when no key is, the rows of a table of integers, dates, timestamps and float8 that fits work_mem are put together by partition first. A WHERE rn <= K over ROW_NUMBER, RANK or DENSE_RANK (the run condition PostgreSQL gives the window) stops a partition at its K-th row; DISTINCT ON is the same with
 * K = 1, or the last row of each partition when ORDER BY reverses the order of the parts. The rows leave in the order of the parts, one tuple for each row the engine emits.
 * What is not taken stays with PostgreSQL's WindowAgg: another sort order, a frame other than from the start of the partition to the current row, a function the engine does not have.
 *
 * The node is described by its plan (the window clause, the keys, the function nodes): sift_v6_window_spec makes the engine's spec from it, when the plan is made and again when the node starts.
 */
#ifndef SIFT_AGG_V6_WINDOW_H
#define SIFT_AGG_V6_WINDOW_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/window.h"
#include "agg/win/partsort.h"
#include "agg/vexpr.h"

struct SiftV6Winx;
struct SiftWinCarry;

typedef struct SiftV6WindowSpec
{
	Oid			indexoid;
	Index		rti;
	SiftWindowSpec win;
	int			part_slot[SIFT_WIN_MAX_KEYS];	/* the slots of the input of the keys and of the arguments of the functions (-1: none) */
	int			ord_slot[SIFT_WIN_MAX_KEYS];
	int			arg_slot[SIFT_WIN_MAX_FUNCS];
	Oid			arg_typid[SIFT_WIN_MAX_FUNCS];
	Oid			res_typid[SIFT_WIN_MAX_FUNCS];
	struct SiftV6Winx *winx;	/* a function has a FILTER or a numeric argument: SiftWinx runs the window (v6_winx_glue.h), win has only its keys and the number of functions */
	bool		partsort;		/* the parts are sorted by the PARTITION BY keys only: each partition is sorted by the window order as it is read (v6_win_partsort.h) */
	bool		regroup;		/* with partsort: the parts are not sorted by the keys either: the rows of each partition are put together first (v6_win_par.h) */
	bool		empty;			/* the run condition of this execution lets no row pass */
	SiftWinPsSpec ps;
} SiftV6WindowSpec;

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_window_plan_methods;

extern void sift_agg_v6_window_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern Plan *sift_agg_v6_window_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

/*
 * The spec of the plan's private list (the oid of the source, rti, mode, run condition function, range frame, how the partitions are made, the source is a sift_table, order flags; partition Vars; order Vars; function nodes; run condition bound), its columns added to in; false when the engine
 * does not run it. The planner and the executor make the spec by this one function. params: the values of this execution, NULL when the plan is made (a parameter bound
 * is taken as 1 then).
 */
extern bool sift_v6_window_spec(const List *priv, SiftV6Input *in, ParamListInfo params, SiftV6WindowSpec *out);

/* A spec that sorts partitions, once every column of the input is known (the surrogate columns of the sorted source follow them): the keys of the engine become the surrogates */
extern void sift_v6_window_partsort(SiftV6WindowSpec *spec, const SiftV6Input *in);

/* The result f of the engine as the Datum of its PostgreSQL type: a sum or a count that leaves int8 raises what PostgreSQL raises */
extern Datum sift_v6_window_datum(const SiftV6WindowSpec *spec, int f, const SiftWinVal *v);

typedef struct SiftV6WindowRun SiftV6WindowRun;

/* The engine over the source (which gives the rows that pass the WHERE clauses, sorted by the keys), in the memory context cxt: the rows come out by step, one batch of the input at a time, each to emit; false once the input is out */
extern SiftV6WindowRun *sift_v6_window_open(MemoryContext cxt, const SiftV6WindowSpec *spec, SiftV6Source *src, struct SiftWinCarry *carry, SiftWinEmit emit, void *emit_arg);
extern bool sift_v6_window_step(SiftV6WindowRun *r);
extern const SiftWinStats *sift_v6_window_stats(const SiftV6WindowRun *r);
extern void sift_v6_window_close(SiftV6WindowRun *r);

#endif							/* SIFT_AGG_V6_WINDOW_H */
