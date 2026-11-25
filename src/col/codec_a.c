/*
 * col/codec_a.c - the reader of the chunk codec for a worker: each routine of col/codec.h that allocates or raises, run in the scope of an
 * arena (core/par/arena.h). The scratch of a call is rewound when it returns; the nodes of a chunk opened here and the arrays of a string list stay
 * until the caller rewinds the arena, which also drops what a failed call left.
 */
#include "col/codec_int.h"

#define CC_SCRATCH_TRY(a, err) \
	SiftArenaMark cc_mark_ = sift_arena_mark(a); \
	SIFT_ARENA_TRY(a, err)

#define CC_SCRATCH_END(a) \
	sift_arena_rewind((a), cc_mark_); \
	SIFT_ARENA_END

bool
sift_cc_open_a(SiftArena *a, SiftErr *err, const uint8 *blob, Size len, bool verify, SiftCc **cc)
{
	SIFT_ARENA_TRY(a, err)
		*cc = sift_cc_open(NULL, blob, len, verify);
	SIFT_ARENA_END
}

bool
sift_cc_open_rows_a(SiftArena *a, SiftErr *err, const uint8 *blob, Size len, bool verify, uint32 nrows, SiftCc **cc)
{
	SIFT_ARENA_TRY(a, err)
		*cc = sift_cc_open_rows(NULL, blob, len, verify, nrows);
	SIFT_ARENA_END
}

bool
sift_cc_decode_a(SiftArena *a, SiftErr *err, const SiftCc *c, int64 *dst)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_decode(c, dst);
	CC_SCRATCH_END(a)
}

bool
sift_cc_decode_head_a(SiftArena *a, SiftErr *err, const SiftCc *c, uint32 w, int64 *dst)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_decode_head(c, w, dst);
	CC_SCRATCH_END(a)
}

bool
sift_cc_decode_u32_a(SiftArena *a, SiftErr *err, const SiftCc *c, uint32 *dst)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_decode_u32(c, dst);
	CC_SCRATCH_END(a)
}

bool
sift_cc_decode_f8_a(SiftArena *a, SiftErr *err, const SiftCc *c, double *dst)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_decode_f8(c, dst);
	CC_SCRATCH_END(a)
}

bool
sift_cc_decode_sel_a(SiftArena *a, SiftErr *err, const SiftCc *c, const uint32 *idx, uint32 m, int64 *dst)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_decode_sel(c, idx, m, dst);
	CC_SCRATCH_END(a)
}

bool
sift_cc_get_a(SiftArena *a, SiftErr *err, const SiftCc *c, uint32 row, int64 *key)
{
	CC_SCRATCH_TRY(a, err)
		*key = sift_cc_get(c, row);
	CC_SCRATCH_END(a)
}

bool
sift_cc_decode_off32_a(SiftArena *a, SiftErr *err, const SiftCc *c, uint32 *dst, bool *fits)
{
	CC_SCRATCH_TRY(a, err)
		*fits = sift_cc_decode_off32(c, dst);
	CC_SCRATCH_END(a)
}

bool
sift_cc_filter_a(SiftArena *a, SiftErr *err, const SiftCc *c, int64 lo, int64 hi, uint64 *bits)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_filter(c, lo, hi, bits);
	CC_SCRATCH_END(a)
}

bool
sift_cc_filter_member_a(SiftArena *a, SiftErr *err, const SiftCc *c, const uint64 *member, uint32 nmember, uint64 *bits)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_filter_member(c, member, nmember, bits);
	CC_SCRATCH_END(a)
}

bool
sift_cc_sum_a(SiftArena *a, SiftErr *err, const SiftCc *c, const uint64 *bits, int64 *sum, uint64 *count)
{
	CC_SCRATCH_TRY(a, err)
		*count = sift_cc_sum(c, bits, sum);
	CC_SCRATCH_END(a)
}

bool
sift_cc_decode_strs_dict_a(SiftArena *a, SiftErr *err, const uint8 *blob, Size len, uint32 n, uint32 min_n, Size min_bytes, uint32 **off,
						   char **bytes, const void *ddict)
{
	SIFT_ARENA_TRY(a, err)
		sift_cc_decode_strs_dict(NULL, blob, len, n, min_n, min_bytes, off, bytes, ddict);
	SIFT_ARENA_END
}

bool
sift_cc_decode_bits_a(SiftArena *a, SiftErr *err, const uint8 *blob, Size len, uint32 nbits, uint64 *words)
{
	CC_SCRATCH_TRY(a, err)
		sift_cc_decode_bits(blob, len, nbits, words);
	CC_SCRATCH_END(a)
}
