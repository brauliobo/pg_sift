/*
 * test/unit/bm25/inv_terms.c - the term bytes of the inverted buffer (bm25/inv.c) past 256 MB: one doc whose distinct terms hold more bytes than INV_MAX_BUDGET
 * keeps every term (the array of their bytes stopped growing at 256 MB and the next term was copied past its end).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"

#include "bm25/inv.c"

#define TERM_LEN	255
#define NTERMS		((int) (INV_MAX_BUDGET / TERM_LEN + 4096))

uint64
sift_seg_postings(Size budget)
{
	return budget / 8;
}

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return parent;
}

int
main(void)
{
	char	   *text = malloc(NTERMS + TERM_LEN);
	SiftTermFreq *terms = calloc(NTERMS, sizeof(SiftTermFreq));
	SiftInvBuf *b = sift_inv_create(INV_MAX_BUDGET, CurrentMemoryContext, false);
	ItemPointerData tid;
	uint64		seed = 1;

	for (int i = 0; i < NTERMS + TERM_LEN; i++)
		text[i] = 'a' + (seed = seed * 6364136223846793005ULL + 1442695040888963407ULL) % 26;
	for (int i = 0; i < NTERMS; i++)
		terms[i] = (SiftTermFreq) {.term = text + i, .len = TERM_LEN, .tf = 1, .hash = (uint32) ((uint64) (i + 1) * 0x9E3779B97F4A7C15ULL >> 32)};
	ItemPointerSet(&tid, 0, 1);
	sift_inv_add(b, &tid, terms, NTERMS, NTERMS);
	SIFT_CHECKF(b->nterms == (uint32) NTERMS, "%u terms of %d", b->nterms, NTERMS);
	SIFT_CHECKF(b->tlen <= b->tcap, "%zu bytes of terms in an array of %zu", (size_t) b->tlen, (size_t) b->tcap);
	for (int i = 0; i < NTERMS; i += 4093)
		SIFT_CHECKF(memcmp(b->tbytes + b->terms[i].term_off, text + i, TERM_LEN) == 0, "term %d lost its bytes", i);
	free(terms);
	free(text);
	return sift_check_summary();
}
