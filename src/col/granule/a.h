/*
 * col/granule/a.h - the decoders of a granule for a worker (col/granule/dec_a.c): the functions of col/granule.h and col/granule_int.h that read a chunk, with the memory of the
 * arena and the failure as data (core/par/arena.h). Each returns true, or false with *err filled (SIFT_ERR_CORRUPT for bytes the format does not allow, SIFT_ERR_UNSUPPORTED for an encoding or
 * wrapper this build lacks, SIFT_ERR_INVALID_PARAM for a column that is not in the granule, SIFT_ERR_ARENA_FULL when the arena cannot hold what the chunk needs); the error has the text of
 * the one the backend raises. The granule record and the part's dictionaries are read only, the view is opened by the leader (sift_granule_open).
 *
 * What the arena holds when a call returns true: nothing for the calls that fill the caller's arrays with numbers (ints, f8, their point reads, tags, nonnull_rows: their scratch is rewound),
 * and what the output points into for the others (strings, dictionaries, a chunk): the caller rewinds the arena after the granule.
 */
#ifndef SIFT_GRANULE_A_H
#define SIFT_GRANULE_A_H

#include "col/granule/int.h"

extern bool sift_chunk_open_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int idx, SiftChunk *c);
/* Does nothing a rewind of the arena does not: a worker closes a chunk by rewinding to the mark it took before opening it */
extern bool sift_chunk_close_a(SiftArena *a, SiftErr *err, SiftChunk *c);

extern bool sift_granule_ints_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, int64 *out, bool *isnull);
extern bool sift_granule_f8_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, double *out, bool *isnull);
extern bool sift_granule_strs_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, SiftStrCell *out);
/* *coded: the chunk is a dictionary of strings and codes, isnull and *dict are set; false: nothing is written */
extern bool sift_granule_str_codes_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint32 *codes, bool *isnull, SiftGranuleDict *dict, bool *coded);
extern bool sift_granule_tags_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint8 *tag);
extern bool sift_granule_chunk_len_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, int *len);
extern bool sift_granule_nonnull_rows_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint16 *out, uint16 cap, uint16 *count);

extern bool sift_granule_point_ints_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, int64 *out, bool *isnull);
extern bool sift_granule_point_f8_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, double *out, bool *isnull);
extern bool sift_granule_point_strs_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out);
extern bool sift_granule_point_str_codes_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, uint32 *codes, bool *isnull,
										   SiftGranuleDict *dict, bool *coded);
/* *valid: the row holds a value, *p and *len are its string */
extern bool sift_granule_point_str_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint16 row, const char **p, uint32 *len, bool *valid);
extern bool sift_granule_point_tag_a(SiftArena *a, SiftErr *err, const SiftGranuleView *v, int col, uint16 row, uint8 *tag);

#endif							/* SIFT_GRANULE_A_H */
