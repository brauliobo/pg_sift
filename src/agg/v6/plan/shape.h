/*
 * agg/v6/plan/shape.h - what the planner of SiftAggV6 asks of a statement before it offers the path of an operator of this directory, as functions of plain descriptions of the statement:
 * the adapters that read PostgreSQL's nodes (v6_filter_pg.c, join.c, join_asof.c, v6_datebin_plan.c, v6_winx_glue.c) fill a description and take the verdict, and these functions need no server, so
 * the standalone tests run every shape, the ones that are taken and each reason one is refused for.
 *
 *   FILTER   the distinct clauses of a statement get one number each (at most 64: one bit of the row mask of v6_filter_pivot.h each); a 65th distinct clause keeps the statement with PostgreSQL
 *   GRACE    an inner, left, semi or anti join of equality clauses over hashable operators and no other join clause, whose build side does not fit in half of work_mem
 *   ASOF     FROM fact LEFT | INNER JOIN LATERAL (SELECT .. FROM dim WHERE dim.k = fact.k AND dim.ts <= fact.ts ORDER BY dim.ts DESC LIMIT 1) ON true, and the mirror with >= and ASC
 *   DATEBIN  date_bin(stride, ts, origin) with constants a stride and an origin that PostgreSQL accepts for every source, as a GROUP BY key
 *   WINDOW   the window functions SiftWinx computes (FILTER, the exact NUMERIC average) with the frame it has
 */
#ifndef SIFT_AGG_V6_PLAN_SHAPE_H
#define SIFT_AGG_V6_PLAN_SHAPE_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/v6/join/asof.h"
#include "agg/v6/win/datebin.h"
#include "agg/v6/filter/pivot.h"
#include "agg/v6/join/grace.h"

typedef enum SiftShapeVerdict
{
	SSV_OK = 0,
	SSV_FILTER_TOO_MANY,		/* more than SIFT_PIVOT_MAX_FILTERS distinct clauses */
	SSV_JOIN_TYPE,				/* not inner, left, semi or anti */
	SSV_JOIN_NO_EQUI,
	SSV_JOIN_RESIDUAL,			/* a join clause that is not an equality */
	SSV_JOIN_UNHASHABLE,
	SSV_JOIN_FITS,				/* the build side fits in half of work_mem: PostgreSQL's hash join stands */
	SSV_ASOF_JOIN_TYPE,
	SSV_ASOF_SUBQUERY,			/* the sub-select is more than one table, or groups, aggregates, windows or sets */
	SSV_ASOF_LIMIT,				/* not LIMIT 1 */
	SSV_ASOF_OFFSET,
	SSV_ASOF_ORDER,				/* no ORDER BY of the one column the order is on, or its direction does not match the comparison */
	SSV_ASOF_NO_KEY,			/* no equality of the keys */
	SSV_ASOF_KEYS,				/* more than one equality */
	SSV_ASOF_TS_QUAL,			/* no comparison of the times, or more than one */
	SSV_ASOF_OTHER_QUAL,		/* a clause of the sub-select that is none of those */
	SSV_ASOF_ON,				/* the join clause is not ON true */
	SSV_ASOF_TYPE,				/* a key or time that is not one int64 whose order is the type's, or the two sides have other types */
	SSV_DATEBIN_ARGS,			/* a NULL or non-constant stride or origin */
	SSV_DATEBIN_STRIDE,			/* not positive, or its microseconds overflow */
	SSV_DATEBIN_MONTHS,			/* a stride with months or years: PostgreSQL raises it */
	SSV_DATEBIN_ORIGIN,			/* an infinite origin */
	SSV_DATEBIN_TYPE,			/* the source is not a timestamp or timestamptz leaf the scan decodes */
	SSV_WIN_FUNC,				/* a function SiftWinx does not have */
	SSV_WIN_TYPE,
	SSV_WIN_FRAME,				/* a frame that is not from the start of the partition to the current row */
	SSV_WIN_TOO_MANY,
	SSV_WIN_NO_NEED				/* every function is of the executor of agg/window.h: it stays */
} SiftShapeVerdict;

extern const char *sift_shape_name(SiftShapeVerdict v);

/* ---- FILTER ---- */

typedef struct SiftFilterReg SiftFilterReg;

extern SiftFilterReg *sift_freg_create(MemoryContext cx);
/* The number of the clause whose canonical bytes are given (the same bytes: the same number), -1 when it would be the 65th distinct clause */
extern int	sift_freg_add(SiftFilterReg *r, const void *canon, uint32 len);
extern int	sift_freg_count(const SiftFilterReg *r);
extern void sift_freg_free(SiftFilterReg *r);

/* ---- Grace join ---- */

typedef enum SiftShapeJoinType
{
	SSJ_INNER,
	SSJ_LEFT,
	SSJ_RIGHT,
	SSJ_FULL,
	SSJ_SEMI,
	SSJ_ANTI
} SiftShapeJoinType;

typedef struct SiftJoinShape
{
	SiftShapeJoinType type;
	int			nequi;			/* equality clauses over hashable operators */
	int			nother;			/* join clauses of any other kind */
	int			nunhashable;	/* equality clauses whose operator has no hash function */
	double		build_rows;
	double		build_width;
	uint64		work_mem_bytes;
} SiftJoinShape;

/* The bytes a hash table of the build side takes: what the planner compares with half of work_mem */
extern uint64 sift_shape_build_bytes(double rows, double width);
extern SiftShapeVerdict sift_shape_join(const SiftJoinShape *s, SiftJoinType *type);

/* ---- ASOF ---- */

typedef enum SiftAsofCmp
{
	SAC_LE,						/* dim.ts <= fact.ts */
	SAC_LT,
	SAC_GE,
	SAC_GT
} SiftAsofCmp;

typedef struct SiftAsofShape
{
	SiftShapeJoinType type;
	bool		on_true;
	bool		single_table;	/* the sub-select reads one relation and has no group, aggregate, window, DISTINCT or set operation */
	bool		limit_one;
	bool		has_offset;
	int			nsort;
	bool		sort_on_ts;
	bool		sort_desc;
	int			nkey_eq;		/* dim.k = fact.k */
	int			nts_cmp;
	SiftAsofCmp ts_cmp;
	int			nother;
	bool		types_ok;		/* k and ts of dim and fact: the same type, one of int2, int4, int8, date, timestamp, timestamptz */
} SiftAsofShape;

extern SiftShapeVerdict sift_shape_asof(const SiftAsofShape *s, SiftAsofSpec *spec);

/* ---- date_bin ---- */

typedef struct SiftDateBinShape
{
	bool		stride_const;
	bool		stride_null;
	bool		origin_const;
	bool		origin_null;
	bool		source_ok;		/* a timestamp or timestamptz column the scan decodes, with the type of the origin */
	SiftInterval16 stride;
	int64		origin;
} SiftDateBinShape;

extern SiftShapeVerdict sift_shape_datebin(const SiftDateBinShape *s, SiftDateBin *out);

/* ---- windows ---- */

typedef struct SiftWinShapeFunc
{
	SiftWinxKind kind;
	SiftVecType arg_type;
	bool		has_filter;
	bool		known;			/* false: a function SiftWinx has not */
} SiftWinShapeFunc;

typedef struct SiftWinShape
{
	int			nfuncs;
	const SiftWinShapeFunc *func;
	bool		frame_from_start;	/* UNBOUNDED PRECEDING to CURRENT ROW, no EXCLUDE, no offsets */
	bool		window_mode;	/* a window, not DISTINCT ON */
	bool		topk;			/* a run condition (WHERE rn <= K): the executor of agg/window.h stops the partition there */
} SiftWinShape;

/* SSV_OK: SiftWinx runs the window (a function has a FILTER or a numeric argument); SSV_WIN_NO_NEED: the executor of agg/window.h does */
extern SiftShapeVerdict sift_shape_winx(const SiftWinShape *s);

#endif							/* SIFT_AGG_V6_PLAN_SHAPE_H */
