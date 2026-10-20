/*
 * bm25/posting_int.h - shared between the posting files (posting.c, postw.c, pos.c).
 */
#ifndef SIFT_POSTING_INT_H
#define SIFT_POSTING_INT_H

#include "bm25/kernels.h"
#include "bm25/posting.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)

struct SiftPostingCursor
{
	SiftTermInfo info;
	sift_cur	cur;
	uint8	   *curbuf;
	const uint8 *blob;
	const SiftGroupScore *gs;	/* GROUPED */
	uint32		ng;
	const uint8 *blkrows;
	const uint8 *l1;
	const uint8 *nib;
	const uint8 *esc;
	Size		bloblen;		/* the blob without its position stream */
	SiftStreamReader *sr;		/* the position stream is read on demand */
	uint64		posoff;			/* its offset in the segment stream */
	uint32		poslen;
	uint32		nck;
	uint32	   *ckpt;			/* checkpoints, read with the first position */
	uint8	   *win;			/* window of the records */
	uint64		win_lo;			/* offset in the records */
	uint32		win_len;
	SiftTfPos	next;			/* LIST: where the last decoded group ended */
	SiftGroupBounds *bounds;
	int			nbounds;
	MemoryContext mcxt;
};

#endif							/* SIFT_POSTING_INT_H */
