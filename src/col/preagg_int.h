/*
 * col/preagg_int.h - shared between the files of the pre-aggregates (col/preagg.c, col/preagg_opt.c, col/preagg_read.c).
 */
#ifndef SIFT_COL_PREAGG_INT_H
#define SIFT_COL_PREAGG_INT_H

#include "core/host.h"

#include "col/preagg.h"

#define PAGG_NULLGROUP	0x01
#define PAGG_MAX_TUPLES	Max(SIFT_PAGG_ROLLUP_TUPLES, SIFT_PAGG_ROWS_CELLS)	/* the most tuples a rollup blob may say it holds */
#define PAGG_MINMAX		0x02			/* the body of the dim has the minimum and maximum of each integer measure after its sums (column format minor 7) */

#endif
