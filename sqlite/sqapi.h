/*
 * sqapi.h - the interface between the SQLite virtual table (sqvtab.c), the segment builder (sqbuild.c), the streaming
 * merge (sqmerge.c) and the page store (sqstore.c), which stands in for sift_store.c.
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

/* Layout version of the shadow tables (stored in <t>_config); a table of another version is refused */
#define SQ_STORE_FORMAT	2
#define SQ_VERSION		"0.2"

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

static inline int64_t
sq_key_rowid(SiftKey key)
{
	return sq_rowid(sift_key_block(key), key & 0xFFFF);
}

/* Options of the virtual table, as the portable sources see an index's reloptions */
typedef struct SqOptions
{
	SiftOptions base;
	char		stemmer[16];
	char		fields[SIFT_FIELDS_BYTES];
} SqOptions;

/* Storage geometry and cache size, fixed at CREATE (chunk) or per connection (cache) */
#define SQ_CHUNK_DEFAULT	16384
#define SQ_CACHE_DEFAULT_KIB	(64 * 1024)

/* ---------- sqstore.c ---------- */

extern SqStore *sq_store_new(sqlite3 *db, const char *schema, const char *name, const SqOptions *opts, int chunk_bytes,
							 int cache_kib);
extern void sq_store_free(SqStore *st);
extern Relation sq_store_rel(SqStore *st);
extern const SqOptions *sq_store_options(const SqStore *st);
extern int	sq_store_chunk_bytes(const SqStore *st);
extern void sq_store_create_tables(SqStore *st);
extern void sq_store_drop_tables(SqStore *st);
extern void sq_store_rename(SqStore *st, const char *newname);

/* The cache holds at most kib KiB of chunks (plus the pinned ones); changing it drops what is cached */
extern void sq_store_set_cache(SqStore *st, int cache_kib);

/* Loads (or reloads) the directory; with changed_only, only when the committed version moved. true when it did */
extern bool sq_store_refresh(SqStore *st, bool changed_only);
extern const SiftDirSnapshot *sq_store_dir(const SqStore *st);
extern uint64 sq_store_new_gen(SqStore *st);
extern BlockNumber sq_store_hw(const SqStore *st);
extern void sq_store_set_hw(SqStore *st, BlockNumber hw);
extern BlockNumber sq_wt_first(const SiftWriteTarget *wt);

/* Reads and writes the persistent settings of <t>_config */
extern int64_t sq_store_config_get(SqStore *st, const char *key, int64_t dflt);
extern void sq_store_config_set(SqStore *st, const char *key, int64_t value);

/* Publishes / retires segments (rows of the manifest) and writes the dirty pages back; all inside the caller's transaction */
extern void sq_store_publish(SqStore *st, const SiftSegEntry *entry);
extern void sq_store_retire(SqStore *st, uint64 gen);
extern void sq_store_set_flags(SqStore *st, uint64 gen, uint16 flags);
extern void sq_store_flush_pages(SqStore *st);
extern void sq_store_rollback(SqStore *st, bool final);
extern void sq_store_invalidate(SqStore *st);

/* After an ERROR inside the engine: forgets the pins its readers held, closes the blob handles */
extern void sq_store_recover(SqStore *st);
extern void sq_store_commit(SqStore *st);
extern void sq_store_hold(SqStore *st, int delta);

/* Brackets every call into the engine: names the store whose pins ReleaseBuffer drops, and checks none is left held */
extern SqStore *sq_store_enter(SqStore *st);
extern void sq_store_leave(SqStore *old);

/* Parallel scans read a fully cached store from several threads: loads every chunk of the live segments (false when they
 * do not fit the cache) and, while on, makes ReadBuffer lookup-only */
extern bool sq_store_preload(SqStore *st);
extern void sq_store_set_shared(SqStore *st, bool on);

/* Fault injection for the tests: the n-th chunk write of the store raises an error (1) or exits the process (2) */
extern void sq_store_set_fault(SqStore *st, int mode, int64_t after_chunks);

typedef struct SqStoreStats
{
	uint64		chunks_total;		/* rows of the chunk table */
	uint64		bytes_total;
	uint64		cached_bytes;
	uint64		hits;
	uint64		misses;
	uint64		evictions;
	uint32		pinned;
	uint32		slots;
	uint64		pin_leaks;
} SqStoreStats;

extern void sq_store_stats(SqStore *st, SqStoreStats *out);

/* The chunk rows of a segment: [lo, hi] and how many exist, for the integrity check */
extern void sq_store_seg_rows(const SqStore *st, const SiftSegEntry *e, int64_t *lo, int64_t *hi);
extern bool sq_store_rows_between(SqStore *st, int64_t lo, int64_t hi, int64_t *count, int64_t *bytes);
extern int64_t sq_store_rows_total(SqStore *st);

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

/* Clears the liveness bit of key in whichever segment holds it; true when it was live */
extern bool sq_delete_key(SqStore *st, SiftKey key);

/* ---------- sqmerge.c ---------- */

#define SQ_MERGE_FANIN	8				/* segments of one tier that trigger a merge */
#define SQ_MERGE_MAX_SEGMENTS 32		/* more segments than this trigger a merge of the smallest ones */

/* Streams the live part of the segments of gens[] into one new segment (bounded memory: the live documents and one
 * term at a time), publishes it and retires them in the caller's transaction. false when nothing live is left (they are
 * retired all the same) */
extern void sq_merge(SqStore *st, const uint64 *gens, int n, bool positions);

/* The segments the policy would merge now (tiers of similar size, else the smallest ones when there are too many, else
 * a segment that is mostly dead): their number, in sel. force: all segments. fanin: segments of a tier that trigger it */
extern int	sq_merge_pick(SqStore *st, bool force, int fanin, uint64 *sel);

/* Calls fn(arg, key, dl) for every live document of the segment, in key order */
extern void sq_seg_live_docs(SqStore *st, const SiftSegEntry *e, void (*fn) (void *arg, SiftKey key, uint16 dl), void *arg);
extern void sq_seg_walk_terms(SqStore *st, const SiftSegEntry *e,
								 void (*fn) (void *arg, const char *term, int len, const SiftKey *keys, uint32 n), void *arg);

#endif							/* SQAPI_H */
