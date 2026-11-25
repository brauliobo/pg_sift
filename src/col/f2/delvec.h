/*
 * col/delvec.h - the deletion vector of a part of format major 2: immutable, copy on write.
 *
 * Deleting rows of a part writes a new vector that holds the rows of the old one and the new ones, and swaps it in with a commit of the manifest; the old one is queued for reuse.
 * Per granule the dead rows are kept as the container that is smallest for their count: a sorted array of 16-bit row numbers up to 512 rows, a bitmap of the 8192 rows beyond,
 * and nothing at all once every row of the granule is dead. A deleter that is not yet settled (not committed before the oldest snapshot) is kept beside its rows as (xmax, cmax),
 * and dropped by the first rewrite that finds the delete committed and old (kept as dead), or aborted (the row is alive again).
 */
#ifndef SIFT_DELVEC_H
#define SIFT_DELVEC_H

#include "core/host.h"

#include "utils/snapshot.h"

#include "col/granule/granule.h"

#define SIFT_DV_MAGIC			0x44563230U		/* "DV20" */
#define SIFT_DV_VERSION			1
#define SIFT_DV_ARRAY_MAX		512
#define SIFT_DV_BITMAP_BYTES	(SIFT_GRANULE_ROWS / 8)

typedef enum SiftDvKind
{
	SIFT_DV_EMPTY = 0,
	SIFT_DV_ARRAY16 = 1,
	SIFT_DV_BITMAP = 2,
	SIFT_DV_ALL_DEAD = 3
} SiftDvKind;

#define SIFT_DV_FLAG_HAS_XMAX	(UINT16_C(1) << 0)
#define SIFT_DV_FLAG_ALL_SETTLED (UINT16_C(1) << 1)	/* every deleter the vector keeps committed (or it keeps none): its rows are dead for every snapshot past the greatest of them */
#define SIFT_DV_FLAG_SINGLE_XMAX (UINT16_C(1) << 2)	/* some granule keeps one (xmax, cmax) for all its dead rows: SIFT_DVG_SINGLE_XMAX in its entry */
#define SIFT_DV_FLAG_DELTA_TAIL	(UINT16_C(1) << 3)	/* the vector of one page has chunks past its base: h->pad is the length of the base (col/f2/delvec_delta.h) */
#define SIFT_DV_FLAG_RLE_XMAX	(UINT16_C(1) << 4)	/* some granule keeps its deleters as a palette: SIFT_DVG_RLE_XMAX in its entry */

/* In SiftDvGranuleEntry.kind above the kind: every dead row of the granule has the deleter that xmax_off holds once */
#define SIFT_DVG_SINGLE_XMAX	UINT16_C(0x100)
#define SIFT_DVG_RLE_XMAX		UINT16_C(0x200)		/* the dead rows have 2 to 32 deleters between them: a palette of them and a byte for each row that names its own */
#define SIFT_DVG_KIND_MASK		UINT16_C(0xFF)

typedef struct SiftDvHeaderData
{
	uint32		magic;
	uint16		version;
	uint16		flags;
	uint32		crc32c;			/* over the stream from byte 12 */
	uint32		ngranules;
	uint64		part_gen;
	uint64		dv_gen;
	FullTransactionId xid_min;
	FullTransactionId xid_max;
	uint64		ndead_total;
	uint32		ndirty_granules;
	uint32		stream_len;		/* the bytes of the whole vector */
	uint32		nextents;		/* 1: the vector is one extent */
	uint32		pad;
} SiftDvHeaderData;

StaticAssertDecl(sizeof(SiftDvHeaderData) == 72, "SiftDvHeaderData must be 72 bytes");

typedef struct SiftDvGranuleEntry
{
	uint32		granule_id;
	uint16		kind;
	uint16		ndead;
	uint32		payload_off;
	uint32		xmax_off;		/* the (xmax, cmax) of each dead row in row order (of the granule once with SIFT_DVG_SINGLE_XMAX), or 0 */
	FullTransactionId xid_max;
} SiftDvGranuleEntry;

StaticAssertDecl(sizeof(SiftDvGranuleEntry) == 24, "SiftDvGranuleEntry must be 24 bytes");

typedef struct SiftDvXmaxEntry
{
	TransactionId xmax;
	CommandId	cmax;
} SiftDvXmaxEntry;

StaticAssertDecl(sizeof(SiftDvXmaxEntry) == 8, "SiftDvXmaxEntry must be 8 bytes");

/* A deletion of one row; xmax invalid: settled */
typedef struct SiftDvMark
{
	uint32		granule;
	uint16		row;
	TransactionId xmax;
	CommandId	cmax;
} SiftDvMark;

/* The dead rows of one granule in memory, ascending */
typedef struct SiftDvGran
{
	uint32		granule_id;
	uint16		granule_rows;
	int			ndead;
	uint16	   *rows;
	SiftDvXmaxEntry *xmax;		/* parallel to rows */
	FullTransactionId xid_max;	/* the greatest deleter it keeps (invalid when it keeps none) */
} SiftDvGran;

typedef struct SiftDv
{
	uint64		part_gen;
	uint64		dv_gen;
	uint32		ngranules;
	int			ndirty;
	SiftDvGran *gran;			/* ascending granule_id */
} SiftDv;

/* The vector of the rows of base (may be NULL) and the marks; a mark of a row already dead keeps the first; granule_rows[g] is the rows of granule g */
extern SiftDv *sift_dv_merge(const SiftDv *base, const SiftDvMark *marks, int n, uint64 part_gen, uint64 dv_gen, uint32 ngranules, const uint16 *granule_rows);

extern const SiftDvGran *sift_dv_granule(const SiftDv *dv, uint32 granule);
extern uint64 sift_dv_ndead(const SiftDv *dv);

/* The deleter of a dead row: its entry in the granule, or NULL when the row is not dead */
extern const SiftDvXmaxEntry *sift_dv_find(const SiftDvGran *g, uint16 row, bool *dead);

/* sift_dv_find for the rows of a granule asked in ascending order: the place of the last is kept, so a walk of all rows is one pass over the dead ones (g may be NULL: none is dead) */
typedef struct SiftDvCursor
{
	const SiftDvGran *gran;
	int			pos;
} SiftDvCursor;

static inline void
sift_dv_cursor_init(SiftDvCursor *c, const SiftDvGran *g)
{
	*c = (SiftDvCursor) {.gran = g};
}

static inline const SiftDvXmaxEntry *
sift_dv_cursor_find(SiftDvCursor *c, uint16 row, bool *dead)
{
	if (c->gran == NULL)
		return NULL;
	while (c->pos < c->gran->ndead && c->gran->rows[c->pos] < row)
		c->pos++;
	if (c->pos == c->gran->ndead || c->gran->rows[c->pos] != row)
		return NULL;
	*dead = true;
	return &c->gran->xmax[c->pos];
}

/* The dead rows of a granule as a bitmap of 8192 bits */
extern void sift_dv_mask(const SiftDvGran *g, uint8 *bitmap);

#define SIFT_DV_MASK_WORDS		(SIFT_GRANULE_ROWS / 64)

/*
 * Fills the 128 words (1024 bytes, 64-byte aligned) of a bitmap with the deletions of the granule that the snapshot sees (snap NULL: every one) and returns how many; 0 tells the
 * caller to pass no mask. A settled vector (no deleter kept) is a plain copy of its rows with no read of the clog.
 */
extern uint16 sift_dv_mask64(const SiftDvGran *g, Snapshot snap, uint64 *words128);

/* The rows of the granule whose deletion the snapshot sees, ascending, the first max of them in out; returns the number of all of them, which may be more than max */
extern uint16 sift_dv_visible_rows(const SiftDvGran *g, Snapshot snap, uint16 *out, uint16 max);

/* The serialized form and back (the checksum of the stream is checked) */
extern char *sift_dv_serialize(const SiftDv *dv, uint32 *len);
extern SiftDv *sift_dv_deserialize(Relation rel, const char *bytes, uint32 len, uint64 part_gen, uint64 dv_gen, const uint16 *granule_rows);

/*
 * Settles the vector: a delete that committed before the horizon is kept as a plain dead row, one that aborted before it is dropped; returns the new vector. *stripped is the
 * greatest of the deleters it made plain (invalid when none): the snapshots that precede it have lost what told them the row alive, which a standby has to be told (core/wal_conflict.h)
 */
extern SiftDv *sift_dv_settle(const SiftDv *dv, FullTransactionId horizon, FullTransactionId *stripped);

/*
 * The vector of a part as the stream it was read from, a granule decoded when asked: what a delete merges its marks into, so that what it holds is the stream (a few bytes for a granule that
 * is all dead or has one deleter) and not the rows it stands for. The rows of an aborted delete are alive in it (a delete that follows marks them again): *unaborted says whether there were any.
 */
typedef struct SiftDvImage SiftDvImage;

extern SiftDvImage *sift_dv_image_read(Relation rel, const SiftPartDesc *desc, const uint16 *granule_rows, bool *unaborted);
extern bool sift_dv_image_dead(const SiftDvImage *img, uint32 granule, uint16 row);

/* sift_dv_merge on the image (may be NULL): the stream of the vector, the granules no mark names copied as they are; *len its bytes */
extern char *sift_dv_image_merge(const SiftDvImage *img, const SiftDvMark *marks, int n, uint64 part_gen, uint64 dv_gen, uint32 ngranules, const uint16 *granule_rows, uint32 *len);

/* Writes the vector into an extent (class size in the returned extent) and fills in the dv fields of the descriptor of its part */
extern SiftExtent sift_dv_write(Relation rel, const SiftDv *dv, uint32 part_id, SiftPartDesc *desc);

extern SiftDv *sift_dv_read(Relation rel, const SiftPartDesc *desc, uint32 part_id, const uint16 *granule_rows);

/* The same, with the length of the stream it read: the one a later sift_dv_stream_len is compared with */
extern SiftDv *sift_dv_read_stream(Relation rel, const SiftPartDesc *desc, uint32 part_id, const uint16 *granule_rows, uint32 *stream_len);

#endif							/* SIFT_DELVEC_H */
