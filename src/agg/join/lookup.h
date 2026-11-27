/*
 * agg/join/lookup.h - what the dimensions of a join of SiftAggV6 do beyond the equality probe of v6_star.h: the rows a RIGHT or FULL join keeps whether or not a fact row matched them (the rows of
 * the dimension are marked as the scan matches them and the ones left come out when it ends), and the conditions of the join that no key says (an inequality between the two sides, a WHERE over the
 * columns of a left join), which PostgreSQL's own expression evaluator runs on the pairs of a batch.
 *
 * A key of a dimension may be one more than the fact table's: the element of an unnest, or a payload column of an earlier dimension (SiftV6XDim.fsrc), and the dimension may be the result of any plan of
 * PostgreSQL (a table, a VALUES list, a CTE, a sub-select, a tree of joins) that the node reads whole when the scan starts. The hot paths of the node (a dimension whose keys are all different, probed by
 * one lookup a row, and the filter of the keys pushed into the scan: v6_star.h, v6_sip.h) do not come here.
 */
#ifndef SIFT_AGG_V6_JOIN_LOOKUP_H
#define SIFT_AGG_V6_JOIN_LOOKUP_H

#include "postgres.h"

#include "commands/explain.h"
#include "executor/executor.h"

#include "agg/ext.h"
#include "agg/star/star.h"

/* dim_row[i] (1-based, 0: none) of the n pairs of a batch matched the rows they name: marks them, for a dimension of a RIGHT or FULL join */
extern void sift_v6_lookup_mark(const SiftStarDim *d, const uint32 *dim_row, uint32 n);
/* The rows (1-based) of the dimension no pair matched, at most cap of them from *next (0 at the first call); returns how many, 0 when there are no more */
extern uint32 sift_v6_lookup_unmatched(const SiftStarDim *d, uint32 *next, uint32 *dim_row, uint32 cap);

/*
 * The tables of the dimensions are shared between the nodes of one statement: a dimension that is a plain scan of a table (the arms of a UNION ALL, a CTE read several times, a self join each
 * join the same small table) is read once into a table that lives as long as the statement, and the node that finds it there uses it. The table must be the same: the same relation, columns and
 * conditions of the scan, and the same keys, payload and mode (a dimension a RIGHT or FULL join keeps is marked by its own node, and is not shared).
 */
typedef struct SiftV6SharedDim SiftV6SharedDim;

/* NULL when the dimension of plan ps cannot be shared; else *hit is the table another node read (NULL: none yet, and the context to read it into is sift_v6_shared_dim_cx of the result) */
extern SiftV6SharedDim *sift_v6_shared_dim_open(const PlanState *ps, const SiftV6XDim *xd, const SiftStarShape *sh, const SiftStarDim **hit);
extern MemoryContext sift_v6_shared_dim_cx(const SiftV6SharedDim *h);
/* The table was read in that context: the next node of the statement finds it */
extern void sift_v6_shared_dim_publish(SiftV6SharedDim *h, const SiftStarDim *dim);
/* The table was not read (it does not fit): the context goes */
extern void sift_v6_shared_dim_abort(SiftV6SharedDim *h);

typedef struct SiftV6Resid SiftV6Resid;

/* The conditions a batch is tested with: those of the whole join (SIFT_V6_RESID_JOIN, x->resid) and those of the ON clause of dimension d (x->on[d]) */
#define SIFT_V6_RESID_JOIN	(-1)

/* The evaluators of x->resid and x->on, in cx; NULL when the join has no such condition */
extern SiftV6Resid *sift_v6_resid_new(MemoryContext cx, const SiftV6Ext *x);
extern void sift_v6_resid_free(SiftV6Resid *r);
/* whether there are conditions of that kind */
extern bool sift_v6_resid_has(const SiftV6Resid *r, int dim);
/* cols[i] is the vector of x->rcol[i], read only for the columns the conditions name; clears the rows of bs the conditions do not keep (NULL is not true) */
extern void sift_v6_resid_filter(SiftV6Resid *r, int dim, const SiftVec *const *cols, SiftBatchSel *bs);

/* The Lookup line of EXPLAIN ANALYZE: the rows the plans of the dimensions (dims, their PlanStates) read and passed on, and the index scans they ran */
extern void sift_v6_explain_lookup_reads(List *dims, ExplainState *es);

#endif							/* SIFT_AGG_V6_JOIN_LOOKUP_H */
