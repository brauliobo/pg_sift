/*
 * test/unit/col/granule/part/codecs.c - a string as long as a value can be (1 GB) among short ones: the codecs a cold part trains for the column (col/granule/part/codecs.c) and the trial of a
 * prefixed integer (col/granule/promo_prefix.c) sample its head, and no buffer of theirs asks for more than one allocation holds (S119-09). The long string is a reservation of address space that
 * is never written, so only what a sample reads of it is ever mapped.
 */
#include "postgres.h"

#include <sys/mman.h>

#include "col/granule/int.h"
#include "col/granule/part/int.h"
#include "test/common/check.h"
#include "test/common/pgshim.h"

#define NROWS		(2 * SIFT_GRANULE_ROWS)
#define WIDE_AT		8				/* the second string the even sample of a dictionary takes, after one short string */
#define WIDE_LEN	((uint32) (MaxAllocSize - VARHDRSZ))

static const char *strs[NROWS];
static uint32 lens[NROWS];
static char short_text[NROWS][16];

volatile sig_atomic_t InterruptPending = false;

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return parent;
}

void
MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
}

static SiftColIn
column(const char *wide)
{
	for (int i = 0; i < NROWS; i++)
	{
		lens[i] = (uint32) snprintf(short_text[i], sizeof(short_text[i]), "n-%d", i * 7919 % 100003);
		strs[i] = short_text[i];
	}
	strs[WIDE_AT] = wide;
	lens[WIDE_AT] = WIDE_LEN;
	return (SiftColIn) {.kind = SIFT_CELL_STR,.s = strs,.slen = lens,.hint = SIFT_CHINT_AUTO};
}

static void
train(void *arg)
{
	SiftColIn	col = column(arg);
	SiftRows	rows = {.nrows = NROWS,.ncols = 1,.cols = &col};
	SiftPartCodecs *pc = sift_part_codecs_train(&rows, SIFT_TIER_COLD_L2, NULL, NULL, NULL);

	SIFT_CHECK(pc->ncols == 1);
}

static void
prefix_trial(void *arg)
{
	SiftColIn	col = column(arg);
	SiftColPromo promo;

	(void) sift_promo_prefix_infer(&col, NROWS, NULL, &promo);
}

int
main(void)
{
	char	   *wide = mmap(NULL, WIDE_LEN, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

	SIFT_CHECK(wide != MAP_FAILED);
	SIFT_CHECKF(pgshim_try(train, wide), "the codecs of a cold part fail on a string of %u bytes: %s", WIDE_LEN, pgshim_errmsg);
	pgshim_reset();
	SIFT_CHECKF(pgshim_try(prefix_trial, wide), "the trial of a prefixed integer fails on a string of %u bytes: %s", WIDE_LEN, pgshim_errmsg);
	pgshim_reset();
	munmap(wide, WIDE_LEN);
	return sift_check_summary();
}
