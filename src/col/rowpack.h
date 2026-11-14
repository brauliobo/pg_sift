/*
 * col/rowpack.h - a row of a column batch as bytes and back: the rows of a build that go to temporary files (build/spill.c) and the rows a merge sorts
 * past its memory (order_sort.c)
 */
#ifndef SIFT_COL_ROWPACK_H
#define SIFT_COL_ROWPACK_H

#include "lib/stringinfo.h"
#include "col/enc.h"

/* Appends the row to out: its TID, the values of the top level leaves, then the elements of the arrays */
extern void sift_colrow_pack(StringInfo out, const SiftColSchema *sc, const SiftColRow *row);
/*
 * Reads the row at p into row (val and elem: arrays of ncols the caller owns); the bytes of a string value stay in p, the arrays of elements are allocated in
 * the current context. Returns the end of the row.
 */
extern const char *sift_colrow_unpack(const char *p, const SiftColSchema *sc, SiftColRow *row);

#endif							/* SIFT_COL_ROWPACK_H */
