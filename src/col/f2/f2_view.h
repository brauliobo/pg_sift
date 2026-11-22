/*
 * col/f2/f2_view.h - the values of a column of a granule as the arrays of a ColView (col/f2/f2_sink.h), for the scan of an index of format major 2.
 */
#ifndef SIFT_F2_VIEW_H
#define SIFT_F2_VIEW_H

#include "col/am.h"
#include "col/f2/f2_sink.h"

/*
 * The column c of a granule of n rows. A string column the sink asked for as codes (codes) is given as them when its chunk is a dictionary of the granule. rows (nrows of them, ascending, nrows <
 * SIFT_POINT_ROWS_MAX) are the only rows that will be read: only their values are made, and the arrays hold nothing for the others; NULL: every row.
 */
extern ColView sift_f2_decode_view(const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows);

#endif							/* SIFT_F2_VIEW_H */
