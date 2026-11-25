/*
 * agg/v6/v6_colstat.h - what the aggregate scan knows of a part without reading its rows: whether every heap page of it is all-visible (one read of the visibility map for the whole
 * relation, 64 bits at a time) and, for an ungrouped aggregate of integer columns, the sum, count, least and greatest of each column of a part, kept by the backend for the next statement.
 *
 * A part is immutable and named by its generation, so the totals of its columns hold for every statement that finds the part all-visible with no deletion vector, pending delete or key
 * range touching it: those fold the totals into the aggregate and skip the part. The totals are a by-product of the scan that read the whole part. They are kept in the backend cache
 * (sift.backend_cache_kb) under the file of the index, the generation, the header page and the row count of the part.
 */
#ifndef SIFT_COL_F2_V6_COLSTAT_H
#define SIFT_COL_F2_V6_COLSTAT_H

#include "postgres.h"

#include "utils/rel.h"

#include "agg/v6/v6.h"
#include "agg/v6/vet.h"
#include "col/f2/sink/sink.h"
#include "col/f2/vacuum/vm.h"

/* The all-visible heap blocks of a relation, read once for a scan */
typedef struct V6VisMap
{
	SiftVmDirtySet dirty;
	MemoryContext cx;
	bool		built;
} V6VisMap;

extern void sift_v6_vis_init(V6VisMap *m, MemoryContext cx);
/* Whether every heap block of the addresses of the part is all-visible: then no row of it needs the heap */
extern bool sift_v6_vis_part_all_visible(V6VisMap *m, Relation heap, const SiftPartDesc *d);
/* The blocks of the addresses of the part that are not all-visible, at most max of them: how many, or -1 when the part has no address range or has more */
extern int	sift_v6_vis_dirty_blocks(V6VisMap *m, Relation heap, const SiftPartDesc *d, BlockNumber *out, int max);
extern void sift_v6_vis_free(V6VisMap *m);
/* Asks the index whether it was told of every delete the snapshot of the vet sees (SiftColFormatOps.deletes_heard) */
extern void sift_v6_vis_ask_index(SiftV6Vet *v, Relation index);
/* The rows of the part (NULL: a slab, which has none to say) are as the heap shows them to the snapshot, but for the deletes the index holds: its inserters ended before the snapshot and every delete was told to the index */
extern bool sift_v6_vis_part_settled(const SiftV6Vet *v, const SiftPartDesc *d);

/* Whether every aggregate is a MIN or MAX of an integer column and nothing else is asked: the zones of a part answer it */
extern bool sift_v6_spec_minmax_only(const SiftV6Spec *spec);

/* Whether the parts of the relation answer the spec whole (their rows, zones or totals): an ungrouped COUNT, MIN or MAX, SUM, AVG of integer columns that no predicate narrows */
extern bool sift_v6_spec_whole_parts(const SiftV6Spec *spec);

typedef struct V6ColStat V6ColStat;

/* The totals of the parts for a spec of ungrouped COUNT, SUM, AVG, MIN and MAX of integer columns; NULL for any other spec. first is the index's first leaf of each attribute */
extern V6ColStat *sift_v6_colstat_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation index, const int *first, MemoryContext cx);
/* The part is offered to the scan: when its totals are known they are folded into the group and true says the part is answered; else it is read, and whole says that its batches are all of its rows */
extern bool sift_v6_colstat_offer(V6ColStat *cs, const SiftPartDesc *d, bool whole);
/* A batch of the part last offered */
extern void sift_v6_colstat_note(V6ColStat *cs, const SiftF2Batch *b);
/* The scan is over: the last part read whole is kept */
extern void sift_v6_colstat_end(V6ColStat *cs);

#endif							/* SIFT_COL_F2_V6_COLSTAT_H */
