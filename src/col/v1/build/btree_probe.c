/*
 * col/v1/build/btree_probe.c - what a read of the heap costs on the disk it is on, measured at the start of a build
 * Whether the heap is read through a btree or the key ranges are written depends on what a random read costs against a sequential one, which is a trait of the disk
 * (a local NVMe answers in 10 to 20 microseconds, a network disk in some hundreds) and of the read stream the build reads through, not a number of the code. The probe
 * reads some hundreds of blocks of the heap at random and a stretch of them in order through a stream made as the build's (READ_STREAM_MAINTENANCE, so as many reads
 * in flight as maintenance_io_concurrency allows) and divides the time by the blocks that came from the disk, not from shared buffers. The system's cache cannot be
 * bypassed without direct I/O: a heap that it holds is read faster by the build too, and the probe sees that.
 */
#include "postgres.h"
#include "access/tableam.h"
#include "common/pg_prng.h"
#include "executor/instrument.h"
#include "storage/bufmgr.h"
#include "storage/read_stream.h"
#include "utils/rel.h"
#include "col/v1/build/int.h"

#define PROBE_BLOCKS	512			/* the blocks of each of the two reads, at most a quarter of the heap */
#define COST_MIN_US		0.5			/* the range a measurement is clamped to: a read of a page in the memory of the system, and a seek of a disk that spins */
#define COST_MAX_US		20000.0

#ifdef SIFT_TESTING
double		sift_test_col_pick_random_us = 80.0;
double		sift_test_col_pick_seq_us = 10.0;
#endif

typedef struct ProbeFeed
{
	const BlockNumber *blk;
	int			n,
				next;
} ProbeFeed;

static BlockNumber
probe_next(ReadStream *stream, void *data, void *per_buffer)
{
	ProbeFeed  *f = data;

	return f->next < f->n ? f->blk[f->next++] : InvalidBlockNumber;
}

/* Microseconds a block that came from the disk took, or 0 when none did */
static double
probe_read_us(Relation heap, const BlockNumber *blk, int n)
{
	ProbeFeed	feed = {blk, n, 0};
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	ReadStream *stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE, strategy, heap, MAIN_FORKNUM, probe_next, &feed, 0);
	int64		read_before = pgBufferUsage.shared_blks_read;
	instr_time	t0,
				t1;
	Buffer		buf;
	int			reads;

	INSTR_TIME_SET_CURRENT(t0);
	while (BufferIsValid(buf = read_stream_next_buffer(stream, NULL)))
		ReleaseBuffer(buf);
	INSTR_TIME_SET_CURRENT(t1);
	read_stream_end(stream);
	FreeAccessStrategy(strategy);
	INSTR_TIME_SUBTRACT(t1, t0);
	reads = (int) (pgBufferUsage.shared_blks_read - read_before);
	return reads > 0 ? INSTR_TIME_GET_MICROSEC(t1) / (double) reads : 0;
}

static double
clamp_us(double us)
{
	return Min(Max(us, COST_MIN_US), COST_MAX_US);
}

/* A stretch of a relation read in order, from a place of it that is not the same twice: microseconds a block that came from the disk took */
static double
seq_probe_us(Relation rel)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	int			n = (int) Min(PROBE_BLOCKS, nblocks / 4);
	BlockNumber *blk = palloc_array(BlockNumber, Max(n, 1));
	BlockNumber first = n > 0 ? pg_prng_uint64_range(&pg_global_prng_state, 0, nblocks - n) : 0;
	double		us;

	for (int i = 0; i < n; i++)
		blk[i] = first + i;
	us = probe_read_us(rel, blk, n);
	pfree(blk);
	return us;
}

static BtCosts
probe(Relation heap)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	int			n = (int) Min(PROBE_BLOCKS, nblocks / 4);
	BlockNumber *blk = palloc_array(BlockNumber, Max(n, 1));
	double		seq_us = seq_probe_us(heap),
				rand_us;
	BtCosts		c;

	for (int i = 0; i < n; i++)
		blk[i] = (BlockNumber) pg_prng_uint64_range(&pg_global_prng_state, 0, nblocks - 1);
	rand_us = probe_read_us(heap, blk, n);
	pfree(blk);
	c.seq_us = clamp_us(seq_us);
	c.random_us = Max(clamp_us(rand_us), c.seq_us);
	c.measured = true;
	return c;
}

/* What a block of a sequential read of a relation costs on its disk: the index a REINDEX replaces is read in order, and has not been read before the build */
double
colb_seq_us(Relation rel)
{
#ifdef SIFT_TESTING
	if (sift_test_col_pick_seq_us >= 0)
		return sift_test_col_pick_seq_us;
#endif
	return clamp_us(seq_probe_us(rel));
}

/* The costs the choice of the path of a build is made at */
BtCosts
colb_btree_costs(Relation heap)
{
#ifdef SIFT_TESTING
	if (sift_test_col_pick_random_us >= 0 && sift_test_col_pick_seq_us >= 0)
		return (BtCosts) {.random_us = sift_test_col_pick_random_us, .seq_us = sift_test_col_pick_seq_us, .measured = false};
#endif
	return probe(heap);
}
