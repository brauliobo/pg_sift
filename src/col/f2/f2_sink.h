/*
 * col/f2/f2_sink.h - the rows of a scan of an index of format major 2 handed to a consumer in place of being collected as addresses (the aggregate scan of agg/v6/).
 */
#ifndef SIFT_COL_F2_SINK_H
#define SIFT_COL_F2_SINK_H

#include "access/relscan.h"

#include "col/granule/granule.h"
#include "core/f2/manifest.h"

typedef struct ColView
{
	SiftCellKind kind;
	const int64 *i;
	const double *f;
	const char *const *s;
	const uint32 *slen;
	const bool *isnull;
	const uint8 *tag;
	const bool *keyp;			/* of the leaf of a key: the rows that have the key */
	const char *const *xpay;	/* of the leaf of a key of a document: the payload of the exception of each row whose value of a JSON type the leaf cannot hold is not in it (a jsonb datum), NULL: no row has one */
	const uint32 *xplen;
	const uint32 *dcode;		/* a string leaf the sink asked for as codes (sift_f2_scan_set_dict_leaves) and the chunk holds as a dictionary: s and slen are then NULL */
	const SiftGranuleDict *dict;
} ColView;

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

/* The sink leaves (by their place among the leaves given to sift_f2_scan_sink; the array must outlive the scan) it takes as the codes of the dictionary of a granule when the chunk has one; call it before the scan runs */
extern void sift_f2_scan_set_dict_leaves(IndexScanDesc scan, const bool *dict_leaf);

/*
 * A parallel scan: the leader lists the units of the scan once, in memory a DSM chunk can hold (no pointer), and the participants take runs of the granules of the units. A unit is the granules of one part
 * that the key ranges and the zone maps of the part leave (a part with several ranges has a unit for each) or the L0 slab of a partition, which is one virtual granule. The descriptor of the part is in the
 * unit, so that a worker reads no manifest; the partition's own fields it needs (the column delta chain) are there too. flags: SIFT_PAR_PART_ALL_VISIBLE says the part is frozen with no deletion vector and no pending delete, so that no row of it needs the visibility map.
 */
#define SIFT_PAR_PART_L0_SLAB		(UINT16_C(1) << 0)
#define SIFT_PAR_PART_ALL_VISIBLE	(UINT16_C(1) << 2)
#define SIFT_PAR_PART_HAS_PEND_DEL	(UINT16_C(1) << 3)

typedef struct SiftParPartRef
{
	SiftPartDesc desc;
	uint64		part_id;
	BlockNumber l0_slab_blk;
	BlockNumber pend_del_blk;
	uint32		g_base;			/* the first granule of the unit, in its part */
	uint32		ngranules;
	uint16		flags;
	uint16		pad;
	uint32		entry_flags;	/* SIFT_PARTENTRY_* of its partition: a participant reads the column delta chain the partition has, as the serial scan does */
	uint64		manifest_gen;	/* where the chain's anchor is read: the partition's list (core/f2/manifest.h) */
	BlockNumber manifest_blk;
	uint32		pad2[13];
} SiftParPartRef;

StaticAssertDecl(sizeof(SiftParPartRef) == 320, "a scan unit is five cache lines");

typedef struct SiftF2ParPlan
{
	SiftParPartRef *refs;
	uint32		nrefs;
	uint64	   *staged_dead;	/* the TIDs the transaction deleted that no partition holds yet, sorted */
	uint32		nstaged;
} SiftF2ParPlan;

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
