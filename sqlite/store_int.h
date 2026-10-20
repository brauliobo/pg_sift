/*
 * store_int.h - what store.c (the cache and the pages), dir.c (the manifest) and stream.c (the stream reader and writer) share:
 * the state of a store and the helpers that more than one of them calls.
 */
#ifndef SIFT_STORE_INT_H
#define SIFT_STORE_INT_H

#include "api.h"

#define SQ_PAGE			BLCKSZ

typedef struct SqSlot
{
	uint32		chunk;					/* unit held */
	uint32		next;					/* hash chain: slot + 1 */
	int32		pins;
	uint8		ref;
	uint8		valid;
	uint8		dirty;					/* bit per block of the unit edited since the last write-back */
	uint8		queued;					/* in the dirty list */
} SqSlot;

typedef struct SqCache
{
	char	   *map;
	size_t		map_len;
	char	   *base;					/* unit-aligned */
	SqSlot	   *slot;
	uint32	   *bucket;
	uint32		nbucket;
	uint32	   *freelist;
	uint32		nfree;
	uint32		soft,					/* units the cache keeps */
				hard;					/* units it may hold while the rest is pinned */
	uint32		nvalid;
	uint32		hand;
	uint64		hits,
				misses,
				evictions;
} SqCache;

struct SqStore
{
	sqlite3    *db;
	char	   *schema;
	char	   *name;
	char	   *chunks_name;			/* <name>_chunks, for sqlite3_blob_open */
	SqOptions	opts;
	RelationData rel;
	int			chunk_bytes;
	uint32		unit;					/* bytes of a cache unit */
	uint32		P;						/* blocks per unit */
	uint32		R;						/* rows per unit */
	int			cache_kib;
	SqCache    *cache;
	bool		shared;					/* parallel readers: lookups only */
	uint32	   *dirty;					/* units with edited blocks */
	int			ndirty,
				dirty_cap;
	/* directory */
	SiftDirSnapshot dir;
	uint32		lversion;
	BlockNumber hw;
	uint64		next_gen;
	uint32		epoch;
	uint32		serial;
	bool		changed;				/* this transaction wrote something */
	int32		npinned;				/* pins held on any unit */
	uint64		pin_leaks;				/* calls that ended holding pins (a bug in the engine paths, counted by the tests) */
	/* statements */
	sqlite3_stmt *put,
			   *ver;
	sqlite3_blob *blob,
			   *wblob;
	int			cursors;				/* open cursors: their statements need the blob handles */
	/* what sq_store_preload last saw resident: the same cache epoch, directory and eviction count means still so */
	uint64		resident_epoch,
				resident_evictions;
	uint32		resident_version;
	/* fault injection */
	int			fault_mode;
	int64_t		fault_after;
};

#define CHECK(st, rc) \
	do { \
		int rc_ = (rc); \
		if (rc_ != SQLITE_OK && rc_ != SQLITE_ROW && rc_ != SQLITE_DONE) \
			elog(ERROR, "sift: %s", sqlite3_errmsg((st)->db)); \
	} while (0)

/* store.c */
extern SIFT_INTERNAL char *tbl(const SqStore *st, const char *suffix);
extern SIFT_INTERNAL sqlite3_stmt *prep(SqStore *st, sqlite3_stmt **slot, const char *fmt, const char *suffix);
extern SIFT_INTERNAL void set_geometry(SqStore *st, int chunk_bytes);
extern SIFT_INTERNAL bool chunk_size_ok(int c);
extern SIFT_INTERNAL void cache_free(SqCache *c);
extern SIFT_INTERNAL void cache_invalidate(SqStore *st);
extern SIFT_INTERNAL void cache_drop_unit(SqStore *st, uint32 unit);

/* dir.c */
extern SIFT_INTERNAL void persist_meta(SqStore *st, bool live);

SQLITE_EXTENSION_INIT3

static inline void
close_blobs(SqStore *st)
{
	sqlite3_blob_close(st->blob);
	sqlite3_blob_close(st->wblob);
	st->blob = st->wblob = NULL;
}

#endif							/* SIFT_STORE_INT_H */
