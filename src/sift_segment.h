/*
 * sift_segment.h - immutable segments: writer (build, promotion, merge) and
 * reader (docmap, liveness, dictionary, term cursors). DESIGN.md (b).
 *
 * Stream layout: DOCMAP, POSTINGS, DICT, DICTIDX, dictionary root array.
 * DOCMAP: one record per SB with docs, in SB order, each 4-byte aligned:
 *   u32 ndocs, u32 chunk_len, codec chunk of the present docs
 *   (sift_chunk_encode), zero pad to 4, then for every present page in page
 *   order a u16 dl row indexed by offset (slot off-1) up to the page's
 *   highest present offset (absent slots 0); then the SB directory
 *   SiftDocSBRef[nsb] and a final u32 nsb. A doc's rank (liveness bit) is
 *   its position in key order. Liveness: one bit per doc in rank order, plus
 *   a dirty bit per SB (LIVE pages, see sift_page.h).
 */
#ifndef SIFT_SEGMENT_H
#define SIFT_SEGMENT_H

#include "sift_host.h"

#include "sift_dict.h"
#include "sift_posting.h"

typedef struct SiftDocSBRef
{
	uint32		sb;
	uint32		ndocs;
	uint64		off;			/* record offset in the stream */
} SiftDocSBRef;

/*
 * Writer: docs first (strictly ascending key; dl 0 for text without
 * tokens), then terms (strictly ascending bytes, possibly none). avgdl_ref
 * is the segment's own avgdl, so only k1 and b of ref are used. positions:
 * the terms' postings carry position records (SIFT_SEG_POSITIONS).
 */
typedef struct SiftSegmentWriter SiftSegmentWriter;

extern SiftSegmentWriter *sift_segw_begin(SiftWriteTarget *wt, uint64 gen,
										  const SiftScoreRef *ref, MemoryContext mcxt, bool positions);
extern void sift_segw_add_docs(SiftSegmentWriter *w, const SiftKey *keys, const uint16 *dl, int n);

/* Returns the posting writer for this term; finish it with sift_segw_end_term */
extern SiftPostingWriter *sift_segw_begin_term(SiftSegmentWriter *w, const char *term, int len);
extern void sift_segw_end_term(SiftSegmentWriter *w);

/* Writes sections, LIVE pages (all live) and the header; fills the directory entry */
extern void sift_segw_finish(SiftSegmentWriter *w, SiftSegEntry *entry);

/* Reader: opened per scan or maintenance pass; everything lives in mcxt */
typedef struct SiftSegment
{
	Relation	index;
	SiftSegEntry entry;
	SiftSegHeaderData *hdr;		/* copy of the header page */
	SiftStreamReader sr;
	SiftDict   *dict;
	MemoryContext mcxt;
	void	   *docmap;			/* private: SB directory and the loaded SB */
	void	   *live;			/* private: dirty-SB bitmap */
} SiftSegment;

/* Reads the header and the dictionary root; ERRCODE_INDEX_CORRUPTED on format errors */
extern SiftSegment *sift_seg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt);
extern void sift_seg_close(SiftSegment *seg);	/* releases buffers only */

/* Docs of one SB; false when the segment has none there */
typedef struct SiftDocChunk
{
	uint32		sb;
	uint32		ndocs;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint64		rank_base;		/* segment rank of the SB's first doc */
	bool		dirty;			/* some doc in this SB was vacuumed */
} SiftDocChunk;

extern bool sift_seg_docs(SiftSegment *seg, uint32 sb, SiftDocChunk *dc);

/* First SB >= sb with docs; false past the last one */
extern bool sift_seg_docs_next(SiftSegment *seg, uint32 sb, SiftDocChunk *dc);

/*
 * Present offsets of page (sb*256 + page) and their dl row (dl[off-1]);
 * present is zero and dl NULL for pages without docs. dl valid until the
 * next docmap call on seg.
 */
extern void sift_seg_page(SiftSegment *seg, const SiftDocChunk *dc, int page,
						  uint64 *present, const uint16 **dl);

/* Live offsets of a page (== present unless dc->dirty) */
extern void sift_seg_page_live(SiftSegment *seg, const SiftDocChunk *dc, int page,
							   uint64 *live);

/* Does the SB hold vacuumed docs, known without reading its docs (false when the segment has none there) */
extern bool sift_seg_sb_dirty(SiftSegment *seg, uint32 sb);

/* The live postings of the pages of group g (of the SB of dc, dirty) that pages, a subset of g's, selects */
extern uint64 sift_seg_count_live(SiftSegment *seg, const SiftDocChunk *dc, const sift_grp *g, const uint64 *pages);

/* Keeps in mask, a subset of the present offsets of a page of a dirty SB (dc->dirty), the live ones; returns how many are left */
extern int	sift_seg_page_mask_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *mask);

extern bool sift_seg_lookup(SiftSegment *seg, const char *term, int len, SiftTermInfo *info);

/* Does the dictionary term still have a posting of a live doc (always true in a clean segment) */
extern bool sift_seg_term_live(SiftSegment *seg, const SiftTermInfo *info);

/*
 * VACUUM: calls cb for every live doc; clears liveness bits and sets dirty
 * SB bits for deleted ones (generic WAL, <= 4 pages per record). Caller holds
 * the maintenance lock. Returns docs removed; adds to stats. (M4)
 */
extern uint64 sift_seg_live_docs(SiftSegment *seg);

/*
 * The rank of the live doc key (its position among the segment's docs, which indexes the liveness bits), -1 when the
 * segment holds no live doc with it; and the deletion of that one doc, the same way VACUUM deletes (the dirty flag, then the
 * bits): true when it was live. The caller holds the maintenance lock (a host without one has a single writer).
 */
extern int64 sift_seg_live_rank(SiftSegment *seg, SiftKey key);
extern bool sift_seg_delete_key(SiftSegment *seg, SiftKey key);
extern int64 sift_seg_bulkdelete(SiftSegment *seg, IndexBulkDeleteCallback cb, void *cbstate,
								 IndexBulkDeleteResult *stats);

/* ---------- inspection (sift.segments, sift.verify, tests) ---------- */

#define SIFT_DF_CLASSES 9

typedef struct SiftDfClassStats
{
	uint64		terms;
	uint64		postings;
	uint64		inline_terms;
	uint64		list_terms;
	uint64		grouped_terms;
	uint64		codec_bytes;	/* inline terms: 0 */
	uint64		sidecar_bytes;	/* group scores, block bounds, tf stream, padding */
} SiftDfClassStats;

typedef struct SiftSegStats
{
	uint64		nterms;
	uint64		postings;
	uint64		dict_bytes;		/* DICT + DICTIDX + root */
	SiftDfClassStats cls[SIFT_DF_CLASSES];
} SiftSegStats;

/* df class bounds: class i holds lo[i] <= df < lo[i + 1] */
extern const uint32 sift_df_class_lo[SIFT_DF_CLASSES];
extern const char *const sift_df_class_name[SIFT_DF_CLASSES];

/*
 * One decoded term: postings in key order with tf and the doc's dl; pos / poslen (NULL without
 * positions, SIFT_TERM_POS) are the postings' position records back to back and their lengths
 */
typedef bool (*SiftTermVisit) (void *arg, const char *term, int len, const SiftTermInfo *info,
							   const SiftKey *keys, const uint8 *tf, const uint16 *dl,
							   const uint8 *pos, const uint32 *poslen);

/*
 * Walks every term of the segment (dictionary order) and decodes its
 * postings. With verify, checks the docmap, liveness, dictionary order and
 * counts, postings (subset of the docmap, count == df, tf stream lengths,
 * group and block bounds >= the true maximum computed with ref's k1 and b
 * and the segment's avgdl_ref) and raises ERRCODE_INDEX_CORRUPTED. visit
 * may be NULL. Memory: O(segment docs) for the dl map.
 */
extern void sift_seg_walk(SiftSegment *seg, const SiftScoreRef *ref, bool verify,
						  SiftSegStats *st, SiftTermVisit visit, void *arg);

/*
 * Streams the live part of the segment, the input of a merge: docs first
 * (ascending key, dl), then every term with a live posting in dictionary
 * order (info->df is the live count). Memory: O(segment docs) plus the
 * largest term. ref supplies k1 and b.
 */
typedef void (*SiftDocVisit) (void *arg, const SiftKey *keys, const uint16 *dl, int n);

extern void sift_seg_dump(SiftSegment *seg, const SiftScoreRef *ref, SiftDocVisit docs,
						  SiftTermVisit terms, void *arg);

#endif							/* SIFT_SEGMENT_H */
