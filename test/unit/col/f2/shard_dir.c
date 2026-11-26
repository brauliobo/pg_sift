/*
 * test/unit/col/f2/shard_dir.c - the errors of the hot micro-slab directory (col/f2/l0/shard_dir.c) over pages in memory, where a record that is done is written and one that is not
 * leaves nothing: an allocator that fails in the middle of a batch leaves no request placed, so that the retry the combiner gives every request of it places each once; a commit of
 * the anchor that fails leaves the tails of the directory open, as the writers that go on into it append to them.
 */
#define SIFT_S49_STANDALONE 1
#define SIFT_HREF_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include <setjmp.h>

#include "test/common/bench.h"

#include "col/f2/l0/shard_dir.c"

enum { NPAGES = 256, NSHARDS = 2, NITEMS = 16, REC_BYTES = 3000 };

typedef struct MemIo
{
	PGAlignedBlock page[NPAGES];
	PGAlignedBlock work[SIFT_L0_REC_PAGES];
	BlockNumber wblk[SIFT_L0_REC_PAGES];
	int			nwork;
	BlockNumber next;
	int			allocs_left;	/* -1: no failure */
} MemIo;

static MemIo mem;
static sigjmp_buf fail_jmp;
static SiftL0GcSlot items[NITEMS];

void
sift_page_seal(Page page)
{
}

FullTransactionId
sift_l0_href_fxid(TransactionId xid)
{
	return FullTransactionIdFromEpochAndXid(0, xid);
}

FullTransactionId
ReadNextFullTransactionId(void)
{
	return FullTransactionIdFromEpochAndXid(0, 1000);
}

uint32
sift_l0_gc_lock_shards(SiftL0GroupCommit *gc, const RelFileLocator *loc, uint64 part_id, uint32 nshards, uint32 *held)
{
	return 0;
}

void
sift_l0_gc_unlock_shards(SiftL0GroupCommit *gc, const uint32 *held, uint32 nheld)
{
}

static void
host_init(char *pg)
{
	PageHeader	ph = (PageHeader) pg;

	memset(pg, 0, BLCKSZ);
	ph->pd_special = ph->pd_upper = BLCKSZ - MAXALIGN(sizeof(SiftPageOpaqueData));
	ph->pd_lower = SIFT_PAGE_DATA_OFFSET;
}

static BlockNumber
m_alloc(void *ctx)
{
	if (mem.allocs_left == 0)
		siglongjmp(fail_jmp, 1);
	if (mem.allocs_left > 0)
		mem.allocs_left--;
	return mem.next++;
}

static void
m_release(void *ctx, BlockNumber blk)
{
}

static void
m_begin(void *ctx)
{
	mem.nwork = 0;
}

static char *
m_page(void *ctx, BlockNumber blk, SiftPageType type, bool full)
{
	for (int i = 0; i < mem.nwork; i++)
		if (mem.wblk[i] == blk)
			return mem.work[i].data;
	mem.wblk[mem.nwork] = blk;
	if (full)
		host_init(mem.work[mem.nwork].data);
	else
		memcpy(mem.work[mem.nwork].data, mem.page[blk].data, BLCKSZ);
	return mem.work[mem.nwork++].data;
}

static void
m_done(void *ctx)
{
	for (int i = 0; i < mem.nwork; i++)
		memcpy(mem.page[mem.wblk[i]].data, mem.work[i].data, BLCKSZ);
	mem.nwork = 0;
}

static void
m_abort(void *ctx)
{
	mem.nwork = 0;
}

static const char *
m_read(void *ctx, BlockNumber blk, SiftPageType type)
{
	return mem.page[blk].data;
}

static void
m_unread(void *ctx, BlockNumber blk)
{
}

static const SiftL0PageIO io = {.alloc = m_alloc,.release = m_release,.record_begin = m_begin,.record_page = m_page,.record_done = m_done,.record_abort = m_abort,
.read_lock = m_read,.read_unlock = m_unread};

static void
count_rows(void *ctx, const SiftL0BatchHeader *rec)
{
	*(uint32 *) ctx += rec->nrows;
}

static uint32
rows_of(BlockNumber anchor)
{
	uint32		rows = 0;
	SiftL0ScanVisitor v = {.batch = count_rows,.ctx = &rows};

	sift_l0_scan(&io, anchor, &v, true);
	return rows;
}

static BlockNumber
fresh_dir(SiftL0ShardShm *shm)
{
	BlockNumber dir;

	memset(&mem, 0, sizeof(mem));
	mem.allocs_left = -1;
	memset(shm, 0, sizeof(*shm));
	dir = sift_l0_dir_create(&io, NSHARDS, NO_BLOCK, 1, 0, FullTransactionIdFromEpochAndXid(0, 3));
	sift_l0_shm_load(&io, shm, dir);
	return dir;
}

static void
make_items(BlockNumber dir)
{
	for (int i = 0; i < NITEMS; i++)
	{
		SiftL0BatchHeader *rec = (SiftL0BatchHeader *) items[i].payload;

		memset(&items[i].h, 0, sizeof(items[i].h));
		items[i].h.slab_bytes = REC_BYTES;
		items[i].h.expect_slab_blk = dir;
		memset(rec, 0, REC_BYTES);
		*rec = (SiftL0BatchHeader) {.nrows = 1,.bytes = REC_BYTES,.xmin = 100 + i};
	}
}

static SiftL0GcSlot *const batch[NITEMS] = {&items[0], &items[1], &items[2], &items[3], &items[4], &items[5], &items[6], &items[7], &items[8], &items[9], &items[10],
&items[11], &items[12], &items[13], &items[14], &items[15]};

static void
test_failed_allocation_places_nothing(void)
{
	SiftL0ShardShm shm;
	BlockNumber dir = fresh_dir(&shm);
	SiftL0ShardCtx c = {.io = &io,.shm = &shm};
	bool		failed = false;

	make_items(dir);
	mem.allocs_left = 2;
	if (sigsetjmp(fail_jmp, 0) == 0)
		sift_l0_shard_apply(&c, batch, NITEMS);
	else
		failed = true;
	SIFT_CHECKF(failed, "the allocator fails in the middle of the batch");
	SIFT_CHECKF(rows_of(dir) == 0, "a batch whose allocator failed placed no request (%u rows)", rows_of(dir));
	mem.allocs_left = -1;
	sift_l0_shard_apply(&c, batch, NITEMS);
	SIFT_CHECKF(rows_of(dir) == NITEMS, "the retry of every request places each once (%u rows of %d)", rows_of(dir), NITEMS);
}

static void
fail_anchor(void *ctx, BlockNumber anchor)
{
	siglongjmp(fail_jmp, 1);
}

static void
test_failed_anchor_leaves_tails_open(void)
{
	SiftL0ShardShm shm;
	BlockNumber dir = fresh_dir(&shm);
	SiftL0ShardDirData d = sift_l0_dir_read(&io, dir);
	SiftL0Flush f = {.io = &io,.shm = &shm,.floor = FullTransactionIdFromEpochAndXid(0, 3)};
	bool		failed = false;

	if (sigsetjmp(fail_jmp, 0) == 0)
		sift_l0_flush_seal(&f, fail_anchor, NULL);
	else
		failed = true;
	SIFT_CHECKF(failed, "the commit of the new anchor fails");
	SIFT_CHECKF(pg_atomic_read_u32(&shm.dir_blk) == dir, "the writers still go to the directory");
	for (uint32 s = 0; s < NSHARDS; s++)
		SIFT_CHECKF((SIFT_L0_SL(mem.page[d.slab_tail[s]].data)->flags & SIFT_L0_SLAB_NOCRC_OPEN) != 0, "the tail of shard %u takes appends", s);
}

int
main(void)
{
	test_failed_allocation_places_nothing();
	test_failed_anchor_leaves_tails_open();
	return sift_check_summary();
}
