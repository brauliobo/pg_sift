/*
 * sift_posting.h - term blobs: codec posting blob + scoring sidecar
 * (DESIGN.md (a), (b)). A codec group (gid) is exactly one superblock (SB).
 *
 * Term blob layout (4-byte aligned):
 *   SiftPostHeader
 *   codec blob (sift_encode, tf disabled: matching data only)
 *   scoring sidecar, present for every term stored as a blob:
 *     GROUPED blobs: SiftGroupScore[ng] aligned with the codec group index,
 *                    then u8 blk_bound[16] rows for groups with BLKBOUND
 *     tf stream (absent when SIFT_TERM_ALL_TF1): L1 bits, one per posting
 *                    in rank order (tf > 1; rank = codec group base + rank
 *                    in group), a nibble min(tf-2, 15) per L1 member, an
 *                    escape byte min(tf, 255) - 17 per nibble 15
 *
 * Bounds: u8 = ceil(255 * max(tf / (tf + K_ref(dl)))) with K_ref from the
 * segment's avgdl_ref (sift_bound_u8); queries scale by
 * max(1, avgdl_now / avgdl_ref). LIST blobs use the term's max_bound.
 */
#ifndef SIFT_POSTING_H
#define SIFT_POSTING_H

#include "sift.h"
#include "sift_bitmap.h"
#include "sift_dict.h"
#include "sift_store.h"

#define SIFT_BLKBOUND_MIN		32	/* postings in a group to store blk_bound[] */

#define SIFT_GS_ALL_TF1			0x01	/* no L1 member in the group */
#define SIFT_GS_BLKBOUND		0x02

typedef struct SiftPostHeader
{
	uint32		codec_len;
	uint32		sidecar_len;
} SiftPostHeader;

typedef struct SiftGroupScore
{
	uint32		nib_base;		/* L1 members (nibbles) before this group */
	uint32		esc_base;		/* escape bytes before this group */
	uint8		bound;			/* max bound in the group */
	uint8		flags;			/* SIFT_GS_* */
	uint16		blk_idx;		/* index into blk_bound rows if BLKBOUND */
} SiftGroupScore;

typedef struct SiftScoreRef
{
	float4		k1;
	float4		b;
	float4		avgdl;
} SiftScoreRef;

extern uint8 sift_bound_u8(const SiftScoreRef *ref, uint32 tf, uint32 dl);

/* Writer: postings of one term in key order, with tf and the doc's dl */
typedef struct SiftPostingWriter SiftPostingWriter;

extern SiftPostingWriter *sift_postw_begin(SiftStreamWriter *sw, const SiftScoreRef *ref,
										   MemoryContext mcxt);
extern void sift_postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf,
						   const uint16 *dl, int n);
/* Finishes the term: encodes, writes, fills df/flags/max_bound/post_off/post_len or inline data */
extern void sift_postw_end_term(SiftPostingWriter *w, SiftTermInfo *info);

/*
 * Reader for one term of one segment. Wraps a codec cursor (usable in codec
 * AND/OR/ANDNOT trees) plus the sidecar. Allocations in mcxt; the term blob
 * is copied out of shared buffers once (zero-copy for single-page blobs is a
 * later optimization inside this module).
 */
typedef struct SiftPostingCursor SiftPostingCursor;

extern SiftPostingCursor *sift_post_open(SiftStreamReader *sr, const SiftTermInfo *info,
										 MemoryContext mcxt);

/* The codec cursor positioned by the caller with sift_seek()/sift_next() */
extern sift_cur *sift_post_cursor(SiftPostingCursor *c);

/* Scoring view of the group the cursor is on (bound, blk bounds) */
typedef struct SiftGroupBounds
{
	uint32		gid;
	uint8		bound;
	uint8		blk_bound[SIFT_BLKS_PER_SB];
} SiftGroupBounds;

/*
 * All group bounds of the term without decoding offsets (UB pass); array
 * valid for the cursor's lifetime. LIST terms return one entry per group
 * touched, all at max_bound.
 */
extern const SiftGroupBounds *sift_post_bounds(SiftPostingCursor *c, int *n);

/* tf of every posting of the cursor's current group, rank order; >= group n slots */
extern void sift_post_group_tf(SiftPostingCursor *c, uint8 *tf);

#endif							/* SIFT_POSTING_H */
