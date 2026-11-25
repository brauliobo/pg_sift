/*
 * manifest_combiner.h - SiftManifestCommitCombiner: the manifest commits of up to 16 partitions of one relation become one
 * PARTDIR rewrite, one Block-0 exclusive lock, one WAL record and one registry publish, with no optimistic retry (spec 25 1.6).
 *
 * Every writer holds its partition lock, has written its partition-private PARTLIST pages and hands in the partition's new
 * entry and the extents it releases. The batch of one relation never holds two deltas of a partition: a writer holds the
 * lock of its partition from before it builds the delta until its commit returns.
 */
#ifndef SIFT_MANIFEST_COMBINER_H
#define SIFT_MANIFEST_COMBINER_H

#include "core/shm/combiner.h"

#include "storage/block.h"
#include "storage/relfilelocator.h"

#define SIFT_MC_SLOTS			SIFT_COMB_POOL_SLOTS
#define SIFT_MC_QUEUES			64
#define SIFT_MC_MAX_BATCH		16
#define SIFT_MC_ENTRY_BYTES		192
#define SIFT_MC_MAX_RELEASE		8

typedef struct SiftManifestExtent
{
	BlockNumber start;
	uint32		nblocks;
} SiftManifestExtent;

/* The partition's new SiftPartitionEntry image (192 bytes) and the extents its commit frees */
typedef struct SiftManifestEntryDelta
{
	uint64		part_id;
	uint32		nrelease;
	uint32		pad;
	uint8		entry[SIFT_MC_ENTRY_BYTES];
	SiftManifestExtent release[SIFT_MC_MAX_RELEASE];
} SiftManifestEntryDelta;

StaticAssertDecl(sizeof(SiftManifestEntryDelta) == 16 + SIFT_MC_ENTRY_BYTES + 8 * SIFT_MC_MAX_RELEASE,
				 "SiftManifestEntryDelta must carry no padding");

typedef struct SiftMcSlotHdr
{
	SiftCombHdr hdr;
	RelFileLocator locator;
} SiftMcSlotHdr;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftMcSlot
{
	SiftMcSlotHdr h;
	SiftManifestEntryDelta delta;
} SiftMcSlot;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftManifestCommitCombiner
{
	SiftCombPool pool;
	SiftCombQueue queues[SIFT_MC_QUEUES];
	SiftMcSlot	slots[SIFT_MC_SLOTS];
} SiftManifestCommitCombiner;

StaticAssertDecl(sizeof(SiftCombQueue) == PG_CACHE_LINE_SIZE, "the per-relation queue is one cache line");
StaticAssertDecl(sizeof(SiftMcSlot) % PG_CACHE_LINE_SIZE == 0, "slots are cache-line multiples");
StaticAssertDecl(offsetof(SiftManifestCommitCombiner, queues) == PG_CACHE_LINE_SIZE, "the queues follow the pool line");

/* Under the relation's queue lock, once per batch: reads the PARTDIR tree once, applies deltas[0..n), takes Block 0 exclusively once, logs one record, publishes to the registry once; returns the new manifest_version */
typedef uint64 (*SiftMcPublishFn) (void *ctx, const SiftManifestEntryDelta *const *deltas, uint32 n);

extern Size sift_mc_shmem_size(void);
extern void sift_mc_init(SiftManifestCommitCombiner *mc, int tranche_id);

/* Returns the manifest_version the batch of this delta published */
extern uint64 sift_mc_commit(SiftManifestCommitCombiner *mc, const RelFileLocator *rel, const SiftManifestEntryDelta *delta,
							 SiftMcPublishFn publish, void *ctx);

#endif							/* SIFT_MANIFEST_COMBINER_H */
