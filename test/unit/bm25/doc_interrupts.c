/*
 * test/unit/bm25/doc_interrupts.c - the loops over the tokens of one document answer a cancel: the analysis of a document (bm25/analyze/analyze.c
 * analyze_doc, which a build and every insert run) and the grouping of its tokens by term that a recheck or a score of the document runs
 * (bm25/query/match.c doc_group) ran over a doc of up to 1 GB of text without a check.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/query/match.c"
#include "core/cpu.h"

#define NTOK	64

SiftCpu		sift_cpu;
volatile sig_atomic_t InterruptPending = false;
static int	nprocessed;

void
ProcessInterrupts(void)
{
	nprocessed++;
	InterruptPending = false;
}

static void
check_analyze_doc(void)
{
	SiftAnalyzerConfig cfg = {0};
	SiftAnalyzer *an = sift_analyzer_create(&cfg, CurrentMemoryContext);
	SiftTermFreq *terms;
	uint32		dl;

	nprocessed = 0;
	InterruptPending = true;
	SIFT_CHECK(sift_analyze_doc(an, "a b c", 5, &terms, &dl) == 3);
	SIFT_CHECKF(nprocessed > 0, "a doc was analyzed with a cancel pending");
}

static void
check_doc_group(void)
{
	SiftToken	tok[NTOK];
	SiftDoc		d = {.tok = tok,.ntok = NTOK};

	for (int t = 0; t < NTOK; t++)
		tok[t] = (SiftToken) {.str = t % 2 ? "b" : "a",.len = 1,.pos = t};
	nprocessed = 0;
	InterruptPending = true;
	doc_group(&d);
	SIFT_CHECK(d.nterms == 2 && d.tf[0] == NTOK / 2);
	SIFT_CHECKF(nprocessed > 0, "the tokens of a doc were grouped with a cancel pending");
}

int
main(void)
{
	check_analyze_doc();
	check_doc_group();
	return sift_check_summary();
}
