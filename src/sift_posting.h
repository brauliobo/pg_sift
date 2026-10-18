/*
 * sift_posting.h - term blobs: codec posting blob + scoring sidecar
 * (DESIGN.md (a), (b)). A codec group (gid) is exactly one superblock (SB).
 *
 * df <= SIFT_INLINE_MAX lives in the dictionary entry. Otherwise the term
 * blob starts 4-byte aligned in the POSTINGS section:
 *   codec blob (sift_encode, tf disabled: matching data only), codec_len
 *   bytes: LIST (Elias-Fano) when df < sift_grouped_min_df, else GROUPED
 *   scoring sidecar at align4(codec_len), absent when it would be empty:
 *     GROUPED blobs: SiftGroupScore[ng] aligned with the codec group index,
 *                    then u8 blk_bound[16] rows, one per group flagged
 *                    SIFT_GS_BLKBOUND, in group order (row of group i = number
 *                    of flagged groups before i); blocks without postings are 0
 *     tf stream (absent when SIFT_TERM_ALL_TF1): L1 bits, one per posting
 *                    in rank order (tf > 1; rank = codec group base + rank
 *                    in group), ceil(df / 8) bytes; nnib nibbles
 *                    min(tf-2, 15), low nibble first, ceil(nnib / 2) bytes;
 *                    an escape byte min(tf, 255) - 17 per nibble 15
 *   position stream (SIFT_TERM_POS), the last pos_len bytes of the blob, unaligned:
 *                    u32 ckpt[ceil(df / SIFT_POS_EVERY)], the byte offset in the
 *                    records of every SIFT_POS_EVERY-th posting (rank order),
 *                    then one record per posting: the token ordinals of the
 *                    term in the doc, first << 1 | (n > 1), then n - 2 if
 *                    n > 1, then the n - 1 gaps minus one, all LEB128 varints
 *                    (sift_analyze.h). n is the true count, which tf
 *                    saturates at 255. Only word terms with df > SIFT_INLINE_MAX
 *                    of a segment written with positions have one.
 * LIST terms with every tf == 1 are the bare codec blob, followed by the position stream if any
 * (post_len == codec_len without). Readers copy the blob with SIFT_KERN_PAD bytes of padding.
 *
 * Bounds: u8 = ceil(255 * max(tf / (tf + K_ref(dl)))) with K_ref from the
 * segment's k1, b and avgdl_ref (sift_kern_bound_u8, conservative: >= every
 * posting's true value); queries scale by max(1, avgdl_now / avgdl_ref) and
 * stop only when the next bound is strictly below the threshold. LIST and
 * inline terms use the term's max_bound.
 */
#ifndef SIFT_POSTING_H
#define SIFT_POSTING_H

#include "sift.h"
#include "sift_bitmap.h"
#include "sift_dict.h"
#include "sift_store.h"

#define SIFT_BLKBOUND_MIN		32	/* postings in a group to store blk_bound[] */
#define SIFT_SPAN_MIN_DF		1024	/* LIST postings from which sift_post_bounds may skip decoding */

/*
 * df from which terms are GROUPED (bitmaps) rather than LIST (docs/CODEC.md 4). Terms of 8K to 64K postings
 * decode their Elias-Fano blocks in every query and have no block bounds as LIST; 8192 makes ranked AND 22%
 * and count(*) of ORs 27% faster for 10% more index (Wikipedia, 218K articles)
 */
#define SIFT_GROUPED_MIN_DF		8192

#define SIFT_POS_EVERY			32	/* postings per position checkpoint */
#define SIFT_POS_SHIFT			5

#define SIFT_GS_ALL_TF1			0x01	/* no L1 member in the group */
#define SIFT_GS_BLKBOUND		0x02

typedef struct SiftGroupScore
{
	uint32		nib_base;		/* L1 members (nibbles) before this group */
	uint32		esc_base;		/* escape bytes before this group */
	uint8		bound;			/* max bound in the group */
	uint8		flags;			/* SIFT_GS_* */
	uint16		reserved;
} SiftGroupScore;

typedef struct SiftScoreRef
{
	float4		k1;
	float4		b;
	float4		avgdl;
} SiftScoreRef;

/* SIFT_GROUPED_MIN_DF; the sift.grouped_min_df GUC overrides it in SIFT_TESTING builds */
extern PGDLLIMPORT int sift_grouped_min_df;

/* The postings of a term take at most this many bytes (the length is a uint32); sift.test_blob_max lowers it in SIFT_TESTING builds */
#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_blob_max;
#define SIFT_BLOB_MAX	((uint64) (sift_test_blob_max > 0 ? sift_test_blob_max : PG_UINT32_MAX))
#else
#define SIFT_BLOB_MAX	((uint64) PG_UINT32_MAX)
#endif

extern uint8 sift_bound_u8(const SiftScoreRef *ref, uint32 tf, uint32 dl);

/*
 * Writer: postings of one term in strictly ascending key order, with tf and the doc's dl. pos
 * holds the position records of the n postings back to back, poslen their lengths; both NULL for
 * a term without positions (all postings of a term alike). The positions are dropped for the
 * terms that stay inline.
 */
typedef struct SiftPostingWriter SiftPostingWriter;

extern SiftPostingWriter *sift_postw_begin(SiftStreamWriter *sw, const SiftScoreRef *ref,
										   MemoryContext mcxt);
extern void sift_postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf,
						   const uint16 *dl, int n, const uint8 *pos, const uint32 *poslen);

/*
 * Finishes the term: encodes, writes the blob (unless inline) and fills every
 * SiftTermInfo field. Memory: about 10 bytes per posting of the term.
 */
extern void sift_postw_end_term(SiftPostingWriter *w, SiftTermInfo *info);

/*
 * The same without a stream (sift_postw_begin with a NULL sw): the term blob in mcxt, padded as the
 * readers need it and shared by every cursor of sift_post_open_blob; NULL for an inline term, which
 * has none. info->post_off stays 0.
 */
extern const uint8 *sift_postw_end_term_mem(SiftPostingWriter *w, SiftTermInfo *info, MemoryContext mcxt);

/*
 * Reader for one term of one segment. Wraps a codec cursor (usable in codec
 * AND/OR/ANDNOT trees) plus the sidecar. Allocations in mcxt; the term blob
 * is copied out of shared buffers, so the cursor holds no buffer (the stream
 * reader keeps its last page pinned and locked until the caller's next
 * sift_sr_get or sift_sr_release).
 */
typedef struct SiftPostingCursor SiftPostingCursor;

extern SiftPostingCursor *sift_post_open(SiftStreamReader *sr, const SiftTermInfo *info,
										 MemoryContext mcxt);

/* A cursor over a blob of sift_postw_end_term_mem (not copied: it must outlive the cursor) */
extern SiftPostingCursor *sift_post_open_blob(const SiftTermInfo *info, const uint8 *blob, MemoryContext mcxt);

/* As sift_post_open, for sift.verify: the codec blob's lengths and offsets are validated first, so corrupt postings raise instead of overrunning */
extern SiftPostingCursor *sift_post_open_verified(SiftStreamReader *sr, const SiftTermInfo *info,
												  MemoryContext mcxt);

#ifdef SIFT_TESTING
/* What this backend's cursors allocated (sift._test_read_stats) */
typedef struct SiftPostStats
{
	uint64		cursors;		/* cursors opened */
	uint64		curbuf_bytes;	/* ... and the buffers they decode groups into */
	uint64		blobs;			/* term blobs copied out of the index */
	uint64		blob_bytes;
} SiftPostStats;
extern SiftPostStats sift_post_stats;
#endif

/* Another cursor over the same blob (shared, read-only), before the first group */
extern SiftPostingCursor *sift_post_clone(const SiftPostingCursor *src, MemoryContext mcxt);

/* The codec cursor positioned by the caller with sift_seek()/sift_next() */
extern sift_cur *sift_post_cursor(SiftPostingCursor *c);

/* Repositions the codec cursor before the first group */
extern void sift_post_rewind(SiftPostingCursor *c);

/* Scoring view of a group (bound, blk bounds) */
typedef struct SiftGroupBounds
{
	uint32		gid;
	uint8		bound;
	uint8		blk_bound[SIFT_BLKS_PER_SB];
} SiftGroupBounds;

/*
 * All group bounds of the term without decoding offsets (UB pass); array
 * valid for the cursor's lifetime. LIST and inline terms return one entry
 * per group touched, all at max_bound; a LIST term with more than
 * SIFT_SPAN_MIN_DF postings and a posting per group on average returns
 * every group from its first to its last without reading the postings.
 */
extern const SiftGroupBounds *sift_post_bounds(SiftPostingCursor *c, int *n);

/*
 * tf of every posting of the cursor's current group, rank order; tf needs
 * the group's n entries plus SIFT_KERN_PAD bytes.
 */
extern void sift_post_group_tf(SiftPostingCursor *c, uint8 *tf);

/* Does the term carry positions */
extern bool sift_post_has_positions(const SiftPostingCursor *c);

/* Occurrences of one posting, ascending; v grows as needed, allocated in mcxt */
typedef struct SiftPosBuf
{
	uint32	   *v;
	uint32		n;
	uint32		cap;
	MemoryContext mcxt;
} SiftPosBuf;

/* Positions of the posting of the given rank in the term (the group's base plus the rank in the group) */
extern void sift_post_positions(SiftPostingCursor *c, uint32 rank, SiftPosBuf *out);

/* The checkpoints and the records of a term with positions, and the records' length */
extern void sift_post_pos_stream(SiftPostingCursor *c, const uint32 **ckpt, const uint8 **rec, uint32 *len);

/*
 * The record at p: its end, the occurrences it holds and the last one; raises
 * ERRCODE_INDEX_CORRUPTED when it runs past end
 */
extern const uint8 *sift_pos_skip(const uint8 *p, const uint8 *end, uint32 *n, uint32 *last);

/*
 * Checks the sidecar against the codec blob: tf stream lengths and group
 * checkpoints, flags (ERRCODE_INDEX_CORRUPTED). Bounds need the docs' dl and
 * are checked by sift_seg_walk.
 */
extern void sift_post_check(SiftPostingCursor *c);

#endif							/* SIFT_POSTING_H */
