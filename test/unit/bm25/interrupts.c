/*
 * test/unit/bm25/interrupts.c - the loops over the terms of a document answer a cancel: the pass of a write buffer doc over the leaves of a plan
 * (bm25/wbind.c doc_hits, which runs a regular expression or a wildcard on every term) and the phrase found from the occurrences of its rarest term
 * (bm25/query/match.c eval_phrase_anchored) ran over a doc of millions of terms without a check.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"

#include "bm25/wbind.c"
#include "bm25/query/match.c"

#define NTOK	(PHRASE_SCAN_MAX + 8)

volatile sig_atomic_t InterruptPending = false;
static int	nprocessed;

void
ProcessInterrupts(void)
{
	nprocessed++;
	InterruptPending = false;
}

static void
check_doc_hits(void)
{
	SiftPlan	p = {0};
	WbufTab		tab = {0};
	bool		hit[1];
	DocHits		h = {.hit = hit};
	uint32		hash = 0;
	uint8		tf = 1,
				len = 1;
	SiftWbufDoc	d = {.nterms = 1,.hash = &hash,.tf = &tf,.len = &len,.terms = "a"};

	nprocessed = 0;
	InterruptPending = true;
	doc_hits(&p, &tab, &d, &h);
	SIFT_CHECKF(nprocessed > 0, "the terms of a write buffer doc were matched with a cancel pending");
}

static void
check_phrase(void)
{
	SiftToken	tok[NTOK];
	SiftDoc		d = {.tok = tok,.ntok = NTOK};
	char	   *terms[2] = {"a", "b"};
	int			lens[2] = {1, 1};
	SiftQNode	n = {.kind = SQ_PHRASE,.nterms = 2,.terms = terms,.termlens = lens};

	for (int t = 0; t < NTOK; t++)
		tok[t] = (SiftToken) {.str = t % 2 ? "b" : "a",.len = 1,.pos = t};
	nprocessed = 0;
	InterruptPending = true;
	SIFT_CHECK(eval_phrase_anchored(&d, &n));
	SIFT_CHECKF(nprocessed > 0, "a phrase was found on the tokens of a doc with a cancel pending");
}

int
main(void)
{
	check_doc_hits();
	check_phrase();
	return sift_check_summary();
}
