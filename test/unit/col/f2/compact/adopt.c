/*
 * test/unit/col/f2/compact/adopt.c - the deletes a compaction adopts under the locks of its commit (col/f2/compact/compact.c dv_delta, col/f2/compact/adopt.c): 32 Mi of them, asked for twice to tell an ambiguous address,
 * are one array past one palloc (1 GB) that a huge allocation holds, where a plain one failed the compaction with "invalid memory alloc request size"; the reads of the deletes of the
 * inputs and the search of each output answer a cancel, which the partition lock held them past (S120-22). The part, its vector and the search of its rows are stand-ins.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "col/f2/compact/compact.c"
#include "col/f2/compact/adopt.c"

#define GRANULES	64
#define GRAN_ROWS	1024
#define AMBIGUOUS	((1 << 25) + 1)

volatile sig_atomic_t InterruptPending = false;
static int	interrupts;
static int	located;

void
ProcessInterrupts(void)
{
	interrupts++;
}

void *
repalloc_extended(void *p, Size n, int flags)
{
	return (flags & MCXT_ALLOC_HUGE) ? repalloc_huge(p, n) : repalloc(p, n);
}

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return AllocSetContextCreate(parent, "scratch", ALLOCSET_SMALL_SIZES);
}

static SiftPart *
part_of(void)
{
	SiftPart   *part = palloc0(sizeof(SiftPart));

	part->hdr.ngranules = GRANULES;
	return part;
}

SiftPart *
sift_part_open(Relation rel, BlockNumber header_blk, uint64 part_gen, uint32 part_id)
{
	return part_of();
}

SiftGranuleDesc *
sift_part_granule_dir(Relation rel, const SiftPart *part)
{
	SiftGranuleDesc *dir = palloc0(GRANULES * sizeof(SiftGranuleDesc));

	for (int g = 0; g < GRANULES; g++)
		dir[g].nrows = GRAN_ROWS;
	return dir;
}

uint64 *
sift_part_read_tids(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g)
{
	uint64	   *tids = palloc(GRAN_ROWS * sizeof(uint64));

	for (int r = 0; r < GRAN_ROWS; r++)
		tids[r] = (uint64) g * GRAN_ROWS + r;
	return tids;
}

int
sift_del_locate(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const SiftPendDelRec *recs, int n, bool *done, SiftDvMark **marks, const SiftDv *base,
				const struct SiftCdRemapMap *rm)
{
	located = n;
	*marks = NULL;
	return 0;
}

/* A vector whose granules hold every row dead, the even rows of which were dead at the read */
static SiftDv *
all_dead(DeadBits *dead)
{
	SiftDv	   *now = palloc0(sizeof(SiftDv));

	*dead = palloc0(GRANULES * sizeof(uint64 *));
	now->ndirty = GRANULES;
	now->gran = palloc0(GRANULES * sizeof(SiftDvGran));
	for (int g = 0; g < GRANULES; g++)
	{
		(*dead)[g] = palloc0(GRAN_ROWS / 64 * sizeof(uint64));
		now->gran[g] = (SiftDvGran) {.granule_id = g,.ndead = GRAN_ROWS,.rows = palloc(GRAN_ROWS * sizeof(uint16))};
		for (int r = 0; r < GRAN_ROWS; r++)
		{
			now->gran[g].rows[r] = r;
			(*dead)[g][r >> 6] |= (uint64) (r % 2 == 0) << (r & 63);
		}
	}
	return now;
}

static void
delta_of_inputs(void)
{
	CompactIn	in = {.d = {.part_gen = 7}};
	SiftCompactDeletes del = {0};
	SiftDv	   *now = all_dead(&in.dead);

	interrupts = 0;
	dv_delta(NULL, 1, &in, now, NULL, &del);
	dv_delta(NULL, 1, &in, now, NULL, &del);
	SIFT_CHECKF(del.n == GRANULES * GRAN_ROWS, "two reads of %d new deletes adopt %d", GRANULES * GRAN_ROWS / 2, del.n);
	for (int i = 0; i < del.n; i++)
		SIFT_CHECKF(del.recs[i].tid % 2 == 1, "delete %d names row %llu, which was dead at the read", i, (unsigned long long) del.recs[i].tid);
	SIFT_CHECKF(interrupts >= GRANULES * GRAN_ROWS, "the reads of %d dead rows answer a cancel at each, %d times", GRANULES * GRAN_ROWS * 2, interrupts);
}

typedef struct Ambiguous
{
	SiftCompactDeletes del;
	SiftPartDesc out[3];
	bool		ambiguous;
} Ambiguous;

static void
ambiguous_huge(void *arg)
{
	Ambiguous  *a = arg;

	a->ambiguous = sift_compact_adoption_ambiguous(NULL, 1, &a->del, a->out, lengthof(a->out));
}

static void
adoption_past_one_alloc(void)
{
	Ambiguous	a = {.del = {.n = AMBIGUOUS}};

	for (int k = 0; k < lengthof(a.out); k++)
		a.out[k].dv_blk = InvalidBlockNumber;
	a.del.recs = palloc_extended((Size) AMBIGUOUS * sizeof(SiftPendDelRec), MCXT_ALLOC_HUGE);
	memset(a.del.recs, 0, (Size) AMBIGUOUS * sizeof(SiftPendDelRec));
	interrupts = 0;
	located = 0;
	SIFT_CHECKF(pgshim_try(ambiguous_huge, &a), "%d deletes asked for twice are searched for: %s", AMBIGUOUS, pgshim_errmsg);
	SIFT_CHECKF(located == 2 * AMBIGUOUS && !a.ambiguous, "the outputs are searched for %d deletes, not %d", located, 2 * AMBIGUOUS);
	SIFT_CHECKF(interrupts >= lengthof(a.out), "the search of %d outputs answers a cancel at each, %d times", (int) lengthof(a.out), interrupts);
	pfree(a.del.recs);
}

int
main(void)
{
	InterruptPending = true;
	delta_of_inputs();
	adoption_past_one_alloc();
	return sift_check_summary();
}
