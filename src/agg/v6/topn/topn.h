/*
 * agg/v6/topn/topn.h - ORDER BY k0 [, k1 ..] LIMIT n OFFSET m of one table over the sorted parts of an index of format major 2, as a SiftAggV6TopN node (UPPERREL_ORDERED): the tournament of
 * agg/topn_v6.h reads the units whose zones can hold a better row first and stops when none can. The node gives the best offset + n rows in order (with WITH TIES, every row that ties
 * the last) and keeps the pathkeys of the sort, so that the Limit above it takes the offset and the rows; it reads nothing the query's WHERE clauses do not pass (compiled by
 * v6_vexpr.h) and is not chosen under row security or, of an index, for a key the order of whose int64 is not the order of the type (a sift_table sorts the rows it fetches with the comparator and the collation of the query, so it takes uuid, bytea, char(n), name and numeric keys too). The limit and the offset
 * may be parameters: a run whose offset + limit is no more than SIFT_TOURN_MAX_K (and that has no WITH TIES) takes the tournament, any other (a constant past it is priced as that sort) sorts the rows the source gives in a tuplesort bounded by it.
 */
#ifndef SIFT_AGG_V6_TOPN_H
#define SIFT_AGG_V6_TOPN_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/topn_v6.h"
#include "agg/v6/vexpr.h"

typedef struct SiftV6TopNSpec
{
	Oid			indexoid;		/* the index, or the sift_table when is_table */
	bool		is_table;
	Index		rti;			/* of the table in the query the plan was made for: the Vars of its clauses */
	int			nkeys;
	SiftV6Col	key[SIFT_TOURN_MAX_KEYS];
	SiftTournKey order[SIFT_TOURN_MAX_KEYS];
	Oid			sortop[SIFT_TOURN_MAX_KEYS];
	Oid			collation[SIFT_TOURN_MAX_KEYS];
	Node	   *limit;			/* of the query: an int8 Const or external Param; a NULL value is LIMIT ALL */
	Node	   *offset;			/* NULL: none */
	uint64		rows;			/* offset + limit, when a run found them small enough for the tournament */
	bool		with_ties;
	char	   *desc;			/* EXPLAIN: the sort keys */
} SiftV6TopNSpec;

/* What a source passed over without reading: partitions and parts whose keys or times leave no row, granules whose zones do */
typedef struct SiftV6SkipStats
{
	uint64		partitions;
	uint64		parts;
	uint64		granules;
} SiftV6SkipStats;

/* What the device took of the query: its best rows (SiftGpuCand of agg/gpu/v6.h, in order) and the generations of the parts it read, ascending, which the source leaves out */
typedef struct SiftV6TopNDevice
{
	const struct SiftGpuCand *cand;
	uint32		ncand;
	const uint64 *skip;
	int			nskip;
} SiftV6TopNDevice;

/* The units of the index as the tournament reads them, and the rows of the winners: what the format gives the node */
typedef struct SiftV6TopNSource
{
	SiftTournOps ops;
	void	   *src;
	/* the rows of the device go to the tournament as the source's own and the parts it read are not scanned; NULL: the source takes nothing of the device */
	void		(*device) (void *src, const SiftV6TopNDevice *dev);
	/* queues the partitions and the rows not yet in a part (sift_tourn_push) */
	void		(*seed) (void *src, SiftTournament *t);
	/* the columns attnos[0 .. n-1] of the table for a winner, as the Datums of their types (the heap row of its tid, or the leaves of the index) */
	bool		(*fetch) (void *src, const SiftTopNWinRow *w, const AttrNumber *attnos, int n, Datum *values, bool *isnull);
	void		(*close) (void *src);
	const SiftV6SkipStats *skips;
} SiftV6TopNSource;

/* SiftOrderOps.topn_open: in is the columns the quals read; the source offers only the rows that pass quals (NULL: all) */

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_topn_plan_methods;

extern void sift_agg_v6_topn_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern Plan *sift_agg_v6_topn_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

/* EXPLAIN of a Top-N: the sort keys of the statement ("total DESC, id") and the rows the scan holds for its LIMIT and OFFSET */
extern char *sift_v6_topn_desc(PlannerInfo *root, Index rti);
extern char *sift_v6_topn_limit_text(const Node *limit, const Node *offset);
extern void sift_v6_topn_explain(ExplainState *es, const char *desc, const char *limit);

extern List *sift_v6_topn_spec_pack(const SiftV6TopNSpec *s);
extern SiftV6TopNSpec *sift_v6_topn_spec_unpack(const List *l);

/* The tournament over the source: the winners in order (valid until sift_tourn_free of *tp) */
extern const SiftTopNWinRow *sift_v6_topn_run(MemoryContext cxt, const SiftV6TopNSpec *spec, SiftV6TopNSource *src, SiftTournament **tp, uint32 *n);

#endif							/* SIFT_AGG_V6_TOPN_H */
