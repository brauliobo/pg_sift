/*
 * col/wbuf.h - L3 of the column engine: the write buffers of a column index. The lane structure, locking,
 * sealing and WAL of bm25/wbuf.c, with row records instead of term records. Page types SIFT_PT_WBUFHEADER / SIFT_PT_WBUFDATA, the
 * SiftWbufHeaderData / SiftLaneData of core/page.h; the data page header and the record are private to col/wbuf/wbuf.c.
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
	uint64		ref_gen;		/* the write buffer whose directory entry is marked as one that holds references (SIFT_COL_FORMAT_REFBUF) */
} SiftCWbufHint;

/*
 * The leaves a write buffer holds an overflow value of (a value the leaf cannot store, SiftColValue.overflow): a bit mask in the reserved word
 * of the buffer's directory entry, bit i for leaf i and bit 31 for every leaf from 31 on. An appender sets the bits of its row before the row
 * goes in and, when that changes the entry, invalidates the plans of the table: a plan made before the value was written must not read it with
 * the scan, and the planner of a later one sees the mask (docs/COL-PLAN.md). The mask stays until the buffer is promoted, whose zones then say it.
 */
extern uint32 sift_cwbuf_overflow(const SiftDirSnapshot *dir);

/* The cached plans of the table are invalid in every session at once (a scan that met a value its plan did not know of calls it too) */
extern void sift_cwbuf_invalidate_plans(Oid heapoid);

/* The leaves a write buffer holds a JSON null of (SIFT_COL_OVF_BIT), in the sum_dl word of its directory entry, kept as the overflow mask is; only a cast of it before PostgreSQL 18 needs it */
extern uint32 sift_cwbuf_jnull(const SiftDirSnapshot *dir);

/*
 * Appends the row to the open write buffer (creating it when none; lane MyProcNumber % SIFT_LANES, other lanes when the tail is
 * busy), one generic WAL record per page touched. Returns true when the segment reached write_buffer_limit pages (the caller asks maintenance to
 * seal: sift_col_maint_after_seal). schema_hash is checked against meta->analyzer_hash when the hint is refreshed (REINDEX needed error).
 * A row whose record exceeds a page is split in pieces (SIFT_COLR_CONT). xmin: the transaction that inserted the heap tuple of the row, kept in the record
 * (InvalidTransactionId: none, as for the rows of an index without track_changes and of a speculative insertion).
 */
extern bool sift_cwbuf_append(Relation index, SiftCWbufHint *hint, const SiftColSchema *schema, const SiftColRow *row, TransactionId xmin, bool ref);

/*
 * ref (sift.col_wbuf_ref, needs an xmin): the record keeps the xmin alone, 16 bytes with its head, and the reader fetches the values from the heap tuple at the TID
 * (col/wbuf/ref.c: a tuple that is gone or is another one's is skipped). The heap is durable by its own WAL, which precedes the record of the index.
 */
extern PGDLLIMPORT bool sift_col_wbuf_ref;

/*
 * A row of a transaction that already wrote one is buffered in the backend (false: the index has no slot, append it) and reaches the lane with
 * the rest of its run, as one WAL record a page: sift_cwbuf_flush writes the rows of the index (true: one crossed write_buffer_limit, as for
 * append), sift_cwbuf_pending names the index that has the file of a slot that holds some (a swap of files moves rows to the other index), sift_cwbuf_discard drops those of an index (all when InvalidOid): the transaction
 * aborted, or the index was rebuilt from the heap that holds them. Every reader of this file flushes first.
 */
extern bool sift_cwbuf_pend(Relation index, SiftCWbufHint *hint, const SiftColSchema *schema, const SiftColRow *row, TransactionId xmin, bool ref, bool *crossed);
extern bool sift_cwbuf_flush(Relation index);
extern bool sift_cwbuf_pending(Oid *index);
extern void sift_cwbuf_discard(Oid index);

/*
 * Provided by the maintenance of the index AM, the counterpart of sift_maint_make_room: waits for the maintenance lock and merges until the
 * directory has room for the active segment; false when it cannot get any. Called without index locks held.
 */
extern bool sift_col_maint_make_room(Relation index);

/* The slots of the write buffer arena (col/wbuf/arena.c): given back when no row finds one; called after a row is written, with no lock held */
extern bool sift_cwbuf_arena_gc_wanted(void);
extern bool sift_cwbuf_arena_asked_back(Relation index);
extern void sift_cwbuf_arena_gc(void);
/* The buffer of generation gen is promoted: its arena slot is free */
extern void sift_cwbuf_arena_drop(Relation index, uint64 gen);

/* Seals the active segment (only above write_buffer_limit unless force) and opens a fresh one; returns the sealed gen or 0 (sift_wbuf_seal) */
extern uint64 sift_cwbuf_seal(Relation index, bool force);
extern uint64 sift_cwbuf_seal_vacuum(Relation index);
extern uint64 sift_cwbuf_rotate(Relation index);
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
/* How many live records of the write buffer entry come from a transaction at or after x that did not abort (or from one that says nothing of itself)? Reads the headers of the records */
extern uint64 sift_cwbuf_inserted_since(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, TransactionId x);
extern SiftColBatch **sift_cwbuf_batches(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n, TransactionId *stamp);

/*
 * What a scan was given of a kept set: the set (a serial, never reused) and the rows added to it in place since it was made. A cache of the scan that
 * is keyed by it (the order of the buffer's rows, col/scan/gorder.c) holds while both are the same; the rows of a set only grow, and a set that is dropped is
 * replaced by one with another serial
 */
typedef struct SiftCWbufToken
{
	uint64		serial;
	uint32		fills;
} SiftCWbufToken;

/*
 * The same for a scan: kept by the backend, which reads the records appended since the last scan only; not to be changed, alive until mcxt is reset or deleted.
 * dirver is the version of the directory the scan read before this call: while it is the one the kept set was checked at, no VACUUM has flagged a record since and the
 * pages of the buffer are not read to find out. stamp (may be NULL): the xid before which every transaction that inserted a row of the batches committed, now; none when
 * one has not, or its record does not say.
 */
extern SiftColBatch **sift_cwbuf_batches_shared(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, uint32 dirver, MemoryContext mcxt, int *n, SiftCWbufToken *tok,
													TransactionId *stamp);

/* The heap addresses of the live records whose plain leaf holds one of the constants of the EQ or IN predicate (an overflow value is kept for the heap to check), ascending, in mcxt; no batch is built (sift_col_wbuf_lookup). *n = the count; *rows = the live records of the leaf */
extern ItemPointerData *sift_cwbuf_lookup(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, const SiftColPred *pred, MemoryContext mcxt, uint32 *n, uint64 *rows);

/* VACUUM: calls cb for every live record and flags the ones it reports dead; returns the rows removed (maintenance lock held) */
extern int64 sift_cwbuf_bulkdelete(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, IndexBulkDeleteCallback cb,
								  void *cbstate, IndexBulkDeleteResult *stats);

/* The rows the backends of the server decoded from the buffer of generation gen to read it, since it was opened (col/wbuf/reads.c): what its promotion is weighed against (maint_wbuf_read_due) */
extern void sift_cwbuf_decoded_note(Relation index, uint64 gen, uint64 rows);
extern uint64 sift_cwbuf_decoded(Relation index, uint64 gen);
/* whether this backend decoded rows since the last call (the scan that did looks for the promotion, sift_col_vacuum_tick) */
extern bool sift_cwbuf_decoded_take(void);

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
