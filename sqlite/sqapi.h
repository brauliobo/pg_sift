/*
 * sqapi.h - the interface between the SQLite virtual table (sqvtab.c), the segment builder (sqbuild.c) and the
 * page store (sqstore.c), which stands in for sift_store.c.
 */
#ifndef SQAPI_H
#define SQAPI_H

#include "postgres.h"

#include <sqlite3ext.h>

#include "sift.h"
#include "sift_page.h"
#include "sift_store.h"
#include "sift_analyze.h"
#include "sift_posting.h"
#include "sift_segment.h"

/* rowid <-> document key: block = rowid >> SQ_ROWID_SHIFT, offset = (rowid & mask) + 1; rowids 0 .. 2^37 - 1 */
#define SQ_ROWID_SHIFT	5
#define SQ_ROWID_MAX	((int64_t) 1 << (32 + SQ_ROWID_SHIFT))

static inline SiftKey
sq_key(int64_t rowid)
{
	return ((uint64) (rowid >> SQ_ROWID_SHIFT) << 16) | (uint64) ((rowid & ((1 << SQ_ROWID_SHIFT) - 1)) + 1);
}

static inline int64_t
sq_rowid(BlockNumber blk, int off)
{
	return ((int64_t) blk << SQ_ROWID_SHIFT) | (off - 1);
}

/* Options of the virtual table, as the portable sources see an index's reloptions */
typedef struct SqOptions
{
	SiftOptions base;
	char		stemmer[16];
	char		fields[SIFT_FIELDS_BYTES];
} SqOptions;

/* ---------- sqstore.c ---------- */

extern int	sq_read_mode;			/* 0: SELECT by block, 1: sqlite3_blob_open/read */

extern SqStore *sq_store_new(sqlite3 *db, const char *schema, const char *name, const SqOptions *opts);
extern void sq_store_free(SqStore *st);
extern Relation sq_store_rel(SqStore *st);
extern const SqOptions *sq_store_options(const SqStore *st);
extern void sq_store_create_tables(SqStore *st);
extern void sq_store_drop_tables(SqStore *st);
extern void sq_store_rename(SqStore *st, const char *newname);

/* Loads (or reloads) the directory; with changed_only, only when the committed version moved. true when it did */
extern bool sq_store_refresh(SqStore *st, bool changed_only);
extern const SiftDirSnapshot *sq_store_dir(const SqStore *st);
extern uint64 sq_store_new_gen(SqStore *st);
extern BlockNumber sq_store_hw(const SqStore *st);
extern void sq_store_set_hw(SqStore *st, BlockNumber hw);
extern BlockNumber sq_wt_first(const SiftWriteTarget *wt);

/* Publishes / retires segments (rows of the manifest) and writes the dirty pages back; all inside the caller's transaction */
extern void sq_store_publish(SqStore *st, const SiftSegEntry *entry);
extern void sq_store_retire(SqStore *st, uint64 gen);
extern void sq_store_set_flags(SqStore *st, uint64 gen, uint16 flags);
extern void sq_store_flush_pages(SqStore *st);
extern void sq_store_rollback(SqStore *st);
extern void sq_store_release_garbage(SqStore *st);
extern void sq_store_invalidate(SqStore *st);

typedef struct SqStoreStats
{
	uint64		pages_read;
	uint64		pages_cached;
	uint64		pages_total;		/* rows of the page table */
	uint64		bytes_total;
} SqStoreStats;

extern void sq_store_stats(SqStore *st, SqStoreStats *out);

/* ---------- sqbuild.c ---------- */

typedef struct SqInv SqInv;

extern SqInv *sq_inv_new(bool positions);
extern void sq_inv_free(SqInv *inv);
extern bool sq_inv_empty(const SqInv *inv);
extern uint64 sq_inv_postings(const SqInv *inv);
extern void sq_inv_add(SqInv *inv, SiftKey key, const SiftTermFreq *terms, int nterms, uint32 dl);
extern bool sq_inv_remove(SqInv *inv, SiftKey key);
extern bool sq_inv_has(const SqInv *inv, SiftKey key);
extern void sq_inv_clear(SqInv *inv);

/* Writes the live content of inv as one new segment and publishes it */
extern void sq_inv_write(SqInv *inv, SqStore *st);

/* Rewrites the live part of the segments of gens[] as one new segment, retires them */
extern void sq_merge(SqStore *st, const uint64 *gens, int n, bool positions);

/* Clears the liveness bit of key in whichever segment holds it; true when it was live */
extern bool sq_delete_key(SqStore *st, SiftKey key);

#endif							/* SQAPI_H */
