/*
 * col/preagg_read/preagg_read_int.h - what the files that read the pre-aggregates share
 * the statics one file of the reader calls from another
 */
#ifndef SIFT_COL_PREAGG_READ_INT_H
#define SIFT_COL_PREAGG_READ_INT_H

#include "col/preagg_int.h"

/*
 * col/preagg_read/preagg_read.c - the pre-aggregates read: varint decoding of the cube and rollup blobs, and their verification against the chunks.
 */

typedef struct Rd
{
	const uint8 *p;
	const uint8 *end;
	bool		bad;
	bool		measlen;		/* a rollup blob: each measure is preceded by its length (column format minor 36) */
} Rd;

extern uint64	cpread_get_u(Rd *r);

extern int64	cpread_get_zz(Rd *r);

extern SiftCI128	cpread_get_zz128(Rd *r);

extern bool	cpread_read_dir(const SiftColSchema *s, const uint8 *blob, uint32 len, int max, bool cube, int *nd, int *leaf, uint32 *blen, const uint8 **body);

extern bool	cpread_read_head(const SiftColSchema *s, Rd *rd, SiftCRollup *r);

#endif