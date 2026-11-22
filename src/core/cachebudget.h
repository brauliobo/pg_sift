/*
 * core/cachebudget.h - the one memory budget of everything a backend keeps for its next statement.
 *
 * Every persistent per-backend cache (parsed dictionaries, bm25 segments, joined-table rows, write-buffer leaves, tombstone logs, ...) allocates in a context of its own that is
 * a child of SiftBackendCacheContext (sift_cache_create_context), and the entries of a cache that are large are a context of their own under it, so that dropping an
 * entry gives its bytes back to the context. sift.backend_cache_kb is the most that tree may allocate, as MemoryContextMemAllocated measures it, whatever the statements the
 * backend has run: a cache that needs room asks sift_cache_room() before it allocates, and the entry used the longest ago in any of the caches, that nobody pins, goes first.
 */
#ifndef SIFT_CACHEBUDGET_H
#define SIFT_CACHEBUDGET_H

#include "core/host.h"

#define SIFT_CACHE_MAX_KINDS 32

typedef struct SiftCacheKind
{
	const char *name;
	uint64		(*oldest) (void);	/* the sift_cache_tick() of its least recently used entry nobody pins; UINT64_MAX when it has none */
	bool		(*evict_one) (void);	/* drops that entry; false when none could be dropped */
	void		(*evict_all) (void);	/* drops every entry nobody pins */
} SiftCacheKind;

extern PGDLLIMPORT int sift_backend_cache_kb;

extern void sift_cache_budget_define_gucs(void);
extern MemoryContext sift_cache_root_context(void);
extern MemoryContext sift_cache_create_context(const char *name);
extern uint64 sift_cache_tick(void);

/* whether an entry last used at tick was used since the last transaction ended: a statement of this transaction may hold it, so a cache that keeps no count of its users does not evict it before the transaction does */
extern bool sift_cache_used_in_xact(uint64 tick);
extern void sift_cache_register(const SiftCacheKind *kind);

/* drops the oldest unpinned entries of any cache until need more bytes fit the limit; false when need alone passes it or what is left is pinned */
extern bool sift_cache_room(Size need);

/* after an entry is in: drops the oldest others until the tree is under the limit again (the allocator rounds up to blocks); false when it still is not, and the entry is the statement's alone */
extern bool sift_cache_commit(void);

extern Size sift_cache_total_bytes(void);

#endif
