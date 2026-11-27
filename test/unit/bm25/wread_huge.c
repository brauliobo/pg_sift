/*
 * test/unit/bm25/wread_huge.c - the terms of a write buffer doc assembled from the pieces of its pages (bm25/wread.c) past 1 GB: the arrays grow with the
 * huge allocation (a doubling past 1 GB failed with "invalid memory alloc request size", and a search or a promotion of the buffer failed for good), and
 * closing the reader frees them.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include <malloc.h>

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/wread.c"

static SiftWbufReader *r;

static void
grow_past_1gb(void *arg)
{
	asm_piece(r, arg);
}

/* A reader whose doc has nasm terms in asmbytes bytes, its arrays full; then one more piece of one term */
static void
check_grow(Size nasm, Size asmbytes)
{
	char		rec[sizeof(SiftWbufDocData) + 8] = {0};
	SiftWbufDocData *d = (SiftWbufDocData *) rec;

	d->nterms = 1;
	d->termbytes = 1;
	r = palloc0(sizeof(SiftWbufReader));
	r->mcxt = CurrentMemoryContext;
	r->page = palloc(BLCKSZ);
	r->nasm = r->asmcap = nasm;
	r->asmbytes = r->asmbytescap = asmbytes;
	r->ahash = palloc(nasm * sizeof(uint32));
	r->atf = palloc(nasm);
	r->alen = palloc(nasm);
	r->aterms = palloc(asmbytes);
	SIFT_CHECKF(pgshim_try(grow_past_1gb, d), "growing %zu terms in %zu bytes failed: %s", nasm, asmbytes, pgshim_errmsg);
	SIFT_CHECKF(r->nasm == nasm + 1 && r->asmbytes == asmbytes + 1, "%zu terms in %zu bytes", (size_t) r->nasm, (size_t) r->asmbytes);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
	{
		Size		mapped = mallinfo2().hblkhd;

		sift_wbuf_close(r);
		SIFT_CHECKF(mapped - mallinfo2().hblkhd >= MaxAllocSize, "closing the reader gave back %zu of the %zu bytes its arrays map",
					mapped - mallinfo2().hblkhd, mapped);
	}
#else
	sift_wbuf_close(r);
#endif
}

int
main(void)
{
	check_grow(MaxAllocSize / sizeof(uint32) / 2 + 1, 1);
	check_grow(1, MaxAllocSize / 2 + 1);
	return sift_check_summary();
}
