/*
 * agg/v6/stream.h - GROUP BY of one table over an index of format major 2 whose parts are sorted by the group keys, as a SiftAggV6Stream node (UPPERREL_GROUP_AGG): equal keys are adjacent in the
 * batches the format gives, so agg/stream_exec.h folds a group in the memory of one group and emits it as soon as the key changes: no hash table, no spill, and the groups leave in key order, which
 * the path declares to the planner. One to four keys of int4, int8, date or timestamp, or buckets of a timestamp (date_bin, date_trunc of a fixed unit);
 * count(*), and count, sum, avg, min and max of columns of int2, int4, int8, float8, numeric(p <= 18), date and timestamp, each under its FILTER; HAVING over them stays with the node as its qual. Not chosen under row security.
 */
#ifndef SIFT_AGG_V6_STREAM_H
#define SIFT_AGG_V6_STREAM_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/stream_exec.h"
#include "agg/v6/vexpr.h"

#define SIFT_V6_STREAM_MAX_AGGS	16
#define SIFT_V6_STREAM_MAX_FILTERS	4

/* An aggregate slot of the engine: the column it folds (an input slot, -1: it counts rows) under a FILTER (an index of the spec's filters, -1: none); aggregates of one column and filter share it */
typedef struct SiftV6StreamVal
{
	int			slot;
	int			filter;
	Oid			typid;
	int			scale;			/* of a numeric column */
	bool		is_f64;
} SiftV6StreamVal;

typedef struct SiftV6StreamAgg
{
	SiftV6Kind	kind;			/* SV6_COUNT_STAR, SV6_COUNT, SV6_SUM, SV6_AVG, SV6_MIN or SV6_MAX */
	Oid			restype;
	int			val;			/* the slot it reads, -1: the rows of the group */
} SiftV6StreamAgg;

/* A key that is the bucket of a timestamp column: date_bin(stride, column, origin), or date_trunc of a fixed unit over a timestamp, whose bucket never falls as the timestamp rises */
typedef struct SiftV6StreamBin
{
	int			key;
	int64		stride_us;
	int64		origin;
} SiftV6StreamBin;

typedef struct SiftV6StreamSpec
{
	Oid			indexoid;
	Index		rti;
	int			nkeys;
	int			key_slot[SIFT_STREAM_MAX_KEYS];	/* the column of the key (the timestamp of a bucket) */
	Oid			key_typid[SIFT_STREAM_MAX_KEYS];
	int			nbins;
	SiftV6StreamBin bin[SIFT_STREAM_MAX_KEYS];
	int			nfilters;
	const Node *filter[SIFT_V6_STREAM_MAX_FILTERS];
	int			nvals;
	SiftV6StreamVal val[SIFT_STREAM_MAX_VALS];
	int			naggs;
	SiftV6StreamAgg agg[SIFT_V6_STREAM_MAX_AGGS];
} SiftV6StreamSpec;

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_stream_plan_methods;

extern void sift_agg_v6_stream_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern Plan *sift_agg_v6_stream_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

/* The spec of the plan's private list (the oid of the source, rti, the source is a sift_table; scan tlist; key Vars; Aggref nodes; HAVING; WHERE clauses), its columns added to in; false when the engine does not run it */
extern bool sift_v6_stream_spec(const List *priv, SiftV6Input *in, SiftV6StreamSpec *out);

/* The result j of a group as the Datum of its PostgreSQL type; false: it is NULL (sum, min and max of no value) */
extern bool sift_v6_stream_datum(const SiftV6StreamSpec *spec, int j, const SiftStreamGroup *g, Datum *out);

/* The key i of a group as the Datum of its type */
extern Datum sift_v6_stream_key_datum(const SiftV6StreamSpec *spec, int i, const SiftStreamGroup *g);

/* The FILTER clauses of the spec compiled for the run (params: its values), into out[spec->nfilters]; false when the machine does not run one */
extern bool sift_v6_stream_filters(MemoryContext cxt, const SiftV6StreamSpec *spec, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out);

/* The engine over the source (which gives the rows that pass the WHERE clauses, sorted by the keys): every group goes to emit */
typedef struct SiftV6StreamRun SiftV6StreamRun;

/* The engine over src in the memory context cx: the groups come out by step, one batch of the input at a time, as final (emit), false once the input is out */
extern SiftV6StreamRun *sift_v6_stream_open(MemoryContext cx, SiftV6Source *src, const SiftV6StreamSpec *spec, const SiftV6Vexpr *filters, void (*emit) (void *, const SiftStreamGroup *), void *emit_arg);
extern bool sift_v6_stream_step(SiftV6StreamRun *r);
extern void sift_v6_stream_close(SiftV6StreamRun *r);

#endif							/* SIFT_AGG_V6_STREAM_H */
