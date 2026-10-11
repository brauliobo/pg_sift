/*
 * sift_segment.h - immutable segments: writer (build, promotion, merge) and
 * reader (docmap, liveness, dictionary, term cursors). DESIGN.md (b).
 *
 * DOCMAP per SB: codec chunk of present docs, u32 rowstart[npages], u16 dl
 * rows indexed by offset (slot off-1). Liveness: one bit per doc in segment
 * rank order (SB order, then key), plus a dirty bit per SB.
 */
#ifndef SIFT_SEGMENT_H
#define SIFT_SEGMENT_H

#include "access/genam.h"

#include "sift_dict.h"
#include "sift_posting.h"

/* Writer: docs first (ascending key), then terms (ascending bytes) */
typedef struct SiftSegmentWriter SiftSegmentWriter;

extern SiftSegmentWriter *sift_segw_begin(SiftWriteTarget *wt, uint64 gen,
										  const SiftScoreRef *ref, MemoryContext mcxt);
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
	void	   *docmap;			/* private: loaded SB directory */
	void	   *live;			/* private: dirty-SB bitmap cache */
} SiftSegment;

extern SiftSegment *sift_seg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt);
extern void sift_seg_close(SiftSegment *seg);	/* releases buffers only */

/* Docs of one SB; false when the segment has none there */
typedef struct SiftDocChunk
{
	uint32		sb;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint64		rank_base;		/* segment rank of the SB's first doc */
	bool		dirty;			/* some doc in this SB was vacuumed */
} SiftDocChunk;

extern bool sift_seg_docs(SiftSegment *seg, uint32 sb, SiftDocChunk *dc);

/* Present offsets of page (sb*256 + page) and their dl row; dl valid until next call */
extern void sift_seg_page(SiftSegment *seg, const SiftDocChunk *dc, int page,
						  uint64 *present, const uint16 **dl);

/* Live offsets of a page (== present unless dc->dirty) */
extern void sift_seg_page_live(SiftSegment *seg, const SiftDocChunk *dc, int page,
							   uint64 *live);

extern bool sift_seg_lookup(SiftSegment *seg, const char *term, int len, SiftTermInfo *info);

/*
 * VACUUM: calls cb for every live doc; clears liveness bits and sets dirty
 * SB bits for deleted ones (generic WAL, <= 4 pages per record). Caller holds
 * the maintenance lock. Returns docs removed; adds to stats.
 */
extern int64 sift_seg_bulkdelete(SiftSegment *seg, IndexBulkDeleteCallback cb, void *cbstate,
								 IndexBulkDeleteResult *stats);

#endif							/* SIFT_SEGMENT_H */
