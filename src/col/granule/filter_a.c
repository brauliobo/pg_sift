/*
 * col/granule/filter_a.c - the filters of a granule as a worker thread runs them: each takes the arena and the error of the call (core/par/arena.h), and answers false, with the error filled, for a
 * damaged chunk or an arena that is full. The caller rewinds the arena between granules; a memo of strings (col/granule/cfilt/str_simd.h) lives in it and is zeroed with it.
 */
#include "postgres.h"

#include "col/granule/cfilt/cc.h"
#include "col/granule/cfilt/str_simd.h"
#include "col/granule/filter.h"

bool
sift_cf_cc_filter_a(SiftArena *a, SiftErr *err, const SiftCc *cc, const SiftCfTest *t, uint64 *dense, bool *handled)
{
	SIFT_ARENA_TRY(a, err)
		*handled = sift_cf_cc_filter(cc, t, dense);
	SIFT_ARENA_END
}

bool
sift_cfstr_bits_live_a(SiftArena *a, SiftErr *err, SiftStrMemo *m, const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint32 live_nn, uint64 *dense, bool *handled)
{
	SIFT_ARENA_TRY(a, err)
		*handled = sift_cfstr_bits_live_in(m, cp, c, dense_live, live_nn, dense);
	SIFT_ARENA_END
}

bool
sift_granule_filter_compressed_live_a(SiftArena *a, SiftErr *err, SiftStrMemo *memo, const SiftGranuleView *v, const SiftCfiltPred *cp, const uint64 *live, uint64 *out_mask, SiftCfiltResult *res)
{
	SIFT_ARENA_TRY(a, err)
		*res = sift_granule_filter_in(memo, v, cp, live, out_mask);
	SIFT_ARENA_END
}
