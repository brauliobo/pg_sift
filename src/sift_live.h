/*
 * sift_live.h - the liveness pages of an immutable segment (DESIGN.md (b) LIVE, docs/COL.md 2.7): a SiftLiveHeaderData, a dirty bit per
 * superblock of the segment's range, then one bit per row in rank order (1 = live), over a contiguous run of SIFT_PT_LIVE pages. Shared by
 * the BM25 segments (docs in rank order) and the column segments (rows in rank order).
 */
#ifndef SIFT_LIVE_H
#define SIFT_LIVE_H

#include "sift_host.h"

#include "sift_store.h"

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

#endif							/* SIFT_LIVE_H */
