/*
 * col/wbuf.h - L3 of the column engine: the write buffers of a column index (docs/COL.md 2.5). The lane structure, locking,
 * sealing and WAL of bm25/wbuf.c, with row records instead of term records. Page types SIFT_PT_WBUFHEADER / SIFT_PT_WBUFDATA, the
 * SiftWbufHeaderData / SiftLaneData of core/page.h; the data page header and the record are private to col/wbuf.c.
 */
#ifndef SIFT_COL_WBUF_H
#define SIFT_COL_WBUF_H

#include "core/host.h"

#include "col/enc.h"
#include "core/store.h"

/* Per-relation state the caller keeps (the relcache entry's rd_amcache holds it for the AM): the active segment and lane tails last seen */
typedef struct SiftCWbufHint
{
	uint64		gen;			/* 0: unknown */
	BlockNumber hdr;
	BlockNumber tail[SIFT_LANES];
	uint64		ovf_gen;		/* the write buffer whose overflow leaves ovf_known describes */
	uint32		ovf_known;
	uint32		jnull_known;	/* the same for the leaves with a JSON null */
} SiftCWbufHint;

/*
 * The leaves a write buffer holds an overflow value of (a value the leaf cannot store, SiftColValue.overflow): a bit mask in the reserved word
 * of the buffer's directory entry, bit i for leaf i and bit 31 for every leaf from 31 on. An appender sets the bits of its row before the row
 * goes in and, when that changes the entry, invalidates the plans of the table: a plan made before the value was written must not read it with
 * the scan, and the planner of a later one sees the mask (docs/COL-PLAN.md). The mask stays until the buffer is promoted, whose zones then say it.
 */
extern uint32 sift_cwbuf_overflow(const SiftDirSnapshot *dir);

/* The leaves a write buffer holds a JSON null of (SIFT_COL_OVF_BIT), in the sum_dl word of its directory entry, kept as the overflow mask is; only a cast of it before PostgreSQL 18 needs it */
extern uint32 sift_cwbuf_jnull(const SiftDirSnapshot *dir);

/*
 * Appends the row to the open write buffer (creating it when none; lane MyProcNumber % SIFT_LANES, other lanes when the tail is
 * busy), one generic WAL record per page touched. Returns true when the segment reached write_buffer_limit pages (the caller asks maintenance to
 * seal: sift_col_maint_after_seal). schema_hash is checked against meta->analyzer_hash when the hint is refreshed (REINDEX needed error).
 * A row whose record exceeds a page is split in pieces (SIFT_COLR_CONT).
 */
extern bool sift_cwbuf_append(Relation index, SiftCWbufHint *hint, const SiftColSchema *schema, const SiftColRow *row);

/*
 * Provided by the maintenance of the index AM, the counterpart of sift_maint_make_room: waits for the maintenance lock and merges until the
 * directory has room for the active segment; false when it cannot get any. Called without index locks held.
 */
extern bool sift_col_maint_make_room(Relation index);

/* Seals the active segment (only above write_buffer_limit unless force) and opens a fresh one; returns the sealed gen or 0 (sift_wbuf_seal) */
extern uint64 sift_cwbuf_seal(Relation index, bool force);
extern uint64 sift_cwbuf_seal_vacuum(Relation index);
extern void sift_cwbuf_flag_sealed(Relation index, const SiftSegEntry *entry);

/* Reader: the records of a write buffer, deleted ones flagged */
typedef struct SiftCWbufReader SiftCWbufReader;
extern SiftCWbufReader *sift_cwbuf_open(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt);
/* Next record (a row split in pieces is joined); row and its strings are valid until the next call; false at the end */
extern bool sift_cwbuf_next(SiftCWbufReader *r, SiftColRow *row, bool *deleted);
extern void sift_cwbuf_close(SiftCWbufReader *r);

/*
 * All live rows of the segment, sorted by key (a repeated TID keeps its first record), as batches, one per superblock, ascending, in
 * mcxt. *n = the number of batches (0 for none).
 */
extern SiftColBatch **sift_cwbuf_batches(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n);

/* VACUUM: calls cb for every live record and flags the ones it reports dead; returns the rows removed (maintenance lock held) */
extern int64 sift_cwbuf_bulkdelete(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, IndexBulkDeleteCallback cb,
								  void *cbstate, IndexBulkDeleteResult *stats);

/* Extents (header and data pages) for release after promotion; records, rows (deleted included) and pages */
extern SiftExtent *sift_cwbuf_pages(Relation index, const SiftSegEntry *entry, int *n);
extern void sift_cwbuf_summary(Relation index, const SiftSegEntry *entry, uint64 *nrows, uint64 *npages);

/* Structural check of every page and record; ERRCODE_INDEX_CORRUPTED. Returns live rows */
extern uint64 sift_cwbuf_verify(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema);

#ifdef SIFT_TESTING
/* Test hook: the append of a row fails after this many page links (once, then 0) so that a torn row is left behind */
extern PGDLLIMPORT int sift_cwbuf_fail_grow;
#endif

#endif							/* SIFT_COL_WBUF_H */
