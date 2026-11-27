/*
 * test/unit/col/f2/del/pend_big.c - a chain of pending deletes (col/f2/del/pend.c) whose pages hold runs of pairs decodes to 128 times its size on disk: 1,025 pages of 8 kB, the settled pairs
 * an old snapshot keeps, are 67 million records past one palloc (1 GB). The chain is read, unpaired and indexed whole, the index of the chain cache (table/f2/chain_cache.c) is reserved for
 * them, and the walks of the chain answer a cancel: none fails with "invalid memory alloc request size", which made every read of the partition fail.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "col/f2/del/pend.c"

#define PAGES			1025
#define PAIRS_PAGE		(PD_MAX_RECS / 2)
#define IDX_TIDS		((1 << 25) + 1)

volatile sig_atomic_t InterruptPending = false;
char	   *BufferBlocks;
static int	interrupts;

void
ProcessInterrupts(void)
{
	interrupts++;
}

Buffer
sift_blk_read(Relation rel, BlockNumber blk, int lockmode)
{
	return (Buffer) blk + 1;
}

void
UnlockReleaseBuffer(Buffer buf)
{
}

void
sift_page_expect(Relation rel, Page page, BlockNumber blk, SiftPageType type, uint64 gen, uint32 part_id)
{
}

bool
sift_page_sum_ok(const Page page)
{
	return true;
}

/* Page blk of a forward chain: one run of PAIRS_PAGE pairs of a deleter that no fold settles here (no xmax a horizon is asked about) */
static void
run_page(BlockNumber blk)
{
	Page		page = BufferBlocks + (Size) blk * BLCKSZ;
	SiftPendDelRec pair[2] = {{.tid = ((UINT64CONST(1) << 16) * (blk + 1)) | SIFT_PD_TID_UPDATED | SIFT_PD_TID_RUN,.xmax = InvalidTransactionId},
	{.tid = ((UINT64CONST(1) << 32) + (UINT64CONST(1) << 16) * (blk + 1)) | SIFT_PD_TID_SUCC,.xmax = PAIRS_PAGE}};

	memset(page, 0, BLCKSZ);
	PD(page)->magic = SIFT_PEND_DEL_MAGIC;
	PD(page)->next_blk = blk + 1 < PAGES ? blk + 1 : InvalidBlockNumber;
	PD(page)->nrecs = 2 * PAIRS_PAGE;
	memcpy(PD(page)->recs, pair, sizeof(pair));
	SiftPageSetUsed(page, offsetof(SiftPendDelPageData, recs) + sizeof(pair));
}

typedef struct Read
{
	SiftPendDelRec *recs;
	int			n;
	int			kept;
	SiftPendDelRec *keep;
} Read;

static void
read_unpair(void *arg)
{
	Read	   *r = arg;
	int			npages;
	BlockNumber *pages;

	r->recs = sift_pend_del_read(NULL, 1, 0, &r->n, &pages, &npages);
	pfree(pages);
	r->kept = sift_pend_del_unpair(r->recs, r->n, &r->keep, &r->n);
}

static void
index_tids(void *arg)
{
	uint64	   *tids = palloc(sizeof(uint64) * IDX_TIDS);
	SiftPendDelIndex *px;

	for (int i = 0; i < IDX_TIDS; i++)
		tids[i] = (UINT64CONST(1) << 16) + i;
	px = sift_pdel_idx_build(tids, IDX_TIDS);
	*(bool *) arg = sift_pdel_idx_has(px, tids[IDX_TIDS - 1]) && !sift_pdel_idx_has(px, tids[IDX_TIDS - 1] + 1);
	sift_pdel_idx_free(px);
	pfree(tids);
}

static void
stamp_chain(void *arg)
{
	*(SiftChainStamp *) arg = sift_pend_del_stamp(NULL, 1, 0);
}

int
main(void)
{
	Read		r = {0};
	bool		found = false;
	SiftChainStamp st;

	InterruptPending = true;
	BufferBlocks = malloc((Size) PAGES * BLCKSZ);
	for (BlockNumber b = 0; b < PAGES; b++)
		run_page(b);
	SIFT_CHECKF(pgshim_try(read_unpair, &r), "%d pages of runs, %lld records, are read and unpaired: %s", PAGES, (long long) PAGES * 2 * PAIRS_PAGE, pgshim_errmsg);
	SIFT_CHECKF(r.kept == PAGES * PAIRS_PAGE && r.n == 0, "the chain unpairs to %d deletes (%d kept), not %d", r.kept, r.n, PAGES * PAIRS_PAGE);
	SIFT_CHECKF(interrupts >= PAGES, "the read of %d pages answers a cancel at each page, %d times", PAGES, interrupts);
	pgshim_reset();
	SIFT_CHECKF(pgshim_try(index_tids, &found) && found, "%d addresses are indexed: %s", IDX_TIDS, pgshim_errmsg);
	pgshim_reset();
	interrupts = 0;
	SIFT_CHECKF(pgshim_try(stamp_chain, &st) && st.pages == PAGES, "the stamp walks the %d pages", PAGES);
	SIFT_CHECKF(interrupts >= PAGES, "the stamp of %d pages answers a cancel at each page, %d times", PAGES, interrupts);
	free(BufferBlocks);
	return sift_check_summary();
}
