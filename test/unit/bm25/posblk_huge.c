/*
 * test/unit/bm25/posblk_huge.c - the position block of a posting with more than 256 M occurrences (bm25/posblk.c): the gaps of the block and the
 * positions a reader decodes take the huge allocation (a plain one failed with "invalid memory alloc request size", and the merge or the read of
 * the term failed for good).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/posblk.c"

#define NGAP	(MaxAllocSize / sizeof(uint32) + 1)

typedef struct Put
{
	const uint8 *rec;
	Size		len;
	StringInfoData out;
} Put;

static void
put(void *arg)
{
	Put		   *pt = arg;

	initStringInfo(&pt->out);
	sift_posblk_put(pt->rec, pt->len, 1, &pt->out);
}

/* One posting at position 0 with NGAP more occurrences, each one past the last: a record of NGAP bytes of gaps */
static void
check_put(void)
{
	uint8	   *rec = MemoryContextAllocHuge(CurrentMemoryContext, NGAP + 16),
			   *p = rec;
	uint32		more = NGAP - 1;
	Put			pt;
	SiftPosBlk	bk;

	*p++ = 1;
	for (; more >= 0x80; more >>= 7)
		*p++ = (uint8) (more | 0x80);
	*p++ = (uint8) more;
	memset(p, 0, NGAP);
	pt = (Put) {.rec = rec,.len = (p - rec) + NGAP};
	SIFT_CHECKF(pgshim_try(put, &pt), "the block of %zu occurrences failed: %s", (size_t) NGAP + 1, pgshim_errmsg);
	SIFT_CHECKF(sift_posblk_open(&bk, (const uint8 *) pt.out.data, pt.out.len, 1), "the block of %d bytes does not open", pt.out.len);
	pfree(rec);
}

static void
grow(void *arg)
{
	sift_posbuf_grow(arg, NGAP);
}

static void
check_grow(void)
{
	SiftPosBuf	b = {.mcxt = CurrentMemoryContext};

	SIFT_CHECKF(pgshim_try(grow, &b), "the positions of a posting grown to %zu failed: %s", (size_t) NGAP, pgshim_errmsg);
	SIFT_CHECKF(b.cap >= NGAP, "the positions hold %u", b.cap);
}

int
main(void)
{
	check_put();
	check_grow();
	return sift_check_summary();
}
