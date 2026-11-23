/*
 * table/f2/f2_gcache.h - the granules a backend's lookups of rows by address have read, kept for the lookups that follow (the point cache of a sift_table).
 *
 * A granule is lazy (sift_f2_gbuf_load_lazy): its _tid and the columns a row asked for are read, once, and shared by every lookup of every statement until the cache needs the room. A
 * part is never changed once written, so a granule of (relation file, part generation, number) stays what it was; what depends on a snapshot (the patches of column deltas, the
 * pending deletes) is never kept in it. The cache is one of those of core/cachebudget.h: sift.backend_cache_kb bounds it with the others.
 */
#ifndef SIFT_TBL_F2_GCACHE_H
#define SIFT_TBL_F2_GCACHE_H

#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_tid_hash.h"
#include "table/f2/f2_row_pack.h"

#define SIFT_F2_GCACHE_MAX_GRANULES	64
#define SIFT_F2_GCACHE_MAX_FRAMES	512
#define SIFT_F2_GCACHE_MAX_BLOCKS	256

/* The lazy granule of the part pin, from the cache or read now; a reference the caller gives back with sift_f2_gbuf_unref */
extern SiftF2GranuleBuf *sift_f2_gcache_get(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g);

/* The string or the double of a row of column phys of a granule whose chunk is framed (SIFT_ENC_FRAMED), from the frame of 256 values the row lies in, which the cache keeps for the rows that follow; false when the row is null or the chunk is not framed */
extern bool sift_f2_gcache_cell_str(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, SiftStrCell *cell);
extern bool sift_f2_gcache_cell_f8(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, double *value);

/* Block blk of the row pack of the part (SEC_ROW_PACK), decompressed: from the cache or read now. It stays until the next call that may drop a cache entry: a caller copies the row it needs at once */
extern const SiftRowPackBlk *sift_f2_gcache_row_pack_block(Relation rel, SiftF2PartPin *pin, uint32 blk);

/* The granules of a relation file whose part generations start again (a TRUNCATE of a table made in the transaction): the ones a lookup holds go when it is done */
extern void sift_f2_gcache_forget(const RelFileLocator *locator);

/* The columns decoded since the granule was handed out count against the budget: whatever is not pinned and is older than it goes if the cache is over */
extern void sift_f2_gcache_charge(void);

/* Offers the rows that the address hash of a part with a row pack names for the address (the hits) to hit, part by part, until it returns true; true when it did. No granule is read, no slab is flushed and the stage of the transaction is not written: an address the stage holds is not offered */
typedef bool (*SiftF2PackedHit) (SiftF2PartPin *pin, const SiftPartitionEntry *entry, const SiftTidHashHit *hits, int nhits, void *arg);
extern bool sift_f2_locate_packed(Relation rel, SiftF2Store *store, uint64 tid, SiftF2PackedHit hit, void *arg);

/* The row at an address through the cache: its granule (a reference), its row and its partition; the micro-slabs are flushed only when the address is in no part */
extern bool sift_f2_locate_cached(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc);

#endif							/* SIFT_TBL_F2_GCACHE_H */
