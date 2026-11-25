/*
 * col/granule_int.h - what the files of a granule share: the pieces of a chunk (validity, exception block, body), the codec blobs the bodies are made of and the decoders of a body.
 */
#ifndef SIFT_GRANULE_INT_H
#define SIFT_GRANULE_INT_H

#include "core/host.h"

#include "col/codec/codec.h"
#include "core/par/sbuf.h"
#include "col/granule/granule.h"
#include "core/par/arena.h"

static inline int
bits_for(uint64 v)
{
	return v == 0 ? 0 : 64 - __builtin_clzll(v);
}

static inline void
pad8(StringInfo s)
{
	while (s->len % 8 != 0)
		sift_sb_char(s, '\0');
}

/* The most bytes a chunk has raw when it is wrapped: what one palloc holds with the 8 bytes its reader adds, the most a writer can make one of in a StringInfo */
#define SIFT_CHUNK_RAW_MAX	((uint64) MaxAllocSize - 8)
#define SIFT_DICT_GRANULE_MAX	65536
#define SIFT_ALP_RD_DICT		8
#define SIFT_ALP_RD_MIN_LEFT	8
#define SIFT_ALP_RD_MAX_LEFT	16
#define SIFT_ALPRD_HAS_EXC		UINT8_C(1)
#define SIFT_ALPRD_MAX_TZ		48

static inline SiftVariantKind
sift_exc_kind(uint8 tag)
{
	return tag == SIFT_COLTAG_OBJECT || tag == SIFT_COLTAG_ARRAY || tag == SIFT_COLTAG_NONE ? SVAR_JSONB_DOC : SVAR_JSONB_SCALAR;
}

static inline const SiftVariantExcEntry *
sift_exc_entries(const SiftExcHeader *h)
{
	return (const SiftVariantExcEntry *) ((const char *) h + sizeof(SiftExcHeader) + SIFT_EXC_MASK_BYTES(h));
}

extern void sift_chunk_open(const SiftGranuleView *v, int idx, SiftChunk *c);

static inline void
sift_granule_check_col(const SiftGranuleView *v, int col)
{
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		SIFT_FAIL_INVALID("sift: column %d is not in the granule", col);
}

static inline void
sift_chunk_open_col(const SiftGranuleView *v, int col, SiftChunk *c)
{
	sift_granule_check_col(v, col);
	sift_chunk_open(v, col + 1, c);
}

/* Frees the buffers a chunk owns (idempotent); the release for strings keeps the decompressed bytes when the strings point into them */
extern void sift_chunk_close(SiftChunk *c);
extern void sift_chunk_release_strs(SiftChunk *c);

/* How many of the rows [0, target_row) hold a value, from the state of the ascending calls before it (*cur_word, *cur_rank: start at 0; a target before the word of the state starts over) */
extern int	sift_chunk_rank_advance(const SiftChunk *c, int *cur_word, int *cur_rank, int target_row);

static inline bool
sift_chunk_row_valid(const SiftChunk *c, int row)
{
	if (c->ref->flags & SIFT_CHUNK_ALL_NULL)
		return false;
	return c->validity == NULL || ((c->validity[row >> 3] >> (row & 7)) & 1);
}

/* How many of the rows [0, row) hold a value */
extern int	sift_chunk_rank(const SiftChunk *c, int row);

/* The bytes of the validity of an unwrapped chunk, and the rows of its nrows that hold a value read from them (p, len: the chunk as stored): the count if it is at most cap, else cap + 1 */
extern uint32 sift_chunk_validity_len(const SiftChunkRef *ref, int nrows);
extern uint16 sift_chunk_nonnull_rows(const SiftChunkRef *ref, const char *p, uint32 len, int nrows, uint16 *out, uint16 cap);

/* The mask of the exception rows of a chunk: the stored one, or one made from its entries (a palloc'd 1024 bytes) */
extern const uint64 *sift_chunk_exc_mask(const SiftChunk *c);

/* The cascade of an integer-class body that sift_cc_decode_sel reads (NULL for a body that is not one): closed with sift_cc_close */
extern SiftCc *sift_chunk_cc_open(const SiftChunk *c);

/* The values of the body of an integer-class chunk, nonnull indexes [k0, k1) */
extern void sift_chunk_ints(const SiftChunk *c, int k0, int k1, int64 *out);
extern void sift_chunk_f8(const SiftChunk *c, int k0, int k1, double *out);
/* The values at the nonnull indexes idx[0..m) (ascending), decoded together */
extern void sift_chunk_ints_sel(const SiftChunk *c, const uint32 *idx, int m, int64 *out);
extern void sift_chunk_f8_sel(const SiftChunk *c, const uint32 *idx, int m, double *out);

/* The strings of a string-class chunk: the nonnull indexes [k0, k1) as pointers into the chunk and lengths */
extern void sift_chunk_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len);

/* The same, keeping what it allocated for the strings in own, which sift_str_own_free frees */
extern void sift_chunk_strs_own(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len, SiftStrOwn *own);
extern void sift_str_own_free(SiftStrOwn *own);

/* The one string of nonnull index k, decoded without the others */
extern void sift_chunk_str_one(const SiftChunk *c, int k, const char **p, uint32 *len);

/* The offsets and bytes of every string of a STR_BLOCK chunk, palloc'd */
extern void sift_chunk_block_strs_own(const SiftChunk *c, uint32 **offs, char **bytes);

/* The dictionary of a DICT_CODE chunk: its strings, and where its codes are (a cascade at *codes, *codes_len bytes) */
extern void sift_chunk_dict(const SiftChunk *c, SiftGranuleDict *d, const char **codes, uint32 *codes_len);

/* A codec blob behind a length: [u32 length, u32 0, blob, padding to 8 bytes]; the length of the whole in the return value */
extern uint32 sift_cc_blob_put(StringInfo s, const StringInfoData *blob);
extern SiftCc *sift_cc_blob_open(const char *p, uint32 avail, uint32 nvals, uint32 *whole);

/* The values of a cascade held in the body at p (nvals of them), indexes [k0, k1) */
extern void sift_cc_blob_ints(const char *p, uint32 avail, uint32 nvals, int k0, int k1, int64 *out);

typedef enum SiftTidMode
{
	SIFT_TID_MODE_SEQ_DELTA = 1,	/* the step from the heap block of the row before, a run while rows share a page */
	SIFT_TID_MODE_BLK_DICT = 2,		/* the distinct blocks (at most 2048) and the index of each row */
	SIFT_TID_MODE_PFOR_MINI = 3,	/* frames of 128 rows of the exact bits their spread needs, the rows far from their frame patched */
	SIFT_TID_MODE_LINEAR = 4,		/* one cascade of block * (param0 + 1) + offset: no stream of offsets */
	SIFT_TID_MODE_BLK_BITMAP = 5	/* the distinct blocks as a bit per block from the lowest, and the rank of each row's block among them */
} SiftTidMode;

#define SIFT_TID_OFFS_GUESSED	UINT8_C(1)	/* the offsets are the miss of the row after the one before on the same page */
#define SIFT_TID_PFOR_ZSTD		UINT8_C(2)	/* PFOR_MINI: the frames and patches are one zstd stream (raw length, compressed length, the stream) */

typedef struct SiftTidBlockHeader
{
	uint8		mode;			/* SiftTidMode */
	uint8		flags;			/* SIFT_TID_OFFS_GUESSED, SIFT_TID_PFOR_ZSTD */
	uint16		param0;			/* BLK_DICT, BLK_BITMAP: the distinct blocks; PFOR_MINI: the rows of a frame; LINEAR: the largest offset */
	uint32		n_patches;		/* PFOR_MINI: the rows held apart */
} SiftTidBlockHeader;

StaticAssertDecl(sizeof(SiftTidBlockHeader) == 8, "SiftTidBlockHeader must be 8 bytes");

/* The _tid chunk (SIFT_CENC_TID_BLOCK) of a granule of n rows */
extern void sift_chunk_tids(const SiftChunk *c, int n, uint64 *out);
/* The same for the first w of the n rows: out[w..n) is not written */
extern void sift_chunk_tids_head(const SiftChunk *c, int n, int w, uint64 *out);

/* Encoding of one column (granule_enc.c) */
typedef struct SiftChunkOpts
{
	SiftCompressTier tier;
	int			alg;
	const SiftDictEnc *dict;	/* the part's dictionary of the column, or NULL */
	const struct SiftFsst *fsst;	/* the table of symbols of the column over the whole part, or NULL */
	const void *cdict;			/* the zstd dictionary of the column over the whole part, or NULL */
	bool		framed;			/* the strings of a block and the doubles of a split-bits chunk go in frames of SIFT_FRAME_ROWS values (SIFT_ENC_FRAMED): a part of a sift_table */
} SiftChunkOpts;

/* Framed chunks (enc_frame.c) */
typedef void (*SiftFramePut) (StringInfo frame, int first, int cnt, void *arg);
extern void sift_frames_put(StringInfo s, int nn, SiftFramePut put, void *arg);
extern void sift_frame_f8_put(StringInfo s, const double *vals, int cnt, int alg);
extern bool sift_chunk_framed(const SiftChunk *c);

/* The offsets and bytes of the strings of frame f, palloc'd */
extern void sift_frame_strs(const SiftChunk *c, uint32 f, uint32 **off, char **bytes);

/* The strings [k0, k1) of a framed block chunk, from the frames they lie in only; the buffer they point into is given in *bytes_out (palloc'd) */
extern void sift_frames_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len, void **bytes_out);

/* The offsets and bytes of every string of a framed block chunk, palloc'd (as sift_chunk_block_strs_own gives those of an unframed one) */
extern void sift_frames_block_strs(const SiftChunk *c, uint32 **offs, char **bytes);

/* The doubles [k0, k1) of a framed split-bits chunk, from the frames they lie in only */
extern void sift_frames_f8(const SiftChunk *c, int k0, int k1, double *out);

extern int	sift_put_cc_ints(StringInfo s, const int64 *vals, int n, const SiftChunkOpts *opt);
extern void sift_chunk_put_str(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt);
extern void sift_chunk_put(StringInfo body, const SiftColIn *col, int n, SiftChunkRef *ref, const SiftChunkOpts *opt);
extern void sift_chunk_put_int(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt);
extern void sift_chunk_put_tids(StringInfo s, const uint64 *tid, int n, SiftChunkRef *ref, const SiftChunkOpts *opt);

/* Text held as a type (granule/promo.c): whether the rows of an astext column are mostly one, the body of a granule's chunk of it (false: the granule is mostly not, keep the text), the zone of a granule, the text of ranks [k0, k1) of such a chunk */
extern bool sift_promo_infer(const SiftColIn *col, int n, const SiftDictEnc *dict, SiftColPromo *out);
extern bool sift_chunk_put_promo(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt);
extern void sift_promo_zone(const SiftColPromo *pm, const SiftColIn *col, int n, SiftZone *z);
extern void sift_promo_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len);

/* SIFT_PROMO_PREFIX_INT (promo_prefix.c): the prefix a chunk of it says, a row's number under it, the rows' text written back; whether the part holds a column so (against the dictionary of the part, or the text) */
#define SIFT_PROMO_PREFIX_MAX		31
#define SIFT_PROMO_PREFIX_DIGITS	18
typedef struct SiftPromoPrefix
{
	uint8		plen;
	uint8		min_digits;		/* the width a padded number has, else 0 */
	char		prefix[SIFT_PROMO_PREFIX_MAX + 1];
} SiftPromoPrefix;

extern bool sift_prefix_derive(const SiftColIn *col, int n, SiftPromoPrefix *cx);
extern bool sift_prefix_typed(const SiftPromoPrefix *cx, const char *s, uint32 len, uint8 tag, int64 *v);
extern uint32 sift_prefix_format(const SiftPromoPrefix *cx, int64 v, char *out);
extern void sift_prefix_put_head(StringInfo s, const SiftPromoPrefix *cx);
extern Size sift_prefix_get_head(const char *b, const char *end, SiftPromoPrefix *cx);
extern bool sift_promo_prefix_infer(const SiftColIn *col, int n, const SiftDictEnc *dict, SiftColPromo *out);

/* The policy of the codec for a tier */
extern void sift_cc_policy_tier(SiftCcPolicy *pol, SiftCompressTier tier, int alg);

/* The most distinct strings a column of nn values may have for the dictionary of its part to pay (35% of them, 64 values at least, at most 1M); 0: none */
extern uint32 sift_part_dict_max(uint64 nn);

/* The dictionary of the strings of a column over the whole part: NULL when a dictionary does not pay */
extern SiftDictEnc *sift_dict_enc_build(const SiftColIn *col, int n);
extern uint32 sift_dict_code(const SiftDictEnc *d, const char *s, uint32 len);

#endif							/* SIFT_GRANULE_INT_H */
