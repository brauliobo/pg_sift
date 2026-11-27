/*
 * agg/dim_cache.h - the dimension of a star join kept for the next statement (agg/star/star.h).
 *
 * A dimension is the rows of a plain scan of a table that keeps a key journal (col/am/conn_route.h: a parent of a connection, of either format). The tables of a statement are kept, backend
 * local, under the budget of core/cachebudget.h. A later statement reads one as it is when the journal vouches for its snapshot, and no row was inserted into the table and no key changed in it
 * since the horizon the table was read under (the protocol of col/v1/conn/conn.h, taken before the read and asked again after it). Any change, committed or not, the statement's own included,
 * makes the next statement read the plan again. What the plan reads is part of what an entry stands for: the table, the user, the columns and the filters of the scan.
 * A table under row level security, a filter that is not immutable or takes a parameter, and any plan but a plain scan of one table are not kept.
 */
#ifndef SIFT_AGG_V6_DIM_CACHE_H
#define SIFT_AGG_V6_DIM_CACHE_H

#include "postgres.h"

#include "executor/executor.h"

#include "agg/ext.h"

typedef struct SiftDimTry SiftDimTry;

/* What a statement may keep of dimension xd (read by the plan ps into a table of shape sh); NULL when the plan is not one whose rows a journal can vouch for. Takes the stamp: call before the read. */
extern SiftDimTry *sift_dimcache_try(PlanState *ps, const SiftV6XDim *xd, const SiftStarShape *sh, MemoryContext mcx);
/* The table kept for this one, pinned until mcx is reset, into *out; false when none is, or it is over budget bytes */
extern bool sift_dimcache_find(SiftDimTry *t, uint64 budget, SiftStarDim *out);
/* The context to read the table into when it may be kept (a child of the statement's until it is) */
extern MemoryContext sift_dimcache_read_cx(SiftDimTry *t);
/* The table was read into sift_dimcache_read_cx(): keeps it when the table still is what the stamp says, true then (it is pinned until mcx is reset and the caller does not free it) */
extern bool sift_dimcache_keep(SiftDimTry *t, const SiftStarDim *dim);

/* The table every row of which the plan ps reads, with no filter, and the column of it that column col of its output is (*att 0: an expression); NULL for any other plan */
extern Relation sift_dimcache_whole_table(PlanState *ps, int col, AttrNumber *att);

#endif							/* SIFT_AGG_V6_DIM_CACHE_H */
