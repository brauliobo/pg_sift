/*
 * bm25/posting_int.h - shared between the posting files (posting.c, postw.c, pos.c).
 */
#ifndef SIFT_POSTING_INT_H
#define SIFT_POSTING_INT_H

#include "bm25/kernels.h"
#include "bm25/posting.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)

/* LEB128 at *pp; false when it runs past end */
static inline bool
get_v32(const uint8 **pp, const uint8 *end, uint32 *x)
{
	const uint8 *p = *pp;
	uint32		v = 0;

	for (int s = 0; s < 35; s += 7)
	{
		if (p >= end)
			return false;
		v |= (uint32) (*p & 0x7f) << s;
		if (!(*p++ & 0x80))
		{
			*pp = p;
			*x = v;
			return true;
		}
	}
	return false;
}

/* A position block opened for reading: where its parts are, and the scan of its unary codes */
typedef struct SiftPosBlk
{
	const uint8 *b;
	uint64		nbits;
	uint64		first_bp;		/* of the first positions */
	uint64		end_bp;			/* of the end of them */
	uint64		rb;				/* of the low bits of the gaps */
	uint64		ub;				/* of the unary codes of the gaps */
	uint64		upos;			/* of the code of gap ug, where the last read stopped */
	uint64		ug;
	uint32		k;
	uint32		wf;
	uint32		multi;			/* bit i: posting i has several occurrences */
	uint32		kr;
	uint32		cnt[SIFT_POS_EVERY];
	uint64		gofs[SIFT_POS_EVERY + 1];	/* gaps before each posting */
} SiftPosBlk;

/* A window of the records of a term's position stream, with 8 readable bytes behind it for the bit reads of posblk.c */
typedef struct PosWin
{
	uint8	   *buf;
	uint64		lo;				/* offset in the records */
	uint32		len;
	uint32		cap;
} PosWin;

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
	PosWin		win;
	uint32		cblk;			/* position blocks: the block open in bk, + 1 (0 = none) */
	SiftPosBlk	bk;
	SiftTfPos	next;			/* LIST: where the last decoded group ended */
	SiftGroupBounds *bounds;
	int			nbounds;
	MemoryContext mcxt;
};

/* bm25/posblk.c */
extern void sift_posbuf_grow(SiftPosBuf *out, uint32 n);

static inline void
sift_posbuf_reserve(SiftPosBuf *out, uint32 n)
{
	if (n > out->cap)
		sift_posbuf_grow(out, n);
}
extern bool sift_posblk_open(SiftPosBlk *bk, const uint8 *b, uint32 len, uint32 k);
extern bool sift_posblk_get(SiftPosBlk *bk, uint32 i, uint32 *v);
extern void sift_posblk_put(const uint8 *p, Size len, uint32 k, StringInfo out);
extern bool sift_posblk_leb(const uint8 *b, uint32 len, uint32 k, StringInfo out, Size *lend);

#endif							/* SIFT_POSTING_INT_H */
