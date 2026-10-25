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

/* A signed value as the varint of its zigzag */
static inline void
put_zz(StringInfo out, int64 v)
{
	sift_col_put_varint(out, ((uint64) v << 1) ^ (uint64) (v >> 63));
}

static inline void
put_zz128(StringInfo out, SiftCI128 v)
{
	unsigned __int128 u = ((unsigned __int128) v << 1) ^ (unsigned __int128) (v >> 127);

	while (u >= 0x80)
	{
		appendStringInfoChar(out, (char) ((uint8) u | 0x80));
		u >>= 7;
	}
	appendStringInfoChar(out, (char) (uint8) u);
}

#endif
