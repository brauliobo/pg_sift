/*
 * agg/wide.h - a numeric leaf that is only summed, over the wide dictionary of col/v1/src_wide.h
 */
#ifndef SIFT_AGGWIDE_H
#define SIFT_AGGWIDE_H

#include "col/v1/src_wide.h"

/*
 * The numeric leaf of the lanes of def as one DNum (agg/combo_int.h) for each row, for a leaf that is only summed: w->codes[i] = i, w->recs[i] the integer and
 * scale of row i, made from the lanes when the row first asks (scale WNUM_UNSET before), or, when it has none (a NaN, an infinity, more than 38 digits), ok false and
 * the numeric's address in m. No entry is hashed, no numeric built, and a row nothing reads costs nothing. The lanes stay those of the unit until the next build.
 */
extern void sift_aggwide_nums(SiftSrcWide *w, const SiftColDef *def, const SiftColVec *lanes, uint32 n);
extern void sift_aggwide_rec(const void *w, uint32 row);

#endif							/* SIFT_AGGWIDE_H */
