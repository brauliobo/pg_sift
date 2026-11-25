/*
 * agg/v6/par/dsm.c - the shared block of the parallel SiftAggV6: its layout in the DSM chunk of the node, and (PostgreSQL only) the five callbacks of a parallel custom scan that make it, attach to it,
 * reset it for a rescan and read its counters back before the segment is unmapped.
 *
 * The block is one allocation with offsets and no pointer (v6_par.h): the header with the barrier and the cache-line-isolated words, the parts of the scan, the staged deletes of the transaction, the morsel
 * queue, the coordinator of the partitions, a slot for the state each participant leaves (the distinct strategy) and the counters of every participant. The chunk the executor gives is aligned
 * to less than the two cache lines the morsel queue asks for, so the block is at the first such boundary of it; the size has the alignment to spare.
 */
#include "core/host.h"

#include "agg/v6/par/par.h"

#define BLOCK_ALIGN	SIFT_PAR_SHM_ALIGN
#define LINE(n)		TYPEALIGN(BLOCK_ALIGN, (n))

uint32
sift_v6_par_radix_parts(SiftV6ParStrategy strategy, int nworkers)
{
	if (strategy == SV6P_RADIX_UNGROUPED_DIST)
		return SIFT_PAR_DIST_PARTS;
	return nworkers >= SIFT_PAR_WIDE_WORKERS ? SIFT_PAR_RADIX_PARTS_WIDE : SIFT_PAR_RADIX_PARTS;
}

/* The chunks of the partitions for the bytes the run is expected to hand over (v6_par_node.c, at most work_mem * hash_mem_multiplier); the two-stage strategy holds states, not groups, so it takes a half, at most SIFT_PAR_RING_MAX_BYTES */
uint32
sift_v6_par_chunks(SiftV6ParStrategy strategy, double expected_bytes)
{
	if (strategy == SV6P_TWO_STAGE)
		return (uint32) Max(SIFT_PAR_RADIX_PARTS, Min(expected_bytes / 2, (double) SIFT_PAR_RING_MAX_BYTES) / SIFT_RADIX_CHUNK_SIZE);
	/* MEM[MEM-AGG-18]: parallel DSM chunk pool (the expected bytes, at most SIFT_PAR_DSM_MAX=256MiB, in SIFT_RADIX_CHUNK_SIZE chunks: a pool short of them sends the partitions to temporary files) */
	return (uint32) Max(16, Min(4096, Min(expected_bytes, (double) SIFT_PAR_DSM_MAX) / SIFT_RADIX_CHUNK_SIZE));
}

static void
lay_out(const SiftV6ParShape *sh, SiftV6ParDsmHdr *h)
{
	const Size	participants = (Size) sh->nworkers + 1;
	Size		at = LINE(sizeof(SiftV6ParDsmHdr));

	h->parts_off = at;
	at = LINE(at + mul_size(sh->n_parts, sh->part_ref_bytes));
	h->staged_dead_off = at;
	at = LINE(at + mul_size((Size) sh->n_children + 1 + sh->n_staged_dead, sizeof(uint64)));
	h->morsel_off = at;
	at = LINE(at + sift_morsel_size(sh->n_parts));
	h->coord_off = at;
	at = LINE(at + sift_par_coord_size_custom(sh->max_chunks));
	h->plain_off = at;
	at = LINE(at + mul_size(participants, sh->plain_slot_bytes));
	h->stats_off = at;
	h->total_bytes = LINE(at + mul_size(participants, sizeof(SiftV6ParWorkerStats)));
}

Size
sift_v6_par_dsm_size(const SiftV6ParShape *sh)
{
	SiftV6ParDsmHdr h;

	lay_out(sh, &h);
	return h.total_bytes + BLOCK_ALIGN;
}

static void
init_shared_words(SiftV6ParDsmHdr *h)
{
	BarrierInit(&h->phase_barrier, 0);
	pg_atomic_init_u32(&h->next_pid, 0);
	pg_atomic_init_u32(&h->keeper, 0);
	pg_atomic_init_u32(&h->final_emitter, 0);
	pg_atomic_init_u32(&h->escalated, 0);
	pg_atomic_init_u64(&h->ring.total_recs, 0);
	pg_atomic_init_u64(&h->ring.total_bytes, 0);
	for (int i = 0; i < SIFT_PAR_MAX_DIST; i++)
		pg_atomic_init_u64(&h->dist_total[i], 0);
}

/* The block a worker finds in the chunk the leader initialized */
SiftV6ParDsmHdr *
sift_v6_par_dsm_block(void *chunk)
{
	return (SiftV6ParDsmHdr *) LINE(chunk);
}

SiftV6ParDsmHdr *
sift_v6_par_dsm_init(void *raw, dsm_segment *seg, const SiftV6ParShape *sh, const uint32 *granules)
{
	SiftV6ParDsmHdr *h = sift_v6_par_dsm_block(raw);

	Assert(sh->strategy != SV6P_SERIAL && sh->n_dist <= SIFT_PAR_MAX_DIST);
	memset(h, 0, sizeof(*h));
	lay_out(sh, h);
	memset((char *) h + h->plain_off, 0, h->total_bytes - h->plain_off);
	h->magic = SIFT_V6_PAR_DSM_MAGIC;
	h->strategy = (uint16) sh->strategy;
	h->nworkers_planned = (uint16) sh->nworkers;
	h->n_parts = sh->n_parts;
	h->n_staged_dead = sh->n_staged_dead;
	h->n_children = sh->n_children;
	h->n_aggs = sh->n_aggs;
	h->n_dist = sh->n_dist;
	h->radix_nparts = sh->radix_nparts;
	h->radix_max_chunks = sh->max_chunks;
	h->plain_slot_bytes = sh->plain_slot_bytes;
	h->seg_handle = dsm_segment_handle(seg);
	h->ring.max_chunks = sh->max_chunks;
	init_shared_words(h);
	sift_morsel_init(sift_v6_par_morsels(h), sh->n_parts, granules, (uint32) sh->nworkers + 1);
	sift_par_coord_init_custom((char *) h + h->coord_off, seg, sh->radix_nparts, sh->max_chunks, sh->nworkers, SIFT_PAR_LOCAL_GROUPS);
	return h;
}

/* A rescan: the scan starts again from the same parts. The files of the scan before it are gone and the new ones carry the next generation in their names. */
void
sift_v6_par_dsm_reinit(SiftV6ParDsmHdr *h, const uint32 *granules)
{
	memset((char *) h + h->plain_off, 0, h->total_bytes - h->plain_off);
	init_shared_words(h);
	sift_morsel_init(sift_v6_par_morsels(h), h->n_parts, granules, (uint32) h->nworkers_planned + 1);
	sift_par_coord_reset(sift_v6_par_coord(h));
	h->rescan_gen = sift_v6_par_coord(h)->rescan_gen;
}
