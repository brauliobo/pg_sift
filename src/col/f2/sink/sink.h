/*
 * col/f2/sink/sink.h - the rows of a scan of an index of format major 2 handed to a consumer in place of being collected as addresses (the aggregate scan of agg/).
 */
#ifndef SIFT_COL_F2_SINK_H
#define SIFT_COL_F2_SINK_H

#include "access/relscan.h"

#include "col/col.h"
#include "col/f2/del/delvec.h"
#include "col/granule/granule.h"
#include "core/scan_types.h"
#include "core/f2/manifest/manifest.h"

/*
 * The rows of a granule or of the slab of a partition that every predicate of a scan accepted exactly (the dead and the pending deletes left out): tid[r] is the heap address of
 * row r, live has a bit for each of the n rows, and views[leaf] holds the rows of each leaf the consumer named. They live until the sink returns.
 */
typedef struct SiftF2Batch
{
	const ColView *views;
	const uint64 *tid;
	const uint64 *live;
	const float4 *scores;		/* a scan with a TID bridge: the score of each row of the bridge (what the bridge keeps, 0 without), the rows not in it already dropped from live; NULL without a bridge */
	const uint64 *maybe;		/* the rows a predicate could not decide on the stored values (a value the leaf could not hold: a NaN): neither live nor dead; always empty for sift_f2_scan_sink */
	const SiftPartDesc *part;	/* the part the rows are of; NULL for the slab of a partition */
	int			n;
} SiftF2Batch;

typedef void (*SiftF2Sink) (void *arg, const SiftF2Batch *b);

/* Runs the scan of the index (its keys given to the rescan) with every accepted row sent to the sink, whose leaves are decoded for it */
extern void sift_f2_scan_sink(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves);

/* The same for a consumer that takes the rows the stored values cannot decide (SiftF2Batch.maybe) and decides them on the relation's own row; sift_f2_scan_sink raises an error for one */
extern void sift_f2_scan_sink_maybe(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves);

/*
 * A scan with no predicate offers each part to its consumer before a byte of it is read, one with predicates offers each part it will read granules of (countable is then false), and then hands
 * it the batches of the part with the descriptor in SiftF2Batch.part. countable: no key
 * range, deletion vector, pending delete or bridge touches the part, so d->nrows are its rows; the consumer returns true when that answers it and the part is then not scanned. It sets
 * *tids_unneeded when it will not read the addresses of the rows (the batches then have none, unless a pending delete needs them to be decoded).
 */
typedef bool (*SiftF2PartSink) (void *arg, const SiftPartDesc *d, bool countable, bool *tids_unneeded);
extern void sift_f2_scan_set_part_sink(IndexScanDesc scan, SiftF2PartSink part_sink);

/*
 * A sink that takes leaves as codes (sift_f2_scan_set_dict_leaves) may be offered, before a part with no predicate, pending delete, deletion vector, bridge or column patch is read, the dictionaries of the
 * whole part of those leaves (dicts[j] of sink leaf j: n 0 when the part has none, or the leaf is not taken as codes): when the strings of a leaf are all the sink wants of it, they are all it needs of
 * the part, and it returns true: no granule of the part is read.
 */
typedef bool (*SiftF2PartDictSink) (void *arg, const SiftPartDesc *d, const SiftGranuleDict *dicts);
extern void sift_f2_scan_set_part_dict_sink(IndexScanDesc scan, SiftF2PartDictSink sink);

/*
 * A granule the scan is about to read, offered to the consumer before a byte of it is decoded (the pruning, the zones and the skipping indexes have left it): its place, the predicates of the scan, which of
 * them the zones or the indexes found true of every row (decided[k]) and the zone of the leaf of each (NULL: none), whether a value the zone holds is one the leaf could not keep (exceptions[k]), its deletion
 * vector for the snapshot and whether a pending delete may be in it. A consumer that answers the granule from what it stores returns true and the granule is not read.
 */
typedef enum SiftF2GranVis
{
	SIFT_GVIS_HEAP,				/* a sift_col index: the heap says what each row is, so the consumer knows them when the heap pages of the granule are all-visible */
	SIFT_GVIS_SETTLED,			/* a sift_table: the inserters of every row are settled for the snapshot, and the deletes it sees are those of the deletion vector and the pending deletes */
	SIFT_GVIS_PER_ROW			/* a sift_table: the rows are asked one by one (an inserter that is not settled, a snapshot that is not MVCC, a serializable transaction) */
} SiftF2GranVis;

typedef struct SiftF2GranOffer
{
	Relation	rel;
	const SiftPart *part;
	const SiftPartDesc *desc;
	const struct SiftColSchema *schema;
	uint32		g;
	const SiftGranuleDesc *gd;
	const SiftDvGran *dead;
	Snapshot	snap;
	const SiftColPred *preds;
	int			npreds;
	const bool *decided;
	const SiftZone *const *zones;
	const bool *exceptions;
	bool		pend_hit;		/* a pending delete may be in the granule */
	bool		patched;		/* a column delta changes cells of the granule */
	const struct SiftPendDelIndex *px;	/* the addresses the pending deletes and the deletes of the transaction take from the snapshot; NULL: none */
	SiftF2GranVis vis;
	const int16 *phys;			/* the column of the part that holds each leaf the scan names, -1: none that holds its cells as they are; NULL: the part has the leaves of the scan */
} SiftF2GranOffer;

typedef bool (*SiftF2GranSink) (void *arg, const SiftF2GranOffer *offer);
/* The consumer of sift_f2_scan_sink offered the granules too; the argument is the one of the sink */
extern void sift_f2_scan_set_gran_sink(IndexScanDesc scan, SiftF2GranSink gran_sink, void *gran_arg);

/*
 * The least and the greatest value of each sink leaf over the granules of a part that no key, predicate, deletion vector, pending delete, bridge, column delta or
 * exception touches, offered after the part is pinned and before a granule of it is read: the consumer returns true when that answers it and the part is then not scanned. zone[j] is the
 * sink leaf j; a part whose zones are not integers (or have a granule with no value) is not offered.
 */
typedef struct SiftF2PartZone
{
	int64		min;
	int64		max;
} SiftF2PartZone;

typedef bool (*SiftF2ZoneSink) (void *arg, const SiftPartDesc *d, const SiftF2PartZone *zone);
extern void sift_f2_scan_set_zone_sink(IndexScanDesc scan, SiftF2ZoneSink zone_sink);

extern void sift_f2_scan_set_bound(IndexScanDesc scan, int sink_leaf, const SiftF2Bound *bound);

/* The sink leaves (by their place among the leaves given to sift_f2_scan_sink; the array must outlive the scan) it takes as the codes of the dictionary of a granule when the chunk has one; call it before the scan runs */
extern void sift_f2_scan_set_dict_leaves(IndexScanDesc scan, const bool *dict_leaf);

/* Zone tests beyond the predicates of the keys (an OR of ranges, col/api/source.h), which must outlive the scan: a granule one of them rules out is not read; call it before the scan runs */
extern void sift_f2_scan_set_zones(IndexScanDesc scan, const SiftColPrune *zones, int nzones);

/* The granules a scan read, and those the zones and the skipping indexes ruled out before it read them (EXPLAIN of the aggregate scan) */
typedef struct SiftF2GranCounts
{
	uint64		scanned;
	uint64		pruned_zone;
	uint64		pruned_skip;
} SiftF2GranCounts;
/* The scan adds its granules to counts (which must outlive it); call it before the scan runs */
extern void sift_f2_scan_count_granules(IndexScanDesc scan, SiftF2GranCounts *counts);

/* The granules the scan would filter and decode go to the window instead, which the compute threads of the backend work through (col/f2/pin_win.h): call it before the scan runs */
struct SiftPinWin;
extern void sift_f2_scan_set_pin_win(IndexScanDesc scan, struct SiftPinWin *win);

extern void sift_f2_par_plan_build(IndexScanDesc scan, SiftF2ParPlan *out);
/* A participant: the sink and the leaves it reads, as for sift_f2_scan_sink; then the runs of granules [first, first + n) of a unit, any number of times */
extern void sift_f2_scan_morsel_begin(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves, const uint64 *staged_dead, uint32 nstaged);
extern void sift_f2_scan_morsel(IndexScanDesc scan, const SiftParPartRef *ref, uint32 first, uint32 n);
/* The participant is done: the part it holds open is given back */
extern void sift_f2_scan_morsel_end(IndexScanDesc scan);

/* A sink of sift_f2_scan_sink_maybe with a bridge: a granule that the bridge leaves a few rows of is not decoded for the predicates the chunks did not decide: its rows go to the sink undecided */
extern void sift_f2_scan_set_heap_decide(IndexScanDesc scan);

/* The granules the quals of the scan (given to the rescan) leave after the key ranges, the zones and the skipping indexes: none of them is read, the slab of a partition is not counted */
extern uint64 sift_f2_scan_survey(IndexScanDesc scan);

/* A hybrid scan: the granules the bridge misses are skipped and the rows not in it dropped before any column is decoded; seen and skipped count the granules */
struct SiftTidBridge;
extern void sift_f2_scan_set_bridge(IndexScanDesc scan, const struct SiftTidBridge *bridge, uint64 *seen, uint64 *skipped);

/* The parts of the generations given (ascending; they must outlive the scan) are not read: the slabs, the pending deletes and every other part are as in any scan. Call it after the rescan. */
extern void sift_f2_scan_skip_parts(IndexScanDesc scan, const uint64 *gens, int ngens);

/* The scan reads the manifest of the plan given (which must outlive it) and flushes no stage: the caller made the plan after its own flush, so the parts it left out and the rows the scan reads are one version of the relation, whatever commits meanwhile (and a hot standby has nothing to flush) */
struct SiftPlanSnapshot;
extern void sift_f2_scan_set_plan(IndexScanDesc scan, const struct SiftPlanSnapshot *plan);

/* The predicates of a hybrid scan that no index qual makes (SIFT_COLOP_IS_EXT: lower(col) LIKE 'p%', array && and @>) join those of the index quals; *parts_skipped counts the parts the bridge's key envelope rules out */
struct SiftColPred;
extern void sift_f2_scan_set_ext_preds(IndexScanDesc scan, const struct SiftColPred *preds, int npreds, uint64 *parts_skipped);

#endif							/* SIFT_COL_F2_SINK_H */
