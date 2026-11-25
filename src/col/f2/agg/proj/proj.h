/*
 * col/f2/agg/proj/proj.h - the choice of the source of a SiftAggV6 query among the base parts and the projections of the relation (col/f2/proj/proj.h): which projections can answer the query exactly,
 * which of the base parts each of them stands for under the snapshot, and which source costs least. No part is read here and no GUC is read: the five costs are those of the planner.
 *
 * A projection can answer an aggregate query (SIFT_PROJ_AGG) when every group column of the query is a group column of the projection (or a coarser date_trunc of it), every predicate is on a
 * group column, and every aggregate is a measure the projection keeps or derives from one; it can serve a row query (SIFT_PROJ_SORT) when it keeps every column the query reads and its sort
 * order prunes the query's predicates or gives its ORDER BY. A base part the projection cannot stand for (f2_proj_fresh.c) is read from the base, so the answer is always exact; the choice is
 * only about cost.
 */
#ifndef SIFT_COL_F2_V6_PROJ_H
#define SIFT_COL_F2_V6_PROJ_H

#include "core/host.h"

#include "col/f2/agg/proj/pred.h"
#include "col/f2/proj/proj.h"

#define SV6P_MAX	SIFT_PROJ_MAX_COLS

typedef enum SiftV6PredOp
{
	SV6P_EQ = 0,
	SV6P_RANGE,					/* <, <=, >, >=, BETWEEN */
	SV6P_IN,
	SV6P_NE,					/* <>: the cells are tested for it, but a sort order cannot prune by it */
	SV6P_OTHER					/* anything the cells of a projection cannot be tested for */
} SiftV6PredOp;

typedef struct SiftV6ProjPred
{
	int16		leaf;
	uint8		fn;				/* SiftProjFn of the expression the predicate is on */
	uint8		unit;
	uint8		op;				/* SiftV6PredOp */
	int			nvals;			/* IN: the values; else 1 */
	double		sel;			/* the selectivity the planner estimates */
	const SiftProjPred *cell;	/* the same predicate for the cells (col/f2/agg/proj/pred.h): what a WHERE of a projection and the edges of a date_trunc bucket are judged by; NULL: unknown */
} SiftV6ProjPred;

typedef struct SiftV6ProjQuery
{
	int			ngroup;
	SiftProjKey group[SV6P_MAX];
	int			nagg;
	SiftProjMeasure agg[SV6P_MAX];
	int			npred;
	SiftV6ProjPred pred[SV6P_MAX];
	int			norder;
	SiftProjKey order[SV6P_MAX];
	int64		limit;			/* -1: none */
	int			nread;			/* the columns of the relation the query reads (a row query) */
	int16		read[SIFT_PROJ_MAX_COLS * 2];
	bool		wants_rows;		/* a row query (SIFT_PROJ_SORT can serve it) and not an aggregate */
} SiftV6ProjQuery;

/* The five costs of the planner (seq_page_cost, random_page_cost, cpu_tuple_cost, cpu_operator_cost, cpu_index_tuple_cost) */
typedef struct SiftV6ProjCost
{
	double		seq_page;
	double		random_page;
	double		cpu_tuple;
	double		cpu_operator;
	double		cpu_index_tuple;
} SiftV6ProjCost;

/* One projection of the relation as the choice sees it, and what it can stand for under the snapshot (serve[i] for the base part i) */
typedef struct SiftV6ProjCand
{
	const SiftManifestProjRef *ref;
	const SiftProjSpec *spec;
	const SiftProjParts *parts;
	const SiftProjPartServe *serve;
} SiftV6ProjCand;

typedef struct SiftV6ProjInput
{
	const SiftV6ProjQuery *q;
	SiftV6ProjCost cost;
	const SiftProjSpec *base_order;	/* the key_order of the relation as a SORT spec; NULL: none */
	const SiftPartDesc *base;
	int			nbase;
	int			ncols;			/* the columns of the relation */
	int			ncand;
	const SiftV6ProjCand *cand;
} SiftV6ProjInput;

typedef struct SiftV6ProjChoice
{
	int			cand;			/* the index of the candidate, -1: the base parts */
	double		cost;
	double		base_cost;
	int			served;			/* base parts the chosen projection stands for */
	int			fallback[SPW_WHERE_EXC + 1];	/* base parts read from the base, by SiftProjWhy */
	SiftProjRollup rollup;		/* an AGG projection: how its cells become the groups of the query */
	bool		covers[SIFT_PROJ_MAX];	/* of each candidate (up to SIFT_PROJ_MAX): it can answer the query at all */
} SiftV6ProjChoice;

/* Whether the projection can answer the aggregate query, and how its cells roll up into the query's groups */
extern bool sift_v6_proj_match_agg(const SiftProjSpec *p, const SiftProjColumns *cols, const SiftV6ProjQuery *q, SiftProjRollup *map);

/* What a sort order does for the query: the leading columns its predicates use (an equality on each, then at most one range), and whether its order gives the ORDER BY */
typedef struct SiftV6SortUse
{
	int			prefix;
	double		prefix_sel;
	bool		order;
	bool		in_merge;		/* the prefix holds one column to a list of values: one sorted stream for each value, merged in the order of the statement */
	int			in_key_idx;		/* that column's place in the sort order */
} SiftV6SortUse;

#define SIFT_PROJ_IN_SEEK_MAX	64

/* Every predicate of the spec's WHERE is implied by a predicate of the query: the projection holds every row the query needs */
extern bool sift_v6_proj_where_covers(const SiftProjSpec *p, const SiftV6ProjQuery *q);

extern bool sift_v6_proj_match_sort(const SiftProjSpec *p, const SiftProjColumns *cols, const SiftV6ProjQuery *q, SiftV6SortUse *use);
extern SiftV6SortUse sift_v6_sort_use(const SiftProjSpec *order, const SiftV6ProjQuery *q);

/* The sort projection of the n given that serves the row query best by what its order does for it: the one that gives the ORDER BY with the longest prefix of predicates, then the longest prefix alone; -1: none serves it. A query with an ORDER BY is served only by an order that gives it. */
extern int	sift_v6_proj_pick_sort(const SiftProjSpec *const *spec, int n, const SiftProjColumns *cols, const SiftV6ProjQuery *q);

extern SiftV6ProjChoice sift_v6_proj_choose(const SiftV6ProjInput *in, const SiftProjColumns *cols);

#endif							/* SIFT_COL_F2_V6_PROJ_H */
