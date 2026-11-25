/*
 * col/v1/build.h - the build of a sift_col index in two files: col/v1/build/build.c turns the rows of heap pages into
 * batches and the batches into segments (a ColBuild is one participant) and runs the serial build; col/v1/build_par.c shares the
 * superblocks of the table between the participants of a parallel build.
 */
#ifndef SIFT_COL_BUILD_H
#define SIFT_COL_BUILD_H

#include "access/genam.h"
#include "datatype/timestamp.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/s_lock.h"
#include "utils/snapshot.h"

#include "col/am.h"
#include "col/v1/enc/enc.h"
#include "col/v1/seg/seg.h"

#define SIFT_COL_PHASE_SCAN		2
#define SIFT_COL_PHASE_WRITE	3
#ifdef SIFT_TESTING
/* gates of sift.test_phase_gate that are no progress phase: before the locks of the end of the build are taken, and in the fill of the scope segments of a concurrent build */
#define SIFT_COL_GATE_TAIL		4
#define SIFT_COL_GATE_FILL		5
#define SIFT_COL_GATE_JOURNAL	6
/* a delete that found the units of its rows, before it appends their deletion records (col/v1/track/dv.c) */
#define SIFT_COL_GATE_DELETE	8
#endif

/* The runs a key_cut build keeps before it merges some: they overlap in the key and the merge that ends the build opens them all, so this bounds its memory, not the directory */
#define SIFT_COL_BUILD_RUNS		121
#define SIFT_BUILD_SOURCE_AUTO	0
#define SIFT_BUILD_SOURCE_HEAP	1
extern PGDLLIMPORT int sift_col_build_source;
extern void sift_col_build_define_gucs(void);
/* The workers a build through a btree may have (the participants take ranges of the key; col/v1/build/btree_par.c) */
#define SIFT_COL_BTREE_WORKERS	15

typedef struct ColBuild ColBuild;

extern Size sift_colb_budget(int nslots);
extern void sift_colb_init_meta(Relation index);

/*
 * A segment is cut when its writer holds more than budget bytes. segs: NULL, the build keeps its segments in an array that grows; else the entries of a participant of a parallel build,
 * segcap of them (sift_colb_segs_bound). shared: a participant of a parallel build, which writes through a SHARED target and takes the generations of its
 * segments from the metapage; the rows and heap blocks it did go to the progress of the leader.
 */
/* What the formula of the design notes looked at in the strings of an index being built, by kind and leaf (past the first SIFT_COL_SCAN_LEAVES none), summed over its participants: [0] the LZ4 chosen, [1] the zstd kept */
#define SIFT_COL_SCAN_LEAVES	64
typedef struct ColScanAcc
{
	slock_t		mutex;
	SiftCcScanSum sum[SIFT_SCAN_KINDS][SIFT_COL_SCAN_LEAVES][2];
} ColScanAcc;

/* The writes of this backend add to acc until it is watched with NULL; the leader reports the sums with one NOTICE at the end of the build */
extern void sift_colb_scan_watch(ColScanAcc *acc);
extern void sift_colb_scan_report(Relation index, ColScanAcc *acc);
extern ColBuild *sift_colb_begin(Relation index, SiftWriteTarget *wt, Size budget, SiftSegEntry *segs, int segcap, bool shared, int nslots, bool scopes);
/* The segments the build wrote, in the order of the directory: valid until it is freed */
extern SiftSegEntry *sift_colb_segments(const ColBuild *b);
extern int sift_colb_segs_bound(Relation index, uint32 nsbs);
/* Scans the whole heap (serial build) or the superblock sb of nblocks heap blocks (parallel); both return the heap tuples scanned */
extern double sift_colb_scan(ColBuild *b, Relation heap, IndexInfo *info);
extern double sift_colb_scan_sb(ColBuild *b, Relation heap, IndexInfo *info, Snapshot snapshot, uint32 sb, BlockNumber nblocks);
/* Writes what is left; returns the number of segments, in segs */
extern int	sift_colb_end(ColBuild *b);
extern double sift_colb_rows(const ColBuild *b);
extern void sift_colb_free(ColBuild *b);
/*
 * Publishes the segments of a build in the directory, ordered by their first superblock. stamped: the build ran under the lock that holds the writers out
 * (not CONCURRENTLY) of an index with track_changes, so every transaction that inserted a row it read has finished: the segments say that every inserter is before the
 * next xid (SIFT_SEG_XSTAMP). A row the building transaction itself inserted is of an xid below it too, and no snapshot has an xmin above that xid before it commits.
 */
extern void sift_colb_publish(Relation index, SiftSegEntry *segs, int nsegs, bool stamped);

/* The segments of a participant of a parallel build: slot i of the parts of segcap entries each (col/v1/build_par.c) */
typedef struct SiftColPart
{
	int			nsegs;
	SiftSegEntry segs[FLEXIBLE_ARRAY_MEMBER];
} SiftColPart;
extern Size sift_colp_part_size(int segcap);
extern SiftColPart *sift_colp_part(char *parts, int segcap, int slot);
extern SiftSegEntry *sift_colp_collect(char *parts, int segcap, int nslots, int *nsegs);

/* col/v1/build_par.c: the whole build with workers, NULL when none could be launched (nothing is written then) */
extern IndexBuildResult *sift_colp_build(Relation heap, Relation index, IndexInfo *info);
/* The merge of the segments of a parallel build by the participants, each a slice of the key range (col/v1/build_par.c); false: no worker could be launched, the caller merges alone */
extern bool sift_colp_merge(Relation index, const SiftSegEntry *in, int n, int nworkers, int nslices, TimestampTz start, bool partitions);

/* The peak of the memory the build holds is noted where it grows (a batch, a run, a unit); a release library does not look */
#ifdef SIFT_TESTING
extern void sift_colb_peak_note(void);
#else
#define sift_colb_peak_note() ((void) 0)
#endif

#ifdef SIFT_TESTING
/*
 * Parallel workers the last sift_col build of this session launched; the superblock (1 = the first) whose participant fails (0 = none);
 * kB the writer of a segment holds before the segment is cut (0 = by maintenance_work_mem)
 */
extern PGDLLIMPORT int sift_test_col_workers;
extern PGDLLIMPORT int sift_test_col_build_peak_kb;	/* kB of memory (contexts of the backend) the builds of the last statement of this session held above what it started with, at their peak */
extern PGDLLIMPORT int sift_test_col_build_table_buffers;	/* the buffers of the table (any fork) the last build of this session accessed itself, not the ones PostgreSQL accesses after it */
extern void sift_colb_peak_begin(void);
extern PGDLLIMPORT bool sift_test_col_build_peak_log;	/* the largest contexts are logged each time the peak grows by a MB */
extern PGDLLIMPORT int sift_test_col_fail_sb;
extern PGDLLIMPORT int sift_test_col_segment_kb;
extern PGDLLIMPORT int sift_test_col_partition_kb;
extern PGDLLIMPORT int sift_test_col_pass_kb;
extern PGDLLIMPORT bool sift_test_col_from_index_off;	/* a REINDEX reads the heap even when the index it replaces could give the rows */
extern PGDLLIMPORT bool sift_test_col_from_index_forced;	/* a REINDEX takes the rows from the index it replaces whenever it can, whatever the chooser prices */
extern PGDLLIMPORT int sift_test_col_from_index_mutant;	/* a defect planted in the build from the replaced index (col/v1/build/from_index.c) */
extern PGDLLIMPORT int sift_test_col_from_index_used;	/* 1: the last build of this session took its rows from the index it replaced */
extern PGDLLIMPORT int sift_test_col_spill_run_kb;	/* kB of memory a run of the ranges of a build by key ranges may take (0 = the fixed cap) */
extern PGDLLIMPORT bool sift_test_col_build_runs_overlap;	/* a build with key_cut writes the runs of the table's order, as a key that is not a number does */
extern PGDLLIMPORT int sift_test_col_spill_bytes;	/* bytes the temporary files of the last build by key ranges of this session took */
extern PGDLLIMPORT int sift_test_col_spill_rows;	/* rows they hold */
extern PGDLLIMPORT int sift_test_col_build_runs;	/* the runs a key_cut build keeps before it merges some (0 = SIFT_COL_BUILD_RUNS) */
extern PGDLLIMPORT int sift_test_col_open;	/* open units (col/v1/build/open.c): -1 none, 0 as the build decides, 1 for every range of the key, no temporary files */
extern PGDLLIMPORT int sift_test_col_open_mutant;	/* a defect in the open units of a build (open.c): 1 a value joins the unit of another, 2 the rows left when the scan ends are lost, 3 a unit is written twice, 4 the cap of the open units is not looked at */
extern PGDLLIMPORT int sift_test_col_open_kb;	/* kB the open units may hold (0 = the first run's) */
extern PGDLLIMPORT int sift_test_col_open_single;	/* the values of the key that have a range of their own (-1 = as many as the memory of the open units holds) */
extern PGDLLIMPORT char sift_test_col_open_stats[256];	/* what the last build by open units did */
extern PGDLLIMPORT bool sift_test_col_no_present;	/* a build writes no presence leaf: the layout of an index made before it */
extern PGDLLIMPORT int sift_test_col_merge_slices;	/* slices the participants of the last parallel build merged in (0 = one process merged) */
extern PGDLLIMPORT int sift_test_col_scope_merges;	/* merges of scope segments this session has written */
extern PGDLLIMPORT int sift_test_col_scope_merge_limit;	/* the merge of scope segments that would be the one after this many stops with an error (0 = no limit) */
extern PGDLLIMPORT bool sift_test_col_parallel_always;	/* a parallel build with key_cut is made whatever its merge (on: the tests of the participants run on small tables; off: build_pays decides) */
extern PGDLLIMPORT int sift_test_col_merge_par_kb;	/* the input of a merge outside a build from which the workers make it, in kB (the gate MERGE_PAR_BYTES) */
extern PGDLLIMPORT int sift_test_col_merge_workers;	/* workers that merge the segments of a parallel build (-1 = those that built them, 0 = the leader alone) */
extern PGDLLIMPORT int sift_test_col_temp_free_kb;	/* the disk space the temporary files of a build by key ranges may use, in kB (-1 = what the disks of the temporary tablespaces have free) */
extern PGDLLIMPORT int sift_test_col_build_source_used;
extern PGDLLIMPORT int sift_test_col_build_spill_said;	/* 1: the NOTICE of the last build's choice said that the segment writer sorts entries on temporary files */
extern PGDLLIMPORT int sift_test_col_choose_mutant;
extern PGDLLIMPORT int sift_test_col_build_btree;	/* the build by a btree on the key (col/v1/build/btree.c): 0 the chooser decides, 1 whenever a btree can, -1 never */
extern PGDLLIMPORT int sift_test_col_btree_entries;	/* entries of the btree the last build read, tuples it found visible and heap blocks it asked for */
extern PGDLLIMPORT int sift_test_col_btree_visible;
extern PGDLLIMPORT int sift_test_col_btree_blocks;
extern PGDLLIMPORT bool sift_test_col_btree_gate_open;	/* a mutant: any btree passes for one that gives the key's order */
extern PGDLLIMPORT int sift_test_col_pick_cache_blocks;	/* heap blocks the cache of the choice holds (-1 = shared_buffers), and what the last choice walked and predicted (col/v1/build/btree_pick.c) */
extern PGDLLIMPORT int sift_test_col_pick_entries;
extern PGDLLIMPORT int sift_test_col_pick_blocks;
extern PGDLLIMPORT int sift_test_col_pick_reads;
extern PGDLLIMPORT int sift_test_col_pick_random;
extern PGDLLIMPORT int sift_test_col_pick_walk_bytes;	/* the bytes the walk of the last choice held: the simulated cache and the keys of the boundaries */
extern PGDLLIMPORT int sift_test_col_pick_spool_bytes;	/* the bytes the last choice estimated the key ranges write */
extern PGDLLIMPORT double sift_test_col_pick_random_us;	/* the costs the choice is made at, in microseconds (-1: measured) */
extern PGDLLIMPORT double sift_test_col_pick_seq_us;
extern void sift_col_btree_test_gucs(void);
extern PGDLLIMPORT int sift_test_col_gap_blocks;	/* blocks a parallel build reserves for the merge (-1 = by the statistics, 0 = none) */
#endif

#endif							/* SIFT_COL_BUILD_H */
