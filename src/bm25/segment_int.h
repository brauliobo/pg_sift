/*
 * bm25/segment_int.h - shared by the files of the segment code (segment.c, writer.c, liveness.c, walk.c, livestream.c).
 */
#ifndef SIFT_SEGMENT_INT_H
#define SIFT_SEGMENT_INT_H

#include "core/host.h"

#include "core/live.h"
#include "bm25/segment.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
#define ULL(x)			((unsigned long long) (x))
#define MAXOFF			SIFT_MAX_OFFSET

#define corrupt(seg, ...) SIFT_SEG_CORRUPT((seg)->index, (seg)->entry.gen, __VA_ARGS__)

/*
 * The dl rows of a DOCMAP record are 16 bits per slot, or in format 3 8 or 4 bits where the largest dl of the SB allows. The width is
 * kept in bits 30-31 of the record's document count (SIFT_DL_CODE_SHIFT), so a record of format 2 and one of format 3 read alike.
 */
#define SIFT_DL_CODE_SHIFT	30
#define SIFT_DL_CODE_BITS(code)	(16 >> (code))
#define SIFT_DL_ROW_BYTES(code, n)	(((n) * SIFT_DL_CODE_BITS(code) + 7) / 8)

/* Reader docmap state */
typedef struct DocMap
{
	SiftDocSBRef *dir;
	uint64	   *rank_base;
	uint32		nsb;
	uint64		dir_off;
	int64		cur;			/* loaded SB (index into dir), -1 none */
	uint8	   *rec;
	Size		reccap;
	uint64		reclen;			/* bytes of the loaded SB's record */
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	int			np;				/* present pages */
	int			nwmax;			/* words of offsets of the fullest page */
	uint64	   *om;				/* canonical offset masks of present pages */
	const uint8 *row0;			/* the dl rows of the pages, after the chunk */
	int			dlcode;			/* width of the dl rows (SIFT_DL_CODE_BITS) */
	bool		indexed;		/* before, maskidx, rows and rowlen are set */
	uint32		before[SIFT_SB_PAGES];	/* docs on earlier pages (present pages) */
	uint8		maskidx[SIFT_SB_PAGES]; /* present-page index (present pages) */
	const uint8 *rows[SIFT_SB_PAGES];	/* the stored dl row of each present page, NULL for the others */
	uint16		rowlen[SIFT_SB_PAGES];	/* slots of the row: its page's highest present offset */
	MemoryContext mcxt;
	uint16	   *xdl;			/* narrow rows expanded to 16 bits, one after the other, for the pages in xdone */
	Size		xcap;
	uint32		xoff[SIFT_SB_PAGES];	/* where the row of the page starts in xdl */
	uint64		xdone[SIFT_PAGEMASK_WORDS];
	bool		live_ok;		/* live_bits holds the bits of SB live_sb */
	uint32		live_sb;
	uint8	   *live_bits;		/* the SB's docs, bit i for rank rank_base + i, from bit rank_base % 8 */
	Size		live_cap;
	bool		live_om_ok;		/* live_om holds the live offsets of the loaded SB's pages */
	uint64	   *live_om;		/* nwmax words per present page, in the order of om */
} DocMap;

/* The header of the LIVE run, and the region of one SB (its live offsets as a group of bitmaps) read last */
typedef struct Live2
{
	SiftLive2HeaderData h;
	bool		rok;			/* grp holds the region of SB rsb */
	uint32		rsb;
	uint64		rstart;			/* byte of the run where that region starts */
	Size		rcap;
	uint8	   *rbuf;			/* the region from byte 24, so that its rows start at 64 */
	sift_grp	grp;
} Live2;

/* The liveness pages of a segment, as the shared module addresses them */
static inline SiftLive
seg_live(const SiftSegment *seg)
{
	return (SiftLive) {seg->index, seg->entry.gen, seg->hdr->live_first, seg->hdr->live_npages,
		seg->hdr->sb_hi - seg->hdr->sb_lo + 1, seg->hdr->ndocs};
}

/* segment.c: the docmap of the reader */
extern SIFT_INTERNAL DocMap *docmap(SiftSegment *seg);
extern SIFT_INTERNAL void index_sb(SiftSegment *seg, DocMap *dm);
extern SIFT_INTERNAL void load_sb(SiftSegment *seg, DocMap *dm, int64 i);
extern SIFT_INTERNAL int64 sb_search(DocMap *dm, uint32 sb);
extern SIFT_INTERNAL DocMap *loaded(SiftSegment *seg, const SiftDocChunk *dc);
extern SIFT_INTERNAL DocMap *indexed(SiftSegment *seg, const SiftDocChunk *dc);
extern SIFT_INTERNAL void expand_row(DocMap *dm, int page);

/* The dl row of a present page of the indexed SB as 16 bits per slot: narrow rows are expanded on the first use */
static inline const uint16 *
dl_row(DocMap *dm, int page)
{
	if (dm->dlcode == 0)
		return (const uint16 *) dm->rows[page];
	if (!(dm->xdone[page / 64] >> (page % 64) & 1))
		expand_row(dm, page);
	return dm->xdl + dm->xoff[page];
}

/* liveness.c: the format 2 liveness rows and the dead postings */
extern SIFT_INTERNAL Live2 *live2(SiftSegment *seg);
extern SIFT_INTERNAL const sift_grp *live2_rows(SiftSegment *seg, uint32 sb);
extern SIFT_INTERNAL void count_dead(SiftSegment *seg, const SiftLive2HeaderData *h, uint32 *dead);

#endif							/* SIFT_SEGMENT_INT_H */
