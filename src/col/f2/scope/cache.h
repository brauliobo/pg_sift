/*
 * col/f2/scope/cache.h - the opened summary of a scope of an index of format major 2, kept by the backend for its next statement: the object (SEC_SCOPE_HDR) is read, checked and parsed once for
 * each generation of it, not twice for each statement. The generation names the object (a refresh writes a new one and the directory points at it), so an entry is never out of date: it
 * is only unused. Entries are held to sift.backend_cache_kb with the other caches (core/cachebudget.h); the rings stay packed until a statement reads them.
 */
#ifndef SIFT_F2_SCOPE_CACHE_H
#define SIFT_F2_SCOPE_CACHE_H

#include "postgres.h"

#include "col/f2/cache.h"
#include "col/f2/cube/meta.h"

/* The summary of scope ord that plan (the directory of the index as of now) names, NULL when there is none; valid, and held in the cache, until mcxt is reset */
extern const SiftScopeMeta *sift_f2_scope_cache_get(Relation index, const SiftPlanSnapshot *plan, int ord, MemoryContext mcxt);

/* The same for a summary the cache holds already: NULL when it does not (nothing is read, nothing is pinned: valid until the entry is dropped, so a caller copies what it needs at once) */
extern const SiftScopeMeta *sift_f2_scope_cache_peek(Relation index, const SiftPlanSnapshot *plan, int ord);

/* The settled-change slot of m, an entry of the cache, and the context the entry owns (what the slot holds is dropped with the entry) */
struct SiftF2ScopeSnap;
extern struct SiftF2ScopeSnap **sift_f2_scope_cache_overlay(const SiftScopeMeta *m, MemoryContext *entry_cx);

/* Unpacks the rings of m, an entry of the cache, for every statement that follows; false when the section is damaged */
extern bool sift_f2_scope_cache_rings(const SiftScopeMeta *m);

/* The scope ref of ord in plan, NULL when it has no active one */
extern const SiftManifestScopeRef *sift_f2_scope_cache_ref(const SiftPlanSnapshot *plan, int ord);

#ifdef SIFT_TESTING
typedef struct SiftScopeCacheStats
{
	int64		hits;
	int64		loads;			/* objects read from the pages */
	int64		private_loads;	/* read for a statement alone: the cache had no room */
	int64		ring_unpacks;
} SiftScopeCacheStats;

extern SiftScopeCacheStats sift_f2_scope_cache_stats;
#endif

#endif							/* SIFT_F2_SCOPE_CACHE_H */
