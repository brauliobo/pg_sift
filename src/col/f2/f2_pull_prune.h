/*
 * col/f2/f2_pull_prune.h - what the predicates of a pull (col/f2/f2_sink_ext.h) leave of an index: the partitions whose key range or time bounds miss them, the parts likewise, the granules of a
 * part outside its key marks (SEC_KEY_MARKS) or the zones of the lead key column, the granules whose zones of a leaf of the predicates say no row passes, and the granules in which no element of an
 * array passes the predicates of the elements. Every test errs towards reading: a predicate that no zone or mark can answer rules out nothing.
 *
 * The slab of a partition (the rows not yet in a part) is never ruled out by the time bounds, which are those of the parts, nor by a zone: only by the key range of its partition.
 */
#ifndef SIFT_COL_F2_PULL_PRUNE_H
#define SIFT_COL_F2_PULL_PRUNE_H

#include "col/f2/f2.h"
#include "col/f2/f2_sink_ext.h"
#include "col/f2/key_plan.h"
#include "table/f2/f2_col_delta.h"

typedef struct SiftF2PullPrune
{
	Relation	index;
	int			npred;
	const SiftColPred *pred;
	int			nelem;
	const SiftColPred *elem;
	bool		possible;		/* false: the predicates contradict one another, no row passes them */
	SiftF2KeySet keys;
	SiftKeyPlan plan;
	SiftF2TimeWin win;

	/* the part being read */
	const SiftPart *part;
	const SiftGranuleDesc *dir;
	int			nspans;
	int			span;
	uint32		next;			/* the next granule to look at, inside spans[span] */
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	SiftZone  **zones;			/* of pred and then elem: the zones of the granules of the leaf, NULL: the part keeps none */
	const SiftVariantExcSummary *exc;
	int			nexc;
} SiftF2PullPrune;

extern void sift_f2_pull_prune_begin(SiftF2PullPrune *pr, Relation index, const SiftF2PullSpec *spec);

/* The window without the time columns a column delta of the partition patches: the time bounds of its parts do not hold for them (SIFT_CDLA_DIRTY_TIME0/1) */
extern SiftF2TimeWin sift_f2_time_window_clean(Relation index, const SiftPartitionEntry *e, const SiftF2TimeWin *w);

/* Whether the partition (its slab and its parts) or a part of it cannot hold a row the predicates leave */
extern bool sift_f2_pull_prune_entry(const SiftF2PullPrune *pr, const SiftPartitionEntry *e);
extern bool sift_f2_pull_prune_part(const SiftF2PullPrune *pr, const SiftPartitionEntry *e, const SiftPartDesc *d);

/*
 * The part to read next: the granules its key marks leave, and the zones of the leaves of the predicates, kept in the current memory context until the next part is opened. Counts the granules outside
 * the key marks in st. cd: the overlay of the part's column deltas for the snapshot, NULL when it sees none: the zones of a leaf say nothing of the granules that overlay patches in it.
 */
extern void sift_f2_pull_prune_open(SiftF2PullPrune *pr, Relation index, const SiftPart *part, const SiftGranuleDesc *dir, const SiftColDeltaPartMap *cd, SiftF2PullStats *st);
/* The next granule of the open part that the key marks and the zones of the rows leave; false after the last. Counts the ones the zones ruled out in st */
extern bool sift_f2_pull_prune_next(SiftF2PullPrune *pr, uint32 *g, SiftF2PullStats *st);
/* Whether the zones of the element leaves say that no element of granule g, which has n_elems of them, passes the predicates of the elements */
extern bool sift_f2_pull_prune_elems(const SiftF2PullPrune *pr, uint32 g, uint32 n_elems);

#endif							/* SIFT_COL_F2_PULL_PRUNE_H */
