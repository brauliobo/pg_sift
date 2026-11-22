/*
 * build_ext_gate.c - SiftBuildExtGate (build_ext_gate.h).
 */
#include "col/f2/build_ext_gate.h"

#include "storage/block.h"

#define FIRST_PART_GEN	1

void
sift_build_ext_gate_init(SiftBuildExtGate *gate, BlockNumber first_data_blk, BlockNumber file_nblocks,
						 uint32 out_parts_cap, int tranche_id)
{
	Assert((uintptr_t) gate % PG_CACHE_LINE_SIZE == 0);
	memset(gate, 0, sizeof(*gate));
	pg_atomic_init_u64(&gate->next_data_blkno, first_data_blk);
	pg_atomic_init_u64(&gate->next_part_gen, FIRST_PART_GEN);
	pg_atomic_init_u32(&gate->file_nblocks, file_nblocks);
	sift_shm_lock_init(&gate->extend_lock, tranche_id);
	pg_atomic_init_u32(&gate->out_part_cursor, 0);
	pg_atomic_init_u32(&gate->overflow_flag, 0);
	gate->out_parts_cap = out_parts_cap;
}

bool
sift_build_ext_reserve(SiftBuildExtGate *gate, uint32 npages, BlockNumber *start)
{
	uint64		first = pg_atomic_fetch_add_u64(&gate->next_data_blkno, npages);

	if (first + npages > (uint64) MaxBlockNumber)
		return false;
	*start = (BlockNumber) first;
	return true;
}

uint64
sift_build_ext_next_gen(SiftBuildExtGate *gate)
{
	return pg_atomic_fetch_add_u64(&gate->next_part_gen, 1);
}

static BlockNumber
extension_target(BlockNumber end_blk)
{
	uint64		aligned = TYPEALIGN64(SIFT_PAR_BUILD_EXTEND_CHUNK, (uint64) end_blk);

	return (BlockNumber) Min(aligned, (uint64) MaxBlockNumber);
}

void
sift_build_ext_ensure(SiftBuildExtGate *gate, BlockNumber end_blk, SiftBuildExtendFn extend, void *ctx)
{
	BlockNumber cur;

	if (likely(pg_atomic_read_membarrier_u32(&gate->file_nblocks) >= end_blk))
		return;
	sift_shm_lock(&gate->extend_lock);
	cur = pg_atomic_read_u32(&gate->file_nblocks);
	if (end_blk > cur)
	{
		BlockNumber target = extension_target(end_blk);

		extend(ctx, cur, target);
		pg_atomic_write_membarrier_u32(&gate->file_nblocks, target);
	}
	sift_shm_unlock(&gate->extend_lock);
}

uint32
sift_build_ext_out_slot(SiftBuildExtGate *gate)
{
	uint32		slot = pg_atomic_fetch_add_u32(&gate->out_part_cursor, 1);

	if (slot < gate->out_parts_cap)
		return slot;
	pg_atomic_write_u32(&gate->overflow_flag, 1);
	return SIFT_PAR_BUILD_NO_SLOT;
}

bool
sift_build_ext_slack(SiftBuildExtGate *gate, BlockNumber *from, BlockNumber *to)
{
	uint64		used = Min(pg_atomic_read_u64(&gate->next_data_blkno), (uint64) MaxBlockNumber);
	uint32		file = pg_atomic_read_u32(&gate->file_nblocks);

	*from = (BlockNumber) used;
	*to = file;
	return used < file;
}
