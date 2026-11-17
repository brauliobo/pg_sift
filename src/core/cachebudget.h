/*
 * core/cachebudget.h - one memory budget for the caches a backend keeps for its next statement.
 *
 * The parsed dictionaries (col/dictcache.c), the bytes of bm25 segments (bm25/segcache.c) and the rows of joined tables (agg/lookcache.c) each have a limit of their
 * own; sift.backend_cache_kb is the total they may hold together, so that a backend of a pool holds a stated amount whatever the statements it has run. A cache that
 * needs room asks sift_cache_room(): the entry that was used the longest ago in any of the caches, that nobody pins, goes first.
 */
#ifndef SIFT_CACHEBUDGET_H
#define SIFT_CACHEBUDGET_H

#include "postgres.h"

typedef struct SiftCacheKind
{
	Size		(*bytes) (void);	/* what the cache holds */
	uint64		(*oldest) (void);	/* the sift_cache_tick() of its least recently used entry nobody pins; UINT64_MAX when it has none */
	void		(*evict) (void);	/* drops that entry */
} SiftCacheKind;

extern PGDLLIMPORT int sift_backend_cache_kb;

extern void sift_cache_budget_define_gucs(void);
extern uint64 sift_cache_tick(void);
extern void sift_cache_register(const SiftCacheKind *kind);
extern bool sift_cache_room(Size need);

#endif
