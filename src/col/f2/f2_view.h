/*
 * col/f2/f2_view.h - the values of a column of a granule as the arrays of a ColView (col/f2/f2_sink.h), for the scan of an index of format major 2.
 */
#ifndef SIFT_F2_VIEW_H
#define SIFT_F2_VIEW_H

#include "col/am.h"
#include "col/f2/f2_sink.h"
#include "col/granule/granule_filter.h"
#include "core/par/arena.h"
#include "core/par/arena.h"

/*
 * The column c of a granule of n rows. A string column the sink asked for as codes (codes) is given as them when its chunk is a dictionary of the granule. rows (nrows of them, ascending, nrows <
 * SIFT_POINT_ROWS_MAX) are the only rows that will be read: only their values are made, and the arrays hold nothing for the others; NULL: every row.
 */
extern ColView sift_f2_decode_view(const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows);

/* The same for a worker: every array of the view is in the arena (until the caller rewinds it), false with *err when the column cannot be decoded (col/granule/granule_a.h).
 * memo: the strings the filters of the worker kept (col/granule/cfilt_str_simd.h), NULL for none */
extern bool sift_f2_decode_view_a(SiftArena *a, SiftErr *err, const SiftStrMemo *memo, const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows, ColView *out);

/* The rows of the leaf of a path of a document that hold a value of a JSON type it could not store: their value is the payload of their exception, which the view then has (xpay) */
extern void sift_f2_decode_payloads(const SiftGranuleView *v, int c, int n, ColView *cv);

#endif							/* SIFT_F2_VIEW_H */
