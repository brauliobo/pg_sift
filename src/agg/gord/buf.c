/*
 * agg/gord/buf.c - the spare cursor buffers of the global order merge (agg/gord.c), kept under the backend cache budget.
 */

#include "postgres.h"

#include "agg/gord/int.h"
#include "core/cachebudget.h"

/*
 * The 16 KB of keys and addresses of a cursor are the largest buffers of a lookup, and a block of that size is a malloc of its own that goes back to the kernel with
 * the top of the heap when the statement ends (24 page faults for 6 segments, a fifth of the time of a lookup of 0.5 million instructions). The backend keeps the blocks
 * it has used, and a scan gets them back through the reset callback of its context.
 */
#define GBUF_KEEP	64
/* MEM[MEM-AGG-21]: backend cache of spare cursor buffers evicted by the cache budget (at most 64 times 16 KB idle) */
static MemoryContext gbuf_cx;
static SiftCGOrdBuf *gbuf_spare[GBUF_KEEP];
static int	ngbuf_spare;
static uint64 gbuf_used;

static bool
gbuf_evict_one(void)
{
	if (ngbuf_spare == 0)
		return false;
	pfree(gbuf_spare[--ngbuf_spare]);
	return true;
}

static void
gbuf_evict_all(void)
{
	while (gbuf_evict_one())
		;
}

static uint64
gbuf_oldest(void)
{
	return ngbuf_spare > 0 ? gbuf_used : UINT64_MAX;
}

static const SiftCacheKind gbuf_kind = {"agg_cursors", gbuf_oldest, gbuf_evict_one, gbuf_evict_all};

void
gbuf_put(SiftCGOrdBuf *b)
{
	static bool registered;

	if (ngbuf_spare >= GBUF_KEEP)
	{
		pfree(b);
		return;
	}
	if (!registered)
	{
		sift_cache_register(&gbuf_kind);
		registered = true;
	}
	gbuf_spare[ngbuf_spare++] = b;
	gbuf_used = sift_cache_tick();
}

SiftCGOrdBuf *
gbuf_get(void)
{
	SiftCGOrdBuf *b;

	if (ngbuf_spare > 0)
		b = gbuf_spare[--ngbuf_spare];
	else
	{
		if (gbuf_cx == NULL)
			gbuf_cx = sift_cache_create_context("sift cache cursors");
		b = MemoryContextAlloc(gbuf_cx, sizeof(SiftCGOrdBuf));
	}
	b->at = UINT32_MAX;
	b->cm = b->rows = b->kfrom = b->kn = b->afrom = b->an = 0;
	b->compact = false;
	b->stored = false;
	b->desc = false;
	return b;
}
