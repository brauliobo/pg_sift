/*
 * core/live.h - the liveness pages of an immutable segment: a SiftLiveHeaderData, a dirty bit per
 * superblock of the segment's range, then one bit per row in rank order (1 = live), over a contiguous run of SIFT_PT_LIVE pages. Shared by
 * the BM25 segments (docs in rank order) and the column segments (rows in rank order).
 */
#ifndef SIFT_LIVE_H
#define SIFT_LIVE_H

#include "core/host.h"

#include "core/store.h"

/* Offset of the first liveness bit in the LIVE run of a segment spanning nsb superblocks */
#define SIFT_LIVE_START(nsb)	TYPEALIGN(8, sizeof(SiftLiveHeaderData) + ((nsb) + 7) / 8)

typedef struct SiftLive
{
	Relation	index;
	uint64		gen;
	BlockNumber first;			/* first LIVE page */
	uint32		npages;
	uint32		nsb;			/* superblocks of the segment's range: the bits of the dirty bitmap */
	uint64		nbits;			/* rows */
} SiftLive;

/* A palloc'd copy of the SEGHEADER page of a segment, its size checked against the extents it lists */
extern SiftSegHeaderData *sift_seg_read_header(Relation index, const SiftSegEntry *entry);

/* Sets the directory's dirty flag of the segment, and that of the entry, unless set: rows may be cleared from then on */
extern void sift_seg_mark_dirty(Relation index, SiftSegEntry *entry);

/* Writes the LIVE run of a new segment, all rows live and no SB dirty; returns its first block and page count */
extern void sift_live_write(SiftWriteTarget *wt, uint64 gen, uint32 nsb, uint64 nbits, BlockNumber *first, uint32 *npages);

/* ERRCODE_INDEX_CORRUPTED if the run does not hold the header, the dirty bitmap and nbits bits */
extern void sift_live_check(const SiftLive *l);

/* Byte b of the run (logical offset over the page payloads); *buf is the share-locked page kept between calls (release it with UnlockReleaseBuffer when valid) */
extern uint8 sift_live_byte(const SiftLive *l, uint64 b, Buffer *buf);

/* len bytes of the run from logical byte b, a page at a time */
extern void sift_live_copy(const SiftLive *l, uint64 b, uint64 len, uint8 *out);

/* The dirty bitmap, (nsb + 7) / 8 bytes, bit sb - sb_lo */
extern void sift_live_dirty(const SiftLive *l, uint8 *out);

/* Dead rows counted by the header */
extern uint32 sift_live_ndead(const SiftLive *l);

/* n liveness bits from rank on into out (bit 0 = rank); the nwords >= (n + 63) / 64 words are zeroed first */
extern void sift_live_bits(const SiftLive *l, uint64 rank, uint32 n, uint64 *out, uint32 nwords);

/*
 * Clears the bits of the ranks dead[0..nd) (ascending, within one superblock: at most two bit pages) and sets the SB's dirty bit
 * (sbidx = sb - sb_lo) and counts them dead, in one generic WAL record. The caller marks the directory entry SIFT_SEG_DIRTY first
 * (bits cleared under a clean flag would be ignored by readers).
 */
extern void sift_live_clear(const SiftLive *l, uint32 sbidx, const uint64 *dead, int nd);

/*
 * Writes the LIVE run of a new segment of format 2, all docs live and no SB dirty: reg[nsb + 1] are the region starts and the end
 * (reg[nsb] bytes long), rows the spool file holding the regions, nslots the terms with a slot (their counters are zero)
 */
extern void sift_live2_write(SiftWriteTarget *wt, uint64 gen, uint32 nsb, uint32 nslots, const uint64 *reg, BufFile *rows,
							 BlockNumber *first, uint32 *npages);

/* The header, checked against the run (ERRCODE_INDEX_CORRUPTED when the run cannot hold what it lists) */
extern void sift_live2_header(const SiftLive *l, SiftLive2HeaderData *h);

/* The dirty bitmap, (nsb + 7) / 8 bytes, bit sb - sb_lo */
extern void sift_live2_dirty(const SiftLive *l, const SiftLive2HeaderData *h, uint8 *out);

/* The byte of the run where the region of SB sbidx starts, and its length */
extern uint64 sift_live2_region(const SiftLive *l, const SiftLive2HeaderData *h, uint32 sbidx, uint64 *len);

/*
 * Clears the bits (bit numbers of the run) of nd docs of SB sbidx, counts them and their dl dead and sets the SB's dirty bit,
 * in one generic WAL record. The caller marks the directory entry SIFT_SEG_DIRTY first.
 */
extern void sift_live2_clear(const SiftLive *l, uint32 sbidx, const uint64 *bits, int nd, uint64 dead_dl);

/* The dead postings of the term with this slot */
extern uint32 sift_live2_dead(const SiftLive *l, const SiftLive2HeaderData *h, uint32 slot);

/* The dead postings of every slot */
extern void sift_live2_dead_all(const SiftLive *l, const SiftLive2HeaderData *h, uint32 *out);

/* Stores the table (the pages that differ, a generic WAL record each) and then sets tbl_ndead to ndead, with the header read again */
extern void sift_live2_set_dead(const SiftLive *l, const SiftLive2HeaderData *h, const uint32 *dead);

#endif							/* SIFT_LIVE_H */
