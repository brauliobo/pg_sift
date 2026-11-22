/*
 * col/f2/delvec_delta.h - the deletion vector with an in-place delta tail and a palette of deleters (spec 42 4), and the codec of the vector.
 *
 * SIFT_DV_FLAG_DELTA_TAIL: a one-page vector whose deleters are not settled takes the rows of a later DELETE where it ends, as SiftDvDeltaChunkHdr chunks past its base, in a
 * record of generic WAL that carries the chunks and a few header words: no new extent, no manifest commit. h->pad is the length of the base; the checksum covers the base only, with
 * the header words that appends change (xid_max, ndead_total, stream_len, pad) read as zero. The vector is rewritten whole (copy on write, one commit for all the parts of a
 * statement) when the page is full, when the deleters settle, and before a compaction counts its dead rows.
 *
 * SIFT_DVG_RLE_XMAX: a granule with 2 to 32 different deleters keeps them once, 8 bytes each, and a byte for each dead row that names its own, where it kept 8 bytes for each row.
 */
#ifndef SIFT_DELVEC_DELTA_H
#define SIFT_DELVEC_DELTA_H

#include "core/host.h"

#include "col/f2/delvec.h"
#include "col/f2/part_format.h"
#include "core/xid_cache.h"

#define SIFT_DV_DELTA_MAGIC			UINT16_C(0x4443)	/* "DC" */
#define SIFT_DV_DELTA_MAX_MARKS		2048
#define SIFT_DV_DELTA_SETTLED		UINT16_C(1)			/* chunk_flags: the rows are plain dead rows (xmax and cmax are 0) */
#define SIFT_DVG_RLE_MAX_UNIQ		32

typedef struct SiftDvDeltaChunkHdr
{
	uint16		magic;			/* SIFT_DV_DELTA_MAGIC */
	uint16		granule_id;
	uint16		nmarks;			/* 1..SIFT_DV_DELTA_MAX_MARKS, the row numbers that follow: uint16 row[nmarks], ascending, padded to 4 bytes */
	uint16		chunk_flags;
	TransactionId xmax;			/* the one deleter of every row of the chunk */
	CommandId	cmax;
} SiftDvDeltaChunkHdr;

StaticAssertDecl(sizeof(SiftDvDeltaChunkHdr) == 16, "SiftDvDeltaChunkHdr must be 16 bytes");

#define SIFT_DV_CHUNK_BYTES(nmarks)	(sizeof(SiftDvDeltaChunkHdr) + (((nmarks) * sizeof(uint16) + 3) & ~(uint32) 3))

/* What the codec asks of the transaction log and the epoch, so that it runs without a server */
typedef struct SiftDvOps
{
	void	   *ctx;
	bool		(*committed) (void *ctx, TransactionId xmax);	/* committed, whatever the snapshot */
	FullTransactionId (*fxid) (void *ctx, TransactionId xmax);	/* the full id of a deleter */
} SiftDvOps;

/* The serialized form of the vector, with the palette where it pays: palloc'd, *len its bytes */
extern char *sift_dvd_serialize(const SiftDv *dv, const SiftDvOps *ops, uint32 *len);

/* The vector of the stream, the delta tail merged; NULL with *why set when the stream is damaged (the checksum, a granule out of order or out of range, an index past the palette, a chunk with no magic) */
extern SiftDv *sift_dvd_deserialize(const char *bytes, uint32 len, uint64 part_gen, uint64 dv_gen, const uint16 *granule_rows, const SiftDvOps *ops, const char **why);

/* The checksum a stream with a delta tail keeps: the base, the words that appends change read as zero */
extern uint32 sift_dvd_base_crc(const char *bytes, uint32 base_len);

/* The marks of one statement for one part: the rows of each granule, ascending, the deleter the same */
typedef struct SiftDvDeltaGran
{
	uint16		granule_id;
	uint16		nrows;
	const uint16 *rows;
} SiftDvDeltaGran;

/* The bytes of the chunks the granules make */
extern uint32 sift_dvd_chunks_bytes(const SiftDvDeltaGran *g, int ng);

/* Does the vector in the page take the chunks past its end, and is it one a delta may be appended to (the flags and the room of the page, not the descriptor of its part) */
extern bool sift_dvd_page_takes(const char *pg, uint32 chunk_bytes);

/* Appends the chunks past the end of the vector in the page (changing only the bytes past pd_lower and the header words above); the deleter is xmax and cmax */
extern void sift_dvd_page_append(char *pg, const SiftDvDeltaGran *g, int ng, TransactionId xmax, CommandId cmax, FullTransactionId fxid);

/* The descriptor of a part allows the append: one block, deleters not settled, a granule count a chunk can name */
static inline bool
sift_dvd_part_takes(const SiftPartDesc *d)
{
	return d->dv_blk != InvalidBlockNumber && d->dv_nblocks == 1 && (d->flags & SIFT_PART_FLAG_DV_SETTLED) == 0 && d->ndead > 0;
}

#ifndef SIFT_HREF_STANDALONE

/* The ops over the transaction status of the backend: committed as the horizon cache tells (hc), the epoch of the cache for the full ids */
typedef struct SiftDvHorizon
{
	SiftXidHorizonCache hc;
	SiftDvOps	ops;
} SiftDvHorizon;

extern void sift_dvd_horizon_open(SiftDvHorizon *h);

typedef struct SiftDvDeltaPart
{
	SiftPartDesc *desc;			/* updated in place by a copy on write, unchanged by a delta */
	const SiftDvDeltaGran *gran;
	int			ngran;
} SiftDvDeltaPart;

/*
 * Appends the chunks of each part to the page of its vector, up to MAX_GENERIC_XLOG_PAGES parts to a record, under the lock of the partition. A part whose page does not take them (the
 * descriptor, the room) is left alone and false is its entry in taken; the caller writes its vector whole.
 */
extern void sift_dv_append_delta_batch(Relation rel, uint32 part_id, const SiftDvDeltaPart *parts, int n, TransactionId xmax, CommandId cmax, bool *taken);

/* Writes the vectors whole, up to MAX_GENERIC_XLOG_PAGES one-page vectors to a record; dv_gen of each is its own, from one sift_manifest_new_gen(rel, n) the caller made; the extents it replaces are the caller's to release */
extern void sift_dv_write_batch(Relation rel, uint32 part_id, const SiftDv *const *dvs, SiftPartDesc **descs, int n);

/* The bytes of the vector at dv_blk, its delta tail included: what a cache of the decoded vector compares, as a delta changes them and not the generation */
extern uint32 sift_dv_stream_len(Relation rel, BlockNumber dv_blk);

/* Folds the delta tails of the parts of the partition into whole vectors (one manifest commit) before a compaction counts the dead rows: how many were folded */
extern int	sift_dv_consolidate(Relation rel, uint64 part_id, const uint8 *key_lo);

#endif							/* !SIFT_HREF_STANDALONE */

#endif							/* SIFT_DELVEC_DELTA_H */
