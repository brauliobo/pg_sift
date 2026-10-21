/*
 * agg/rows.h - Custom Scan (SiftAggRows): the grouping of the rows of a plan, with exact count(DISTINCT) (agg/rows.c, agg/plan_rows.c)
 */
#ifndef SIFT_AGG_ROWS_H
#define SIFT_AGG_ROWS_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT bool sift_agg_rows;

/* what the node computes for each aggregate of the query; the arguments and the FILTERs are columns of the child plan */
typedef enum SiftRowsKind
{
	SRA_COUNT_STAR,
	SRA_COUNT,
	SRA_DISTINCT,				/* count(DISTINCT x) of an integer column: a set of (group, value) pairs */
	SRA_PG						/* sum and avg: the transition and final functions of PostgreSQL's aggregate */
} SiftRowsKind;

typedef struct SiftRowsAgg
{
	SiftRowsKind kind;
	int			argcol;			/* the column of the child plan that holds the argument, -1 for count(*) */
	int			filtercol;		/* ... and the FILTER (a boolean), -1 for none */
	Oid			fn;
	Oid			restype;
	Oid			argtype;
	Oid			collid;
} SiftRowsAgg;

/* the child plan returns the group columns, then each aggregate's argument and FILTER */
typedef struct SiftRowsSpec
{
	int			nkeys;
	Oid		   *eqop;
	Oid		   *collid;
	int			naggs;
	SiftRowsAgg *agg;
} SiftRowsSpec;

/* at most this many count(DISTINCT) of one argument share a set: a pair carries a bit for each */
#define SIFT_ROWS_MAX_SHARED	32

extern List *sift_agg_rows_pack(const SiftRowsSpec *s);
extern SiftRowsSpec *sift_agg_rows_unpack(List *pack);

/* agg/plan_rows.c: the path, from the grouping stage of the upper path hook */
extern void sift_agg_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern PGDLLIMPORT const CustomScanMethods sift_agg_rows_plan_methods;

/* agg/rows.c */
extern void sift_agg_rows_init(void);
extern Node *sift_agg_rows_create_state(CustomScan *cscan);

#endif							/* SIFT_AGG_ROWS_H */
