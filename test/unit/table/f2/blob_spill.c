/*
 * test/unit/table/f2/blob_spill.c - the references of wide values (table/f2/blob.c, sift_tbl_blob_spill_rows) live as long as the row buffer they are put in: a bulk flush spills them in a
 * scratch context that goes before the rows of a moved partition are routed again (col/f2/l0/stage_flush.c), and the routed rows read every reference after that.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"

/* The scratch context of the flush: what is allocated in it is freed with it, as MemoryContextDelete does */
static MemoryContext scratch = (MemoryContext) 0x5c;
static void *in_scratch[64];
static int	nscratch;

static void *
alloc_in(MemoryContext cx, Size n)
{
	void	   *p = MemoryContextAllocZero(cx, n);

	if (cx == scratch)
		in_scratch[nscratch++] = p;
	return p;
}

#define palloc0(n) alloc_in(CurrentMemoryContext, (n))
#define MemoryContextAllocZero(cx, n) alloc_in((cx), (n))

#include "table/f2/blob.c"

#undef palloc0
#undef MemoryContextAllocZero

uint64
sift_manifest_new_gen(Relation rel, uint32 n)
{
	return 1000;
}

bool
sift_colz_available(int alg)
{
	return false;
}

uint32
sift_crc32c(const void *data, Size len)
{
	return (uint32) len;
}

int
main(void)
{
	MemoryContext buffer = (MemoryContext) 0xb0;
	static const char value[] = "a wide value that its reference holds inline";
	const char *pay[1] = {value};
	uint32		plen[1] = {sizeof(value)};
	uint8		tag[1] = {SIFT_COLTAG_WIDE_RAW};
	const char **xpay[1] = {pay};
	uint32	   *xplen[1] = {plen};
	uint8	   *tags[1] = {tag};
	SiftRowBuf	rb = {.cx = buffer,.ncols = 1,.n = 1,.tag = tags,.xpay = xpay,.xplen = xplen};
	const SiftTblBlobRef *ref;

	CurrentMemoryContext = scratch;
	sift_tbl_blob_spill_rows(NULL, &rb);
	for (int i = 0; i < nscratch; i++)
		pfree(in_scratch[i]);
	ref = (const SiftTblBlobRef *) rb.xpay[0][0];
	SIFT_CHECKF(tag[0] == SIFT_COLTAG_WIDE_BLOB, "the value is a reference");
	SIFT_CHECKF(ref->magic == SIFT_TBL_BLOB_MAGIC && ref->raw_len == sizeof(value) && memcmp(ref->ext, value, sizeof(value)) == 0,
				"the reference outlives the scratch context of the flush");
	SIFT_CHECKF(rb.xplen[0][0] == SIFT_TBL_BLOB_REF_SIZE(0) + sizeof(value), "the length is that of the reference");
	return sift_check_summary();
}
