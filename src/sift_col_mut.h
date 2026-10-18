/*
 * sift_col_mut.h - L3 of the column engine: the mutable segments of a column index (docs/COL.md 2.5). The lane structure, locking,
 * sealing and WAL of sift_mutable.c, with row records instead of term records. Page types SIFT_PT_MUTHEADER / SIFT_PT_MUTDATA, the
 * SiftMutHeaderData / SiftLaneData of sift_page.h; the data page header and the record are private to sift_col_mut.c.
 */
#ifndef SIFT_COL_MUT_H
#define SIFT_COL_MUT_H

#include "sift_host.h"

#include "sift_col_enc.h"
#include "sift_store.h"

/* Per-relation state the caller keeps (the relcache entry's rd_amcache holds it for the AM): the active segment and lane tails last seen */
typedef struct SiftCMutHint
{
	uint64		gen;			/* 0: unknown */
	BlockNumber hdr;
	BlockNumber tail[SIFT_LANES];
} SiftCMutHint;

/*
 * Appends the row to the active mutable segment (creating it when none; lane MyProcNumber % SIFT_LANES, other lanes when the tail is
 * busy), one generic WAL record per page touched. Returns true when the segment reached mutable_limit pages (the caller asks maintenance to
 * seal: sift_col_maint_after_seal). schema_hash is checked against meta->analyzer_hash when the hint is refreshed (REINDEX needed error).
 * A row whose record exceeds a page is split in pieces (SIFT_COLR_CONT).
 */
extern bool sift_cmut_append(Relation index, SiftCMutHint *hint, const SiftColSchema *schema, const SiftColRow *row);

/*
 * Provided by the maintenance of the index AM, the counterpart of sift_maint_make_room: waits for the maintenance lock and merges until the
 * directory has room for the active segment; false when it cannot get any. Called without index locks held.
 */
extern bool sift_col_maint_make_room(Relation index);

/* Seals the active segment (only above mutable_limit unless force) and opens a fresh one; returns the sealed gen or 0 (sift_mut_seal) */
extern uint64 sift_cmut_seal(Relation index, bool force);
extern void sift_cmut_flag_sealed(Relation index, const SiftSegEntry *entry);

/* Reader: the records of a mutable segment, deleted ones flagged */
typedef struct SiftCMutReader SiftCMutReader;
extern SiftCMutReader *sift_cmut_open(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt);
/* Next record (a row split in pieces is joined); row and its strings are valid until the next call; false at the end */
extern bool sift_cmut_next(SiftCMutReader *r, SiftColRow *row, bool *deleted);
extern void sift_cmut_close(SiftCMutReader *r);

/*
 * All live rows of the segment, sorted by key (a repeated TID keeps its first record), as batches, one per superblock, ascending, in
 * mcxt. *n = the number of batches (0 for none).
 */
extern SiftColBatch **sift_cmut_batches(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n);

/* VACUUM: calls cb for every live record and flags the ones it reports dead; returns the rows removed (maintenance lock held) */
extern int64 sift_cmut_bulkdelete(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, IndexBulkDeleteCallback cb,
								  void *cbstate, IndexBulkDeleteResult *stats);

/* Extents (header and data pages) for release after promotion; records, rows (deleted included) and pages */
extern SiftExtent *sift_cmut_pages(Relation index, const SiftSegEntry *entry, int *n);
extern void sift_cmut_summary(Relation index, const SiftSegEntry *entry, uint64 *nrows, uint64 *npages);

/* Structural check of every page and record; ERRCODE_INDEX_CORRUPTED. Returns live rows */
extern uint64 sift_cmut_verify(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema);

#ifdef SIFT_TESTING
/* Test hook: the append of a row fails after this many page links (once, then 0) so that a torn row is left behind */
extern PGDLLIMPORT int sift_cmut_fail_grow;
#endif

#endif							/* SIFT_COL_MUT_H */
