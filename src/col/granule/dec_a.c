/*
 * col/granule/dec_a.c - the entrypoints of a worker to the decoders of a granule (col/granule/a.h): each is the decoder of the backend path run inside the scope of an arena.
 * The calls whose output is numbers in the caller's arrays give back the arena they used.
 */
#include "core/host.h"

#include "col/granule/a.h"

bool
sift_chunk_open_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int idx, SiftChunk *c)
{
	SIFT_ARENA_TRY(a, err)
		sift_chunk_open(v, idx, c);
	SIFT_ARENA_END
}

bool
sift_chunk_close_a(SiftArena *a, SiftErr *err, SiftChunk *c)
{
	SIFT_ARENA_TRY(a, err)
		sift_chunk_close(c);
	SIFT_ARENA_END
}

bool
sift_granule_ints_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, int64 *out, bool *isnull)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		sift_granule_ints(v, col, out, isnull);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_f8_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, double *out, bool *isnull)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		sift_granule_f8(v, col, out, isnull);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_strs_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, SiftStrCell *out)
{
	SIFT_ARENA_TRY(a, err)
		sift_granule_strs(v, col, out);
	SIFT_ARENA_END
}

bool
sift_granule_str_codes_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint32 *codes, bool *isnull, SiftGranuleDict *dict, bool *coded)
{
	SIFT_ARENA_TRY(a, err)
		*coded = sift_granule_str_codes(v, col, codes, isnull, dict);
	SIFT_ARENA_END
}

bool
sift_granule_tags_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint8 *tag)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		sift_granule_tags(v, col, tag);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_chunk_len_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, int *len)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		*len = sift_granule_chunk_len(v, col);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_nonnull_rows_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint16 *out, uint16 cap, uint16 *count)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		*count = sift_granule_nonnull_rows(v, col, out, cap);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_point_ints_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, int64 *out, bool *isnull)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		sift_granule_point_ints(v, col, rows, nrows, out, isnull);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_point_f8_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, double *out, bool *isnull)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		sift_granule_point_f8(v, col, rows, nrows, out, isnull);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}

bool
sift_granule_point_strs_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out)
{
	SIFT_ARENA_TRY(a, err)
		sift_granule_point_strs(v, col, rows, nrows, out);
	SIFT_ARENA_END
}

bool
sift_granule_point_str_codes_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, uint32 *codes, bool *isnull,
							   SiftGranuleDict *dict, bool *coded)
{
	SIFT_ARENA_TRY(a, err)
		*coded = sift_granule_point_str_codes(v, col, rows, nrows, codes, isnull, dict);
	SIFT_ARENA_END
}

bool
sift_granule_point_str_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint16 row, const char **p, uint32 *len, bool *valid)
{
	SIFT_ARENA_TRY(a, err)
		*valid = sift_granule_point_str(v, col, row, p, len);
	SIFT_ARENA_END
}

bool
sift_granule_point_tag_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint16 row, uint8 *tag)
{
	SiftArenaMark m = sift_arena_mark(a);

	SIFT_ARENA_TRY(a, err)
		*tag = sift_granule_point_tag(v, col, row);
		sift_arena_rewind(a, m);
	SIFT_ARENA_END
}
