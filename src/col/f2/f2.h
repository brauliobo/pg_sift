/*
 * col/f2/f2.h - the sift_col access method on format major 2: a CREATE INDEX with format_major = 2 builds parts of 8192-row granules in a partitioned manifest (col/col_meta.h,
 * core/manifest.h), a row an INSERT writes is staged by the transaction and reaches the index as an L0 part or a slab batch (col/l0_stage.h), VACUUM deletes the dead TIDs through the
 * deletion vectors (col/delete.h) and compacts (col/compact.h), and a scan reads the parts, the slab and the pending deletes of every partition.
 *
 * The leaves of the index are those of format major 1 (col/am/rel.c: the columns, the shredded paths, the numeric, uuid and text images); what changes is where their values are kept.
 * A scan returns the TIDs of the rows whose stored values pass the quals; the heap tests the quals again only for the rows a leaf could not hold (and checks visibility for all).
 */
#ifndef SIFT_F2_H
#define SIFT_F2_H

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "commands/trigger.h"
#include "nodes/tidbitmap.h"

#include "col/am.h"
#include "col/am/format_ops.h"
#include "col/f2/col_meta.h"
#include "col/f2/f2_kll.h"
#include "col/f2/rowbuf.h"

/* What a relation of format major 2 derives from its leaves, kept with the leaves (SiftColRel.f2) */
typedef struct SiftF2Rel
{
	int			ncols;
	SiftCellKind *kinds;
	uint8	   *hints;
	uint8	   *scales;
	uint16		key_cols[SIFT_KEY_ORDER_MAX];	/* the leaves of key_order, in order; none: the index is unkeyed and its rows are in the order of their TIDs */
	int			nkey;
	int16	   *parent;			/* per leaf the leaf of the lengths of the array it is an element of, -1: a leaf of the rows */
	bool	   *key_leaf;		/* per leaf: the leaf of a top-level key of a jsonb document (its chunks keep on which rows jsonb_exists(document, key) holds) */
	const char **key;			/* of such a leaf the key, not terminated (a pointer into the path of its definition), and its length */
	uint16	   *key_len;
	int16	   *key_attno;		/* and the attribute of the index it is a key of */
	bool		has_keys;
	bool		has_groups;
	int64		time_unit[SIFT_MANIFEST_TIME_COLS];	/* microseconds in one step of the time column: 1 for a timestamp, a day for a date; 0: no such time column */
	/* MEM[MEM-COL-15]: SiftF2Rel 280 B; the 1 KB SiftF2KllCols is allocated only with the sketches option (-1028 B) */
	SiftF2KllCols *kll;			/* the columns of the kll groups of the sketches option, as leaves */
	SiftPartSpec spec;			/* spec.time_cols names the time columns */
} SiftF2Rel;

extern SiftF2Rel *sift_f2_rel(Relation index);
/* The types of the member and of the key of a pair a summary is made of */
extern bool sift_f2s_pair_types(Oid member, Oid key);

/* The leaf of the key of the jsonb attribute attno that jsonb_exists(document, key) is answered from (-1: none keeps it) */
extern int	sift_f2_key_leaf(const SiftF2Rel *f, int attno, const char *key, int len);

/* The cells of a row, the cells of the elements of its arrays (elem[c] for a leaf of elements, made in rowcxt) and its key (the 40 bytes of core/range.h that route it to its partition and order it) */
extern void sift_f2_row(Relation index, SiftF2Rel *f, MemoryContext rowcxt, const ItemPointerData *tid, const Datum *values, const bool *isnull, SiftCell *cells, SiftCell **elem, uint8 *key);

/* The key of the row an index entry was made for, from the values of the index columns of its tuple (the key columns only: the row is not shredded); cells: f->ncols zeroed cells the keys' leaves are written to */
extern void sift_f2_key_of(const SiftColRel *r, const SiftF2Rel *f, SiftCell *cells, const ItemPointerData *tid, const Datum *values, const bool *isnull, uint8 *key);

/* The row trigger of track_changes of an index of format major 2 (track_f2.c): the one AFTER UPDATE OR DELETE trigger of the table; the version of a row a statement ended, at the address of
 * the root of its chain, is a delete of the index at the commit. The triggers of an index follow its option (col/track.c); a NOTICE when the table is held and they are left to ALTER INDEX */
extern Datum sift_f2_track_trigger(PG_FUNCTION_ARGS);
extern void sift_f2_track_reconcile(Relation index);
extern void sift_f2_track_install(Relation heap, Relation index, bool concurrent);

/* The address of a heap tuple as the part stores it, and back */
static inline uint64
sift_f2_tid(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static inline void
sift_f2_tid_set(ItemPointer tid, uint64 addr)
{
	ItemPointerSet(tid, (BlockNumber) (addr >> 16), (OffsetNumber) (addr & 0xFFFF));
}

typedef enum ZoneVerdict
{
	Z_MAYBE,					/* the zone does not decide */
	Z_NONE,						/* no row of the granule passes the predicate */
	Z_ALL						/* every row of the granule passes it: it is not tested */
} ZoneVerdict;

/* What the zone of a granule of nrows rows says of a predicate on column d; exceptions: some row of the granule holds a value the column could not */
extern ZoneVerdict sift_f2_zone_verdict(const SiftColDef *d, const SiftColPred *p, const SiftZone *z, uint32 nrows, bool exceptions);

#ifdef SIFT_TESTING
/* What the scans of this backend did since the last read (sift._test_f2_scan_counters): the addresses they returned exact and those the heap has to test again, the granules the zones ruled out and the granules they read, and those read for the rows a sparse column lists */
typedef struct SiftF2ScanStats
{
	int64		exact;
	int64		recheck;
	int64		zone_skipped;
	int64		read;
	int64		sparse;
	int64		parts_read;				/* the parts whose header a scan opened */
	int64		parts_time_skipped;		/* the parts, and */
	int64		partitions_time_skipped;	/* the partitions (their whole part lists), the bounds of their time columns ruled out */
	int64		skip_probed;			/* the predicates of a granule the skipping indexes were asked about, and */
	int64		skip_pruned;			/* the granules they ruled out */
	int64		part_pruned;			/* the parts the dictionary of a leaf rules out whole */
	int64		compressed;				/* the predicates of a granule that ran on a chunk as it is stored */
	int64		late_skipped;			/* the granules the chunks left no row of before the _tid and the other columns were read */
	int64		prefetched;				/* the pages a scan asked of the storage ahead of reading the granules that lie on them */
} SiftF2ScanStats;

extern SiftF2ScanStats sift_f2_scan_stats;
#endif

/* The keys a scan is confined to (f2_prune.c): the integers of the lead key column its predicates allow, as bounds in the order of the keys of a part (core/range.h) */
typedef struct SiftF2KeyRange
{
	uint8		lo[SIFT_BOUND_BYTES];
	uint8		hi[SIFT_BOUND_BYTES];	/* exclusive */
	bool		has_lo;
	bool		has_hi;
	bool		null_only;
	int64		ilo;
	int64		ihi;
} SiftF2KeyRange;

/* The ranges of an IN list on the lead key column that is short enough to probe each value apart, else the one range of all the predicates; hull holds them all */
#define SIFT_F2_MAX_RANGES	16

typedef struct SiftF2KeySet
{
	int			n;
	SiftF2KeyRange range[SIFT_F2_MAX_RANGES];
	SiftF2KeyRange hull;
} SiftF2KeySet;

/* The key set of the predicates; false when no row can satisfy them */
extern bool sift_f2_key_set(const SiftF2Rel *f, const SiftColPred *preds, int npreds, SiftF2KeySet *ks);

/* The integers the predicates allow the time columns (SiftF2Rel.spec.time_cols), as closed ranges; a column no predicate narrows is not used (use[s] false) */
typedef struct SiftF2TimeWin
{
	bool		use[SIFT_MANIFEST_TIME_COLS];
	int64		lo[SIFT_MANIFEST_TIME_COLS];
	int64		hi[SIFT_MANIFEST_TIME_COLS];
} SiftF2TimeWin;

/* The time window of the predicates; false when no row can satisfy them */
extern bool sift_f2_time_window(const SiftF2Rel *f, const SiftColPred *preds, int npreds, SiftF2TimeWin *w);

/* May the immutable parts of a partition, or a part, hold a row inside the window (by the exact bounds of their time columns); the slab of a partition is not judged here: it is always read */
extern bool sift_f2_entry_meets_time(const SiftPartitionEntry *e, const SiftF2TimeWin *w);
extern bool sift_f2_part_meets_time(const SiftPartDesc *d, const SiftF2TimeWin *w);

/* The first time column's value below which rows are cold (30 days before now, from a day boundary), in the unit of that column; PG_INT64_MIN when the index has no time column */
extern int64 sift_f2_hot_cut(const SiftF2Rel *f);

struct SiftF2Store;

/* VACUUM, after its compactions: what pins the high-water mark above the free space under it (a part, a scope summary, a projection part, the pages of a projection directory) is rewritten into that space, so that the cut of the file can take its old place (f2_tail.c) */
extern void sift_f2_tail_relocate(Relation rel, const struct SiftF2Store *store, FullTransactionId horizon, int64 hot_cut);

/* May a part hold a key of one of the ranges (by the exact bounds of its keys) */
extern bool sift_f2_part_overlaps(const SiftPartDesc *d, const SiftF2KeySet *ks);

struct SiftKeyPlan;

/* The granules of an opened part that the key ranges meet, by the marks of its whole key when the part has them and plan is a set of ranges, else by the zones of the lead column, as ascending disjoint spans [spans[i][0], spans[i][1]) found by binary search; returns their number (at most SIFT_F2_MAX_RANGES) */
extern int	sift_f2_granule_spans(Relation rel, const SiftPart *part, const SiftF2Rel *f, const SiftF2KeySet *ks, const struct SiftKeyPlan *plan, uint32 spans[][2]);

/* The addresses a snapshot sees deleted by the pending deletes of a partition, ascending (NULL: none) */
extern uint64 *sift_f2_pending_dead(Relation index, const SiftPartitionEntry *e, Snapshot snap, int *n);
struct SiftPendDelIndex;
extern struct SiftPendDelIndex *sift_f2_staged_index(const uint64 *staged, int nstaged);
extern struct SiftPendDelIndex *sift_f2_pending_index(Relation index, const SiftPartitionEntry *e, Snapshot snap, const struct SiftPendDelIndex *staged_px);

/* The manifest with the slabs of the partitions written out as parts and the delta tails of their deletion vectors folded into the vectors */
extern SiftManifestRead sift_f2_settled_manifest(Relation rel, const SiftPartSpec *spec);

/* The SQL functions of the file of the index (f2_maint.c) */
extern Datum sift_col_v2_sql(SiftColSqlFn fn, FunctionCallInfo fcinfo);
extern void sift_f2_check(struct SiftCheck *ck, Oid indexoid, bool heapallindexed);
extern void sift_f2_check_relation(struct SiftCheck *ck, Relation rel);
extern void sift_f2_check_heap(Relation index);

/* Checks the key invariants of every part (f2_check.c); ERROR when one fails, else a summary */
extern char *sift_f2_check_keys(Relation index);

extern IndexBuildResult *sift_f2_ambuild(Relation heap, Relation index, IndexInfo *info);
extern void sift_f2_ambuildempty(Relation index);
/* The row of a heap tuple in the stage of the transaction (col/f2/l0_stage.h) */
extern void sift_f2_stage_row(Relation index, const Datum *values, const bool *isnull, ItemPointer heap_tid);
extern bool sift_f2_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo);
extern IndexBulkDeleteResult *sift_f2_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *sift_f2_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
/* The steps of a VACUUM on the index, without the heap's: the slab, the compaction, the summaries, the end of the file */
extern void sift_f2_maintain(Relation index);
extern void sift_f2_amcostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
								   double *indexCorrelation, double *indexPages);
/* An index-only scan returns the columns of a fixed size that the leaves hold as they are: integers, dates, timestamps, booleans and floats */
extern IndexScanDesc sift_f2_ambeginscan(Relation index, int nkeys, int norderbys);
extern void sift_f2_amrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys);
extern int64 sift_f2_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool sift_f2_amgettuple(IndexScanDesc scan, ScanDirection dir);
extern bool sift_f2_amcanreturn(Relation index, int attno);
extern void sift_f2_amendscan(IndexScanDesc scan);

#endif							/* SIFT_F2_H */
