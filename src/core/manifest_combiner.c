/*
 * manifest_combiner.c - SiftManifestCommitCombiner over the shared combiner (manifest_combiner.h, shm_combiner.h).
 */
#include "core/manifest_combiner.h"

typedef struct McAdapter
{
	SiftMcPublishFn fn;
	void	   *ctx;
} McAdapter;

Size
sift_mc_shmem_size(void)
{
	return sizeof(SiftManifestCommitCombiner);
}

void
sift_mc_init(SiftManifestCommitCombiner *mc, int tranche_id)
{
	Assert((uintptr_t) mc % PG_CACHE_LINE_SIZE == 0);
	memset(mc, 0, sizeof(*mc));
	sift_comb_pool_init_sharded(&mc->pool);
	for (int i = 0; i < SIFT_MC_QUEUES; i++)
		sift_comb_queue_init(&mc->queues[i], tranche_id);
}

static bool
same_relation(const SiftCombHdr *lead, const SiftCombHdr *other)
{
	return RelFileLocatorEquals(((const SiftMcSlotHdr *) lead)->locator, ((const SiftMcSlotHdr *) other)->locator);
}

static void
publish_batch(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	const McAdapter *ad = ctx;
	const SiftManifestEntryDelta *deltas[SIFT_MC_MAX_BATCH] = {NULL};
	uint64		version;

	for (uint32 i = 0; i < n; i++)
		deltas[i] = &((const SiftMcSlot *) batch[i])->delta;
	version = ad->fn(ad->ctx, deltas, n);
	for (uint32 i = 0; i < n; i++)
	{
		batch[i]->result = 0;
		batch[i]->out = version;
	}
}

static const SiftCombOps mc_ops = {.same_group = same_relation,.apply = publish_batch,.max_batch = SIFT_MC_MAX_BATCH,.gather_spins = SIFT_MC_GATHER_SPINS};

uint64
sift_mc_commit(SiftManifestCommitCombiner *mc, const RelFileLocator *rel, const SiftManifestEntryDelta *delta,
			   SiftMcPublishFn publish, void *ctx)
{
	SiftMcSlot	local;
	McAdapter	ad = {.fn = publish,.ctx = ctx};
	SiftCombArena arena = {.pool = &mc->pool,.slots = (char *) mc->slots,.stride = sizeof(SiftMcSlot)};

	memset(&local.h, 0, sizeof(local.h));
	local.h.locator = *rel;
	local.delta = *delta;
	sift_comb_submit(&arena, &mc->queues[(uint32) rel->relNumber % SIFT_MC_QUEUES], &mc_ops, &ad, &local.h.hdr,
					 sizeof(local));
	return local.h.hdr.out;
}
