/*
 * build_ext_gate.h - SiftBuildExtGate: what the workers of a parallel Format 2.0 build share to place their output (spec 25 2.2).
 * Extents and part generations are reserved with one atomic add each; the file grows only through the ordered high-water gate.
 *
 * The gate keeps one rule: a worker writes blocks [start, end) only after ensure(end) returned, and the file is extended only
 * from the high-water mark under the extension lock. So no extension ever covers a block some worker already wrote.
 */
#ifndef SIFT_BUILD_EXT_GATE_H
#define SIFT_BUILD_EXT_GATE_H

#include "core/shm/port.h"

#include "storage/block.h"

#define SIFT_PAR_BUILD_EXTEND_CHUNK		1024U	/* 8 MB */
#define SIFT_PAR_BUILD_NO_SLOT			UINT32_MAX

typedef void (*SiftBuildExtendFn) (void *ctx, BlockNumber from, BlockNumber to);

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftBuildExtGate
{
	pg_atomic_uint64 next_data_blkno pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 next_part_gen pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 file_nblocks pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	SiftShmLock extend_lock;
	pg_atomic_uint32 out_part_cursor pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 overflow_flag;
	uint32		out_parts_cap;
} SiftBuildExtGate;

StaticAssertDecl(sizeof(SiftBuildExtGate) == 4 * PG_CACHE_LINE_SIZE, "SiftBuildExtGate is four cache lines");
StaticAssertDecl(offsetof(SiftBuildExtGate, next_part_gen) == PG_CACHE_LINE_SIZE, "the generation counter has its own line");
StaticAssertDecl(offsetof(SiftBuildExtGate, file_nblocks) == 2 * PG_CACHE_LINE_SIZE, "the extension gate has its own line");
StaticAssertDecl(offsetof(SiftBuildExtGate, out_part_cursor) == 3 * PG_CACHE_LINE_SIZE, "the output cursor has its own line");

extern void sift_build_ext_gate_init(SiftBuildExtGate *gate, BlockNumber first_data_blk, BlockNumber file_nblocks,
									 uint32 out_parts_cap, int tranche_id);

/* The SiftBuildExtendFn of a build that writes a new relfilenode through smgr bulk writes, and of the first pass of CREATE INDEX CONCURRENTLY through the buffer manager; ctx is the Relation */
extern void sift_build_extend_smgr(void *ctx, BlockNumber from, BlockNumber to);
extern void sift_build_extend_buffered(void *ctx, BlockNumber from, BlockNumber to);

/* The next npages blocks of the file; false when the relation cannot hold them (block numbers end at MaxBlockNumber) */
extern bool sift_build_ext_reserve(SiftBuildExtGate *gate, uint32 npages, BlockNumber *start);

/* Generations start at first_gen and are never reused */
extern uint64 sift_build_ext_next_gen(SiftBuildExtGate *gate);

/* Returns once the file holds blocks [0, end_blk): grows it from the high-water mark, in 8 MB aligned steps */
extern void sift_build_ext_ensure(SiftBuildExtGate *gate, BlockNumber end_blk, SiftBuildExtendFn extend, void *ctx);

/* The slot of the next output part descriptor, or SIFT_PAR_BUILD_NO_SLOT (and overflow_flag set) past the capacity */
extern uint32 sift_build_ext_out_slot(SiftBuildExtGate *gate);

/* After the workers: the unused tail [*from, *to) of the file the leader trims or registers as one free extent; false when none */
extern bool sift_build_ext_slack(SiftBuildExtGate *gate, BlockNumber *from, BlockNumber *to);

#endif							/* SIFT_BUILD_EXT_GATE_H */
