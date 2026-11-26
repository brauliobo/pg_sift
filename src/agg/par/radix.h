/*
 * agg/par/radix.h - the parallel aggregation of the 2048-row engine in two phases, with no leader that merges: GROUP BY and
 * count(DISTINCT) scale with the workers because the merge is as parallel as the scan.
 *
 * Phase 1 (scan): every participant (the leader is 0, worker w is w + 1) claims morsels of granules, aggregates into a small local
 * table of SIFT_PAR_LOCAL_CAP slots that stays in its cache, and when it is full scatters its groups, by the radix bits of the hash
 * kept in each slot, into the SIFT_RADIX_PARTS partitions: a record per group (its state) and a record per distinct value of a
 * count(DISTINCT), the pair partitioned with its group, so that one partition holds everything of the groups it owns. A partition
 * is a list of 64 KB chunks of the shared segment; a participant stages 8 KB per partition (64 partitions of it are 512 KB) and appends a block to the chunk list,
 * once per block, with no lock: one atomic add on the tail word of the partition reserves the bytes of the block in the last chunk,
 * the participant copies the block into them and adds its length (and its records, in the high half of the same word) to the commit counter of the chunk. The reservation that crosses
 * the end of a chunk takes the next chunk of the pool and publishes it as the new tail; the reservations after it wait for that,
 * a few instructions of the one that installs (no allocation, no error). When the chunks are all taken (the budget is
 * work_mem * hash_mem_multiplier, at most SIFT_PAR_DSM_MAX) the partition is marked, and its blocks go to a file of the
 * SharedFileSet of the coordinator, p<part>_w<participant>. Every hot word of the coordinator and every partition is on a cache
 * line of its own (PG_CACHE_LINE_SIZE), so that the participants scattering to different partitions do not share a line.
 * A barrier ends the phase. Phase 2 (merge): the participants claim partitions from a shared cursor; the one that claims a partition
 * merges the records of its chunks and of its spill files into a table of its own (no other participant holds a key of it, so there
 * is no lock) and emits the finished groups. A worker emits into a queue to the leader, which drains the queues after its own
 * partitions.
 *
 * A participant that attaches after the barrier has passed has nothing to scan: the work it would claim is gone and every record
 * is written. The record layout is private to this file.
 */
#ifndef SIFT_AGG_PAR_RADIX_H
#define SIFT_AGG_PAR_RADIX_H

#include "access/parallel.h"
#include "agg/ht/dist.h"
#include "agg/ht/rec.h"
#include "core/par/morsel.h"
#include "agg/ht/ht.h"
#include "port/atomics.h"
#include "storage/barrier.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/sharedfileset.h"

#define SIFT_MIN_RADIX_PARTS	16
#define SIFT_MAX_RADIX_PARTS	128
#define SIFT_RADIX_CHUNK_SIZE	(64 * 1024)
/* the tail word of a partition with no chunk yet, and of one the pool has no chunk for (its blocks go to the files) */
#define SIFT_RADIX_NO_CHUNK		UINT32_MAX
#define SIFT_RADIX_SPILL_CHUNK	(UINT32_MAX - 1)
/* a participant stages this much per partition before it appends a block to the partition */
#define SIFT_RADIX_STAGE		8192
#define SIFT_RADIX_MQ_SIZE		(64 * 1024)
/* the queue of a worker when there are this many workers: the leader reads them in turn, and a full queue stalls its worker */
#define SIFT_RADIX_MQ_WIDE_WORKERS	8
#define SIFT_RADIX_MQ_WIDE_SIZE	(256 * 1024)
/* a participant takes this many chunks of the pool at a time when the pool is at least this many times its participants, and one otherwise */
#define SIFT_RADIX_CHUNK_BATCH	4
#define SIFT_RADIX_BATCH_POOL	32
/* slots of the local table of a participant: 4096 * 64 bytes = 256 KB */
#define SIFT_PAR_LOCAL_CAP		4096
/* the local table is also emptied when its count(DISTINCT) sets hold this many values */
#define SIFT_PAR_LOCAL_DIST_MAX	65536
/* the most shared memory the partitions take, whatever work_mem says */
#define SIFT_PAR_DSM_MAX		((size_t) 256 * 1024 * 1024)

/*
 * One partition: the chunks of the records of its groups. tail is (chunk << 32) | bytes reserved in the chunk; the bytes reserved
 * pass SIFT_RADIX_PAYLOAD when a reservation crossed the end, and the chunk that reservation installs replaces the tail word.
 */
typedef struct SiftRadixPart
{
	pg_atomic_uint64 tail;
	pg_atomic_uint64 est_rows;	/* the records pushed, added once by each participant when its scan is done: an upper bound of the groups */
	pg_atomic_uint32 spilled;	/* a participant wrote a block of this partition to its file */
	uint32		first_chunk;	/* set once, by the reservation that installs the first chunk */
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftRadixPart;

/* The header of a chunk, on a line of its own: the committers of the blocks and the writers of the payload do not share a line */
typedef struct SiftRadixChunk
{
	uint32		next_chunk;		/* set once, by the reservation that installs the next chunk */
	pg_atomic_uint64 commit;	/* (the records committed << 32) | the payload bytes committed: when every participant is done, the bytes of the records */
	/* the records follow */
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftRadixChunk;

#define SIFT_RADIX_PAYLOAD		(SIFT_RADIX_CHUNK_SIZE - sizeof(SiftRadixChunk))
#define SIFT_RADIX_COMMIT(n_items, len)	(((uint64) (n_items) << 32) | (len))
#define SIFT_RADIX_COMMITTED(c)	((uint32) (c))

StaticAssertDecl(sizeof(SiftRadixPart) == PG_CACHE_LINE_SIZE, "a radix partition is one cache line");
StaticAssertDecl(sizeof(SiftRadixChunk) == PG_CACHE_LINE_SIZE, "the header of a radix chunk is one cache line");

/* The hot words of the coordinator are each on a cache line */
typedef struct SiftParCoord
{
	uint32		n_parts;
	uint8		radix_bits;
	uint32		max_chunks;
	uint32		local_cap;
	uint32		rescan_gen;		/* the generation of the spill files: a rescan of the node never meets the files of the scan before it */
	uint32		n_participants;
	Size		mq_size;		/* the bytes of the queue of a worker */
	Size		chunk_pool_off;
	Size		mq_off;
	Size		morsel_off;
	pg_atomic_uint32 next_part pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 next_free_chunk pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	Barrier		phase_barrier pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 total_rows_scanned pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 total_groups_out pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	SharedFileSet spill_files pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	SiftRadixPart parts[SIFT_MAX_RADIX_PARTS];
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftParCoord;

StaticAssertDecl(offsetof(SiftParCoord, next_free_chunk) - offsetof(SiftParCoord, next_part) >= PG_CACHE_LINE_SIZE &&
				 offsetof(SiftParCoord, phase_barrier) - offsetof(SiftParCoord, next_free_chunk) >= PG_CACHE_LINE_SIZE &&
				 offsetof(SiftParCoord, total_rows_scanned) - offsetof(SiftParCoord, phase_barrier) >= PG_CACHE_LINE_SIZE &&
				 offsetof(SiftParCoord, total_groups_out) - offsetof(SiftParCoord, total_rows_scanned) >= PG_CACHE_LINE_SIZE &&
				 offsetof(SiftParCoord, parts) % PG_CACHE_LINE_SIZE == 0,
				 "the hot words of the coordinator are on lines of their own");

/* The shared memory of a coordinator for n_workers workers and max_chunks chunks, and the number of chunks the memory budget allows */
extern Size sift_par_coord_size(uint32 max_chunks, int n_workers);
extern uint32 sift_par_budget_chunks(void);
/*
 * Initializes the coordinator in the shared memory of seg, at raw (sift_par_coord_size() bytes, any alignment), and returns it: the
 * coordinator is on the first cache line boundary of raw, and it is the one the workers are given. The leader is the receiver of
 * the queues. local_cap: the slots of a local table.
 */
extern SiftParCoord *sift_par_coord_init(void *raw, dsm_segment *seg, uint32 max_chunks, int n_workers, uint32 total_granules,
										 uint32 local_cap);
/*
 * The coordinator of a node that emits its finished groups itself, through the Gather above it, and has no queue: n_parts_radix partitions (a power of 2 in [SIFT_MIN_RADIX_PARTS, SIFT_MAX_RADIX_PARTS])
 * and max_chunks chunks. The records are the caller's: len in the first 4 bytes, the hash that picks the partition at offset 8, len a multiple of 8 (a record over a chunk goes to the files of the participant).
 */
extern Size sift_par_coord_size_custom(uint32 max_chunks);
extern SiftParCoord *sift_par_coord_init_custom(void *raw, dsm_segment *seg, uint32 n_parts_radix, uint32 max_chunks, int n_workers, uint32 local_cap);
/* The coordinator as it was after the init, for a rescan: the partitions and the pool empty, the barrier fresh, the files of the scan before it gone */
extern void sift_par_coord_reset(SiftParCoord *c);
/* A participant wrote a block of some partition to its file: the pool of chunks was too small for the records */
extern bool sift_par_coord_spilled(SiftParCoord *c);
extern uint32 sift_par_coord_parts_spilled(SiftParCoord *c);
/* A worker joins: the file set of the spills and the queue to the leader */
extern shm_mq_handle *sift_par_worker_attach(SiftParCoord *c, dsm_segment *seg);
/* The same for a coordinator made by sift_par_coord_init_custom: the file set only */
extern void sift_par_worker_attach_custom(SiftParCoord *c, dsm_segment *seg);
/* The leader's handles of the queues of the workers launched, once they are */
extern shm_mq_handle **sift_par_leader_queues(SiftParCoord *c, dsm_segment *seg, ParallelContext *pcxt);

/* The participant id: the leader 0, worker w w + 1 */
extern int	sift_par_participant(void);

/* Phase 1: the next morsel of granules [*first, *first + *n) of the flat list of the scan (core/par/morsel.h), false when there is none */
extern bool sift_par_next_morsel(SiftParCoord *c, uint32 *first, uint32 *n);

/* Joins the barrier; returns the phase: 0 when there is a scan to do */
extern int	sift_radix_join(SiftParCoord *c);
/* Ends the participant's scan: waits for every other participant's */
extern void sift_radix_scan_done(SiftParCoord *c);
extern void sift_radix_leave(SiftParCoord *c);

/* The local table of a participant in Phase 1 */
typedef struct SiftRadixLocal SiftRadixLocal;

extern SiftRadixLocal *sift_radix_local_new(SiftParCoord *c, const SiftRadixShape *shape);
/* The stages of a participant with no table of its own: the caller keeps its groups and puts their records */
extern SiftRadixLocal *sift_radix_local_new_raw(SiftParCoord *c);
extern void sift_radix_put(SiftRadixLocal *l, uint64 hash, const void *rec, uint32 len);
/* The room for a record of len bytes in the stage of the partition of hash, counted as a record put: the caller fills it at once, before its next call here. NULL when a stage is smaller than len (sift_radix_put takes the record) */
extern uint8 *sift_radix_stage_slot(SiftRadixLocal *l, uint64 hash, uint32 len);
/* The slot of an inline key (found = false: a new group, in its init state): valid until the next call, which may empty the table into the partitions */
extern SiftAggSlot64 *sift_radix_local_upsert(SiftRadixLocal *l, uint64 hash, const SiftInlineKey16 *key, bool *found);
extern SiftAggSlot64 *sift_radix_local_upsert_wide(SiftRadixLocal *l, uint64 hash, const void *key, uint32 len, uint64 nulls,
												   bool *found);
extern uint8 *sift_radix_local_ext(SiftRadixLocal *l, SiftAggSlot64 *slot);
/* Adds a value to the distinct set of the group (the shape has one): the value itself when it is 64 bits (never the hash of a wider one), else its bytes */
extern void sift_radix_local_dist_add(SiftRadixLocal *l, SiftAggSlot64 *slot, uint64 v);
extern void sift_radix_local_dist_add_bytes(SiftRadixLocal *l, SiftAggSlot64 *slot, const void *bytes, uint32 len);
/* Empties the table and the stages into the partitions and closes the spill files; the local table is freed */
extern void sift_radix_local_finish(SiftRadixLocal *l);

/* Phase 2: claims the next partition, false when none is left */
extern bool sift_par_next_part(SiftParCoord *c, uint32 *part);
/* Calls fn for each block of whole records of the partition (the chunks, then the files of the participants, each deleted once read, so a partition is scanned once): the records of a block are at its offsets of their lengths */
typedef void (*SiftRadixBlockFn) (void *arg, const uint8 *block, uint32 len);
extern void sift_radix_scan_part(SiftParCoord *c, uint32 part, SiftRadixBlockFn fn, void *arg);

/* The output of a participant: a record at a time, to the consumer (the leader) or to its queue (a worker) */
typedef void (*SiftRadixConsume) (void *arg, const void *rec, uint32 len);
typedef struct SiftRadixSink SiftRadixSink;

extern SiftRadixSink *sift_radix_sink_new(shm_mq_handle *mqh, SiftRadixConsume consume, void *arg);
extern void sift_radix_sink_put(SiftRadixSink *s, const void *rec, uint32 len);
extern void sift_radix_sink_finish(SiftRadixSink *s);
/* The leader: reads the queues to their ends, a record at a time to consume. It takes what each has in turn and sleeps only when none has: a slow worker does not hold the others */
extern void sift_radix_drain(shm_mq_handle **mqh, int n, SiftRadixConsume consume, void *arg);

#endif							/* SIFT_AGG_PAR_RADIX_H */
