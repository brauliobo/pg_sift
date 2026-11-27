/*
 * agg/join/found.h - a dimension of a star join answered from the found flag of a connected index (col/am/conn_route.h), without reading the parent table.
 *
 * An inner, semi or anti join of the fact table to the parent of a connection of its index, by the key of the connection alone, that reads no column of the parent but the key and keeps every row of it,
 * asks one thing of each fact row: is there a parent row with its key. The flag the index keeps ("<fk>#found") says it as of the stamp it was filled under, so the parent is not read: a row
 * matches when its flag is set and its key is not among the keys the journal of the parent says changed since the oldest stamp, or when its key is one of those the parent holds now. That
 * holds for every row only if the index vouches for its flags (no write buffer, every part stamped by one journal), the journal vouches for the stamp under the statement's snapshot, and,
 * unless no row had a key the parent lacked, no parent row was inserted since. Else the dimension is read from its plan, and EXPLAIN says why.
 */
#ifndef SIFT_AGG_V6_JOIN_FOUND_H
#define SIFT_AGG_V6_JOIN_FOUND_H

#include "postgres.h"

#include "commands/explain.h"
#include "executor/executor.h"

#include "agg/engine.h"
#include "agg/ext.h"
#include "core/store/ops.h"

typedef struct SiftV6Found SiftV6Found;

/* What the found flags did for the dimensions of a node: the dimensions they answered, the keys the journals named, the parent btree probes, and why a dimension they could answer was read */
typedef struct SiftV6FoundStats
{
	uint32		proofs;
	uint64		keys;
	uint64		probes;
	const char *why;
} SiftV6FoundStats;

/* The scan of the fact table whose dimensions the flags may answer: one index of a format that keeps them, read by the aggregate (not the rows of a Top-N, not an unnest) */
typedef struct SiftV6FoundFact
{
	const SiftStoreOps *ops;
	Relation	heap;
	Relation	index;
	Snapshot	snap;
} SiftV6FoundFact;

/* Plan time: may the found flag of the fact index answer dimension d of x, read by child (the executor proves it, or reads the dimension): the node then pays no read of it */
extern bool sift_v6_found_planned(PlannerInfo *root, const SiftStoreOps *ops, Oid fact_index, const SiftV6Ext *x, int d, const Path *child);
/* Does the scan of spec over the fact read its rows through a fold the flags can join */
extern bool sift_v6_found_serves(const SiftV6Spec *spec, const SiftV6FoundFact *fact);
/* The answer for dimension d of x, read by the plan ps, in cx; NULL when the flags do not answer it (stats->why says why when they could have) */
extern SiftV6Found *sift_v6_found_open(const SiftV6FoundFact *fact, const SiftV6Ext *x, int d, PlanState *ps, MemoryContext cx, SiftV6FoundStats *stats);
/* The column of the fact scan that holds the flag */
extern SiftV6Col sift_v6_found_col(const SiftV6Found *f);
/* Clears from the rows of bs those the join does not keep: flag is the column of sift_v6_found_col, key the probing key of the dimension */
extern void sift_v6_found_apply(const SiftV6Found *f, const SiftVec *flag, const SiftVec *key, SiftBatchSel *bs);
extern void sift_v6_found_explain(const SiftV6FoundStats *s, ExplainState *es);

#endif							/* SIFT_AGG_V6_JOIN_FOUND_H */
