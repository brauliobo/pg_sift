/*
 * col/ver.h - the version vectors of a column segment (docs/COL-TAM.md section 7): the transaction (xmin) and the command (cmin) that
 * inserted each row of a unit, kept for the rows of a sift_table that are not final yet. 0 is a frozen row. The engine only stores them;
 * what they mean is the owner's (the table AM).
 *
 * One vector is a kind byte and: CONST: the value; DELTA: the first value, the smallest step (int64), the bits of a residual (0..33) and the
 * residuals of the n - 1 steps packed least significant bit first. A run of single row inserts is a step of 1 (cmin) or no step (xmin): no
 * residual bits at all.
 */
#ifndef SIFT_COL_VER_H
#define SIFT_COL_VER_H

#include "core/host.h"

#define SIFT_CVER_CONST		0
#define SIFT_CVER_DELTA		1

/* Appends the vector of n >= 1 values */
extern void sift_cver_encode(const uint32 *v, uint32 n, StringInfo out);
/* Decodes the n values of a vector that is exactly len bytes; false when it is malformed */
extern bool sift_cver_decode(const uint8 *p, Size len, uint32 n, uint32 *out);

/* Xids compare as the server does for normal ones (wraparound); the summary of a unit and of a segment are in that order */
static inline bool
sift_cver_xid_precedes(uint32 a, uint32 b)
{
	return (int32) (a - b) < 0;
}

#endif							/* SIFT_COL_VER_H */
