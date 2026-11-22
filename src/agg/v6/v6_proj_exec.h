/*
 * agg/v6/v6_proj_exec.h - the executor side of the choice of a projection (v6_proj.h) for a SiftAggV6 node: when a pre-aggregate (SIFT_PROJ_AGG) stands for some of the base parts of the relation
 * under the statement's snapshot and costs less than scanning them, the groups come from its cells for those parts and from the relation for the rest: the parts it does not stand for, the slabs
 * and the rows the snapshot has deleted since a part was projected. The answer is the one the scan gives, row for row, for a sift_col index and a sift_table alike.
 */
#ifndef SIFT_AGG_V6_PROJ_EXEC_H
#define SIFT_AGG_V6_PROJ_EXEC_H

#include "agg/v6/v6.h"
#include "agg/v6/v6_proj_host.h"
#include "col/f2/f2_proj.h"

typedef struct SiftV6ProjResult
{
	bool		used;
	char		name[SIFT_PROJ_NAME_LEN];
	int			nparts;			/* projection parts read */
	int			nsubtracted;	/* of them, the base parts whose dead rows were taken away */
	int			nbase;			/* base parts the projection does not stand for: read from the relation with the slabs */
	int64		nrest_rows;		/* the rows those reads found */
	int			natts;
	int			nrows;
	int			next;
	Datum	   *values;			/* nrows * natts: the keys then the aggregates of each group */
	bool	   *isnull;
} SiftV6ProjResult;

/* false: no projection answers the statement, read the relation (nothing was changed); true: the groups are in out, allocated in cx. h->preds are the statement's predicates. */
extern bool sift_v6_proj_serve_agg(const SiftV6ProjHost *h, const SiftV6Spec *spec, MemoryContext cx, SiftV6ProjResult *out);

/* The same for a SiftAggV6 node over a sift_col index: the host and its predicates are made from the index, its heap and the keys of the scan */
extern bool sift_v6_proj_serve(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, MemoryContext cx, SiftV6ProjResult *out);

/* The same for a SiftAggV6 node over a sift_table: the host is the table, which has no heap, and its predicates are those of keys */
extern bool sift_v6_proj_serve_table(const SiftV6Spec *spec, Relation rel, ScanKey keys, int nkeys, Snapshot snap, MemoryContext cx, SiftV6ProjResult *out);

/* The next group as the Datums of the keys then the aggregates; false after the last */
extern bool sift_v6_proj_next(SiftV6ProjResult *r, Datum *values, bool *isnull);

#endif							/* SIFT_AGG_V6_PROJ_EXEC_H */
