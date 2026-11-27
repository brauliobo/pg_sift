/*
 * test/unit/bm25/phrase_huge.c - a phrase decided by the stored positions of a doc whose term has more than 256 M occurrences (bm25/phrase.c): the
 * starts of the phrase take the huge allocation (a plain one failed with "invalid memory alloc request size") and the narrowing answers a cancel.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/phrase.c"

#define NPOS	(MaxAllocSize / sizeof(uint32) + 1)

volatile sig_atomic_t InterruptPending = false;
static int	nprocessed;

void
ProcessInterrupts(void)
{
	nprocessed++;
	InterruptPending = false;
}

static PosCtx c;
static int	leaf[3],
			order[3] = {0, 1, 2};
static PhraseCheck ph = {.nterms = 3,.leaf = leaf,.order = order};
static bool held;

static void
match(void *arg)
{
	held = phrase_match(&c, &ph);
}

/* The phrase "a a a" in a doc that is NPOS times "a" */
int
main(void)
{
	SiftPosBuf	pb = {.v = MemoryContextAllocHuge(CurrentMemoryContext, NPOS * sizeof(uint32)),.n = NPOS,.cap = NPOS};
	uint64		stamp = 1;

	for (uint32 j = 0; j < NPOS; j++)
		pb.v[j] = j;
	c = (PosCtx) {.mcxt = CurrentMemoryContext,.pb = &pb,.stamp = &stamp,.doc = 1};
	InterruptPending = true;
	SIFT_CHECKF(pgshim_try(match, NULL), "the phrase of a doc of %zu occurrences failed: %s", (size_t) NPOS, pgshim_errmsg);
	SIFT_CHECKF(held, "the phrase of the doc was not found");
	SIFT_CHECKF(nprocessed > 0, "the phrase of a doc of %zu occurrences was narrowed with a cancel pending", (size_t) NPOS);
	return sift_check_summary();
}
