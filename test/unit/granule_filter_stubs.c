/*
 * granule_filter_stubs.c - the predicate engine that col/granule/granule_filter.c calls and the decoder tests do not run (they link the file for the FSST chunk opener the point decode
 * uses), and the memory-context callbacks of the string memo of col/granule/cfilt_str_simd.c.
 */
#include "postgres.h"

#include "col/f2/f2_pred.h"
#include "col/v1/enc.h"
#include "core/f2/bitscatter.h"
#include "pgshim.h"

int
sift_f2_str_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	pgshim_unreached("sift_f2_str_cmp");
}

bool
sift_f2_str_pred(const SiftColPred *p, const char *s, uint32 len)
{
	pgshim_unreached("sift_f2_str_pred");
}

SiftLikeVerdict
sift_f2_str_verdict(const SiftColPred *p, const char *s, uint32 len)
{
	pgshim_unreached("sift_f2_str_verdict");
}

bool
sift_f2_undecided(const SiftColDef *d, const SiftColPred *p)
{
	pgshim_unreached("sift_f2_undecided");
}

void
sift_ctest_make(const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	pgshim_unreached("sift_ctest_make");
}

bool
sift_ctest_key(const SiftCTest *t, int64 key)
{
	pgshim_unreached("sift_ctest_key");
}

void
sift_bits_scatter_valid(uint64 *row_bits, const uint64 *dense_bits, const uint64 *valid_bits, uint32 nrows)
{
	pgshim_unreached("sift_bits_scatter_valid");
}

MemoryContext
GetMemoryChunkContext(void *pointer)
{
	pgshim_unreached("GetMemoryChunkContext");
}

void
MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
	pgshim_unreached("MemoryContextRegisterResetCallback");
}
