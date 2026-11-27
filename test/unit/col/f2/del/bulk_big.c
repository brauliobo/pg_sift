/*
 * test/unit/col/f2/del/bulk_big.c - a bulk delete (col/f2/del/delete.c) that meets a chain of pending deletes past one palloc (67 million records, 1 GB) puts them with its own into one
 * huge array, where a plain one failed the delete with "invalid memory alloc request size" under the lock of the partition; and the search of the granules of a part answers a cancel
 * (S120-22). The manifest, the chain and the part are stand-ins.
 */

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "col/f2/del/delete.c"

#define PENDING		((1 << 26) + 1)
#define GRANULES	256
#define GROWS		64

volatile sig_atomic_t InterruptPending = false;
static int	interrupts;
static SiftPartitionEntry entry = {.pend_del_blk = 1,.l0_slab_blk = InvalidBlockNumber};
static int	merged;

void
ProcessInterrupts(void)
{
	interrupts++;
}

void
sift_mem_init(SiftMemTracker *t, const char *name, MemoryContext parent_cx, bool create_child, SiftBudgetClass cls, int nparticipants, int noperators)
{
	memset(t, 0, sizeof(*t));
	t->root_cx = parent_cx;
}

void
sift_mem_end(SiftMemTracker *t)
{
}

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return AllocSetContextCreate(parent, "scratch", ALLOCSET_SMALL_SIZES);
}

const SiftPartitionEntry *
sift_manifest_lock_part(Relation rel, uint64 *part_id, const uint8 **key_lo, SiftManifestRead *mr)
{
	return &entry;
}

SiftPendDelRec *
sift_pend_del_read_unsettled(Relation rel, uint64 part_id, BlockNumber head, BlockNumber *settled, int *n, BlockNumber **pages, int *npages)
{
	SiftPendDelRec *recs = palloc_extended((Size) PENDING * sizeof(SiftPendDelRec), MCXT_ALLOC_HUGE);

	for (int i = 0; i < PENDING; i++)
		recs[i] = (SiftPendDelRec) {.tid = i};
	*n = PENDING;
	*pages = NULL;
	*npages = 0;
	return recs;
}

int
sift_pend_del_unpair(SiftPendDelRec *recs, int n, SiftPendDelRec **keep, int *nkeep)
{
	merged = n;
	SIFT_CHECKF(recs[n - 1].tid == 7 && recs[PENDING - 1].tid == PENDING - 1, "the deletes follow the pending ones");
	ereport(ERROR, (errmsg("unpaired")));
	return n;
}

static void
bulk(void *arg)
{
	SiftPendDelRec mine = {.tid = 7};

	(void) sift_del_bulk(NULL, 1, NULL, &mine, 1, true, NULL);
}

static void
bulk_meets_long_chain(void)
{
	SIFT_CHECK(!pgshim_try(bulk, NULL));
	SIFT_CHECKF(strcmp(pgshim_errmsg, "unpaired") == 0 && merged == PENDING + 1, "a delete meets %d pending ones: %s", PENDING, pgshim_errmsg);
}

static SiftTidIndexEntry tix[GRANULES];

void *
sift_part_read_section(Relation rel, const SiftPart *part, uint16 tag, uint64 *len)
{
	for (int g = 0; g < GRANULES; g++)
		tix[g] = (SiftTidIndexEntry) {.tid_min = (uint64) g << 16,.tid_max = ((uint64) g << 16) + GROWS - 1,.blk_bloom = ~UINT64CONST(0)};
	*len = sizeof(tix);
	return tix;
}

uint64 *
sift_part_read_tids(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g)
{
	uint64	   *tids = palloc(GROWS * sizeof(uint64));

	for (int r = 0; r < GROWS; r++)
		tids[r] = ((uint64) g << 16) + r;
	return tids;
}

/* One delete for each granule of a part: each is found, and the search answers a cancel at each granule */
static void
locate_answers_cancel(void)
{
	SiftPart	part = {.hdr = {.ngranules = GRANULES}};
	SiftGranuleDesc dir[GRANULES] = {0};
	SiftPendDelRec recs[GRANULES];
	bool		done[GRANULES] = {0};
	SiftDvMark *marks;
	int			nm;

	for (int g = 0; g < GRANULES; g++)
	{
		dir[g].nrows = GROWS;
		recs[g] = (SiftPendDelRec) {.tid = ((uint64) g << 16) + g % GROWS,.xmax = 5};
	}
	interrupts = 0;
	nm = sift_del_locate(NULL, &part, dir, recs, GRANULES, done, &marks, NULL, NULL);
	SIFT_CHECKF(nm == GRANULES, "%d deletes of %d granules mark %d rows", GRANULES, GRANULES, nm);
	for (int i = 0; i < nm; i++)
		SIFT_CHECKF(marks[i].granule == (uint32) i && marks[i].row == i % GROWS && done[i], "mark %d is granule %u row %u", i, marks[i].granule, marks[i].row);
	SIFT_CHECKF(interrupts >= GRANULES, "the search of %d granules answers a cancel at each, %d times", GRANULES, interrupts);
}

int
main(void)
{
	InterruptPending = true;
	locate_answers_cancel();
	bulk_meets_long_chain();
	return sift_check_summary();
}
