/*
 * agg/cte/int.h - what the files of agg/cte/ share
 * The walk over the shared CTEs of a statement and the functions the parts of the hook
 * call each other by.
 */
#ifndef SIFT_AGG_CTE_INT_H
#define SIFT_AGG_CTE_INT_H

#include "agg/plan_int.h"

/*
 * What the walk below finds: with inline_them the shared CTEs (the first 32 of the statement in the order of the walk, or those of only) are written NOT
 * MATERIALIZED, and their names are kept in *names. pos counts the shared CTEs the walk met. With survey the walk changes nothing and sets in reading the bits
 * of those that read a table of reads.
 */
typedef struct SharedCteWalk
{
	bool		inline_them,
				survey;
	List	   *names;
	int			hoisted;
	int			pos;
	uint32		only,
				reading;
	const List *reads;
} SharedCteWalk;

#define SHARED_CTE_BIT(pos)	((pos) < 32 ? (uint32) 1 << (pos) : 0)
#define SHARED_CTES(n)		((n) < 32 ? ((uint32) 1 << (n)) - 1 : ~(uint32) 0)

extern bool shared_cte_walker(Node *n, SharedCteWalk *w);
extern bool has_shared_cte(Query *parse);

typedef struct ScanArms
{
	double		total;
	double		first;			/* the dearest arm */
	double		vet;			/* of total, what the arms pay to check the heap for the pages that are not all-visible */
	double		vet_pct;		/* and the largest share of the pages that are not */
	int			n;
} ScanArms;
extern void sift_agg_arms(Plan *plan, ScanArms *a);
extern void sift_agg_arms_list(List *plans, ScanArms *a);
extern bool uses_sift_agg(Plan *plan);
extern bool any_sift_agg(List *plans);
extern void arm_tables(Plan *plan, List **oids);
extern double cte_size(PlannedStmt *stmt);
extern double plan_cost_without_unread_ctes(PlannedStmt *stmt);
extern double shared_work_cost(PlannedStmt *stmt, List *names, double read_rows, double lift, int nscans, double *charged, int *unnesting);
extern bool plain_reader_stmt(PlannedStmt *stmt, double share);

/* the plans of a statement with its shared CTEs and with them inlined, and what the scans of the inline plan and the work of the shared one cost */
typedef struct CteVariants
{
	PlannedStmt *plain,
			   *inlined;
	MemoryContext plain_cx,
				inlined_cx;
	Query	   *stock_parse;	/* a copy of the statement for the plan that is the fallback, when the shared plan was made with lifted rows (plan_stock) */
	bool		lifted;			/* the shared plan was made with rows that derived_rel_rows, derived_cte_rows or key_list_lift raised */
	ScanArms	scans;
	double		plain_cost,
				charged,		/* of plain_cost, what the unnesting aggregates are charged for the rows their estimate lost */
				work,
				read_rows;		/* the most rows the scans of the inline plan read (agg_read_rows) */
} CteVariants;
extern PlannedStmt * plan_in_own_context(SIFT_PLANNER_PARAMS, MemoryContext *cx);
extern void discard_plan(MemoryContext cx);
extern void delete_discarded(void);
extern void forget_discarded(void);
extern PlannedStmt * plan_shared(SIFT_PLANNER_PARAMS, MemoryContext *cx, double *lift, bool stock, bool *lifted);
extern void plan_variants(SIFT_PLANNER_PARAMS, Query *inline_parse, List *names, CteVariants *v);
extern bool inline_wins(const CteVariants *v, double vet, double shared_vet);
extern bool inline_wins_at_scale(SIFT_PLANNER_PARAMS, Query *pristine_inline, List *names);
extern double planner_total(SIFT_PLANNER_PARAMS);
extern PlannedStmt * plan_cached(SIFT_PLANNER_PARAMS, const CteVerdict *vd, MemoryContext *cx);

/*
 * The inline plan is kept when the scans cost this many times less than what the shared plan spends on the work they replace (the plans of the
 * CTEs and the aggregates that read them). Both are prices of the same rows, so the plan above them, which PostgreSQL estimates from the rows
 * each plan expects (the shared CTE one row, the scans a group count, and a join on an expression key the default selectivity), is not part of
 * the comparison. The factor covers the scans' unnest charge (agg_cost prices the plain plan with the 100 rows per call that PostgreSQL assumes
 * for jsonb_to_recordset, the data has a handful): 56 statements of a 195,000-row slice priced 1.15 to 1.6 times cheaper on the scans ran 63 s shared
 * and 23 s inline, none slower (the one that measured 1.3 times slower under load tied on a repeat). The statements priced less than 1.15 times
 * cheaper are not measured.
 */
#define CTE_INLINE_MARGIN	1.15

/* A plan above the scans priced this many times the shared plan has an estimate that cannot be trusted (a nested loop over a join of rows it takes for one). */
#define CTE_INLINE_ABOVE	20.0

/* ... and where an ordinary plan still reads the table, the inline plan may total this many times the shared one at most (priced 1.06 to 1.22 times above it, run 1.2 to 1.5 times faster) */
#define CTE_INLINE_TOTALS	1.25

/* the planner hook this one was installed in front of, and whether the plan the hook returned is the inline one */
extern planner_hook_type prev_planner_hook;
extern bool inline_won;

#endif							/* SIFT_AGG_CTE_INT_H */
