/*
 * l0_group_commit.c - SiftL0GroupCommit over the shared combiner (l0_group_commit.h, shm_combiner.h).
 */
#include "col/f2/l0/group_commit.h"

typedef struct L0Adapter
{
	SiftL0GcApplyFn fn;
	void	   *ctx;
} L0Adapter;

Size
sift_l0_gc_shmem_size(void)
{
	return sizeof(SiftL0GroupCommit);
}

void
sift_l0_gc_init(SiftL0GroupCommit *gc)
{
	Assert((uintptr_t) gc % PG_CACHE_LINE_SIZE == 0);
	memset(gc, 0, sizeof(*gc));
	sift_comb_pool_init(&gc->pool);
}

static bool
same_shard(const SiftCombHdr *lead, const SiftCombHdr *other)
{
	const SiftL0GcSlotHdr *a = (const SiftL0GcSlotHdr *) lead,
			   *b = (const SiftL0GcSlotHdr *) other;

	return RelFileLocatorEquals(a->locator, b->locator) && a->part_id == b->part_id && a->shard_id == b->shard_id &&
		a->expect_slab_blk == b->expect_slab_blk;
}

static void
apply_batch(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	const L0Adapter *ad = ctx;
	SiftL0GcSlot *items[SIFT_L0_GC_MAX_BATCH];

	for (uint32 i = 0; i < n; i++)
		items[i] = (SiftL0GcSlot *) batch[i];
	ad->fn(ad->ctx, items, n);
}

static const SiftCombOps l0_ops = {.same_group = same_shard,.apply = apply_batch,.max_batch = SIFT_L0_GC_MAX_BATCH};

uint32
sift_l0_gc_submit(SiftL0GroupCommit *gc, const SiftL0GcReq *req, SiftL0GcApplyFn apply, void *ctx)
{
	SiftL0GcSlot local;
	L0Adapter	ad = {.fn = apply,.ctx = ctx};
	SiftCombArena arena = {.pool = &gc->pool,.slots = (char *) gc->slots,.stride = sizeof(SiftL0GcSlot)};
	Size		dels_bytes = (Size) req->ndels * sizeof(SiftL0GcDel);

	Assert(req->rec_bytes + dels_bytes <= SIFT_L0_GC_MAX_BYTES && req->ndels <= SIFT_L0_GC_MAX_DELS);
	memset(&local.h, 0, sizeof(local.h));
	local.h.slab_bytes = req->rec_bytes;
	local.h.ndels = req->ndels;
	local.h.shard_id = req->shard_id;
	local.h.locator = req->locator;
	local.h.part_id = req->part_id;
	local.h.expect_slab_blk = req->expect_slab_blk;
	local.h.expect_pd_blk = req->expect_pd_blk;
	if (req->rec_bytes > 0)
		memcpy(local.payload, req->rec, req->rec_bytes);
	if (dels_bytes > 0)
		memcpy(local.payload + req->rec_bytes, req->dels, dels_bytes);
	sift_comb_submit(&arena, &sift_part_stripes()[sift_part_shard_stripe(&req->locator, req->part_id, req->shard_id & SIFT_L0_GC_SHARD_MASK)],
					 &l0_ops, &ad, &local.h.hdr, offsetof(SiftL0GcSlot, payload) + req->rec_bytes + dels_bytes);
	return local.h.hdr.result;
}

static int
cmp_stripe(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return (x > y) - (x < y);
}

uint32
sift_l0_gc_lock_shards(SiftL0GroupCommit *gc, const RelFileLocator *loc, uint64 part_id, uint32 nshards, uint32 *held)
{
	uint32		all[SIFT_PART_LOCK_STRIPES];
	uint32		n = 0;

	for (uint32 s = 0; s < nshards; s++)
		all[s] = sift_part_shard_stripe(loc, part_id, s);
	qsort(all, nshards, sizeof(uint32), cmp_stripe);
	for (uint32 i = 0; i < nshards; i++)
		if (i == 0 || all[i] != all[i - 1])
			held[n++] = all[i];
	for (uint32 i = 0; i < n; i++)
		sift_shm_lock(&sift_part_stripes()[held[i]].lock);
	return n;
}

void
sift_l0_gc_unlock_shards(SiftL0GroupCommit *gc, const uint32 *held, uint32 nheld)
{
	for (uint32 i = nheld; i > 0; i--)
		sift_shm_unlock(&sift_part_stripes()[held[i - 1]].lock);
}
