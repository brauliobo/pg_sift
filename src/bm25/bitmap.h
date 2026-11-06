/*
 * sift_bitmap: ctid posting codec (no PostgreSQL dependency).
 *
 * A posting set is a sorted set of heap tids (block << 16 | offset, offset
 * 1..SIFT_MAX_OFF).  Heap pages are partitioned into aligned groups of 256
 * pages (gid = block >> 8).  Every operation works group by group on a
 * decoded view, sift_grp: a 256-bit page bitmap plus, for each set page in
 * page order, an offset bitmap of w bytes (bit off-1).  Invariant: every set
 * page has a non-zero offset bitmap.
 *
 * Encoded term blob (little endian, 4-byte aligned start):
 *   u8 kind | flags << 4, then
 *   LIST:    u8 ob, varint n, [u8 max ub if tf], Elias-Fano blocks of 256 postings over
 *            key = blk << ob | (off - 1): varint (x0 - previous block's last
 *            key), varint (u - 1), cnt * l low bits, high-bits bitmap of
 *            cnt + ((u - 1) >> l) bits; l = floor(log2(u / cnt))
 *   GROUPED: u8 0[3], u32 n, u32 ng, u32 key[ng] (gid << 8 | ctype), u32 poff[ng + 1],
 *            u32 base[ng + 1], payload; ctype = w | 0x80 if ARRAY, 0xE0 | index of w if PACKED
 *            BITMAP payload: u64 pages[4], popcount(pages) * w bytes
 *            ARRAY payload:  u16 (page << 8 | off - 1) if w <= 32, else
 *                            u32 (page << 16 | off - 1)
 *   tf (flags & SIFT_F_TF): GROUPED adds u8 ub[ng] (max per-posting bound) and
 *            u8 blk[ng][8]: per 16-page block, nibble q = ceil(15 * block max / ub);
 *            both add n tf nibbles
 *            (posting order, low nibble first, 1..15 saturating).
 * Singletons (df == 1) are meant to live inline in the dictionary: open them
 * with sift_cur_tids().
 *
 * Callers own all memory: no allocation inside the library.  The only global
 * state is the kernel dispatch table chosen by sift_init().
 */
#ifndef SIFT_BITMAP_H
#define SIFT_BITMAP_H

#include <stddef.h>
#include <stdint.h>

#include "core/popcount.h"

#define SIFT_GROUP_SHIFT 8
#define SIFT_GROUP_PAGES 256
#define SIFT_MAX_OFF     512
#define SIFT_LIST_OB_MAX 9    /* bits of an offset in a LIST blob: log2(SIFT_MAX_OFF) */
#define SIFT_MAX_W       64
#define SIFT_LIST_BLOCK  256
#define SIFT_LIST_SLICE  16

/* an Elias-Fano block of a LIST blob: keys x0 + [0, u), cnt postings with l low bits each */
typedef struct { uint64_t x0, u; uint32_t cnt, l; const uint8_t *lows, *highs, *end; } ef_blk;
#define SIFT_GRP_BYTES   (SIFT_GROUP_PAGES * SIFT_MAX_W)
#define SIFT_CUR_BUF     (2 * SIFT_GRP_BYTES)
#define SIFT_GRP_MAXTIDS (SIFT_GROUP_PAGES * SIFT_MAX_OFF)

typedef uint64_t sift_tid;
#define SIFT_TID(blk, off) ((uint64_t)(blk) << 16 | (off))
#define SIFT_BLK(t)        ((uint32_t)((t) >> 16))
#define SIFT_OFF(t)        ((uint32_t)((t) & 0xffff))
#define SIFT_GID(t)        ((uint32_t)((t) >> (16 + SIFT_GROUP_SHIFT)))

enum { SIFT_LIST = 1, SIFT_GROUPED = 2 };
enum { SIFT_F_TF = 1 };
/* the ranks of an architecture's levels: NEON is the first level of AArch64, as AVX2 is of x86-64 (core/cpu.h) */
enum { SIFT_ISA_SCALAR, SIFT_ISA_AVX2, SIFT_ISA_AVX512, SIFT_NISA, SIFT_ISA_NEON = SIFT_ISA_AVX2 };
/* SIFT_CT_PACKED: as AUTO, but a group that would be an ARRAY is PACKED when that is smaller (segment format 3) */
enum { SIFT_CT_AUTO, SIFT_CT_BITMAP, SIFT_CT_ARRAY, SIFT_CT_PACKED };

typedef struct {
	uint32_t gid;
	uint32_t w;          /* bytes per offset bitmap: 1, 2, 4, 8, 16, ..., 64 */
	uint32_t base;       /* rank of the group's first posting in its term */
	uint32_t n;          /* postings in the group, 0 if not known */
	uint64_t pages[4];
	const uint8_t *off;
} sift_grp;

typedef struct {
	int kind;            /* 0 = smaller of LIST/GROUPED (with bias), else forced */
	int ctype;           /* SIFT_CT_*: container choice inside GROUPED */
	double bias;         /* pick GROUPED when its size <= bias * LIST size (0 = 1.0) */
	int tf;              /* store tf nibbles (requires tf input) */
} sift_enc_opts;

typedef struct sift_cur sift_cur;
struct sift_cur {
	const sift_grp *(*seek)(sift_cur *c, uint32_t gid);
	sift_grp g;
	int state;           /* 0 unpositioned, 1 positioned, 2 exhausted */
	uint8_t *buf;        /* SIFT_CUR_BUF bytes, caller owned */
	const uint8_t *tf, *ub, *blk;  /* ub: per-group bounds, blk: 16-page block nibbles (GROUPED) */
	uint32_t ub1;               /* term bound (LIST), 255 if unknown */
	union {
		struct { const uint8_t *payload; const uint32_t *key, *poff, *base; uint32_t ng, i, plen; } gr;
		struct {
			const sift_tid *t;          /* raw tids, or NULL for a LIST blob */
			const uint8_t *p;           /* next LIST block */
			const uint8_t *end;         /* end of the blob */
			uint64_t prev, lim;         /* last key of the block before p; keys are below lim, a power of two */
			uint32_t n, i, bi, bn, w, ob;  /* postings, position, window [bi, bi + bn) of decoded postings */
			ef_blk cb;                  /* the block the window comes from: its first posting cstart, the postings decoded or passed ck, the next slice's size cw, */
			uint32_t cstart, ck, cw;
			uint64_t cpos;              /* and the bit of its high bits after the last posting decoded */
			sift_tid blk[SIFT_LIST_BLOCK];
		} ls;
		struct { sift_cur **kid; int k; } op;
	};
};

/* setup / dispatch */
int sift_init(void);                 /* picks the best supported ISA and returns it */
int sift_set_isa(int isa);           /* returns 0 if unsupported */
int sift_isa(void);
const char *sift_isa_name(int isa);

/* encoding */
size_t sift_encode_bound(uint32_t n);
/*
 * tids sorted and unique; tf (raw counts) and ub (per-posting score bound,
 * u8) optional: GROUPED stores max(ub) per group, 255 if ub is NULL.
 * out == NULL returns the exact size.
 */
size_t sift_encode(const sift_tid *tids, const uint8_t *tf, const uint8_t *ub, uint32_t n, const sift_enc_opts *o, uint8_t *out);
uint32_t sift_blob_n(const uint8_t *blob);
/* NULL when every length, count and offset the cursors follow lies inside blob[0, len); else what is wrong */
const char *sift_blob_check(const uint8_t *blob, size_t len);
int sift_blob_kind(const uint8_t *blob);
uint32_t sift_w_for(uint32_t maxoff);
uint32_t sift_tf_quant(uint32_t tf);

/*
 * Raises ERRCODE_DATA_CORRUPTED (the host provides it; it does not return) for bytes a decoder found to be damaged: a length, offset, count
 * or parameter that does not lie inside the blob it came from. Cursors check them at the granularity they read (the blob header, a group,
 * a LIST block), not per posting.
 */
extern void sift_codec_corrupt(const char *what) __attribute__((noreturn));

/*
 * cursors: seek returns the first group with gid >= target at or after the current one, NULL at end.
 * sift_cur_blob follows the lengths and offsets of blob[0, len), which it validates as it reads them (sift_blob_check is the exhaustive
 * check that sift.verify runs); the blob is followed by SIFT_KERN_PAD zero bytes.
 */
void sift_cur_blob(sift_cur *c, const uint8_t *blob, size_t len, uint8_t *buf);
void sift_cur_tids(sift_cur *c, const sift_tid *t, uint32_t n, uint8_t *buf);
void sift_cur_and(sift_cur *c, sift_cur **kid, int k, uint8_t *buf);
void sift_cur_or(sift_cur *c, sift_cur **kid, int k, uint8_t *buf);
void sift_cur_andnot(sift_cur *c, sift_cur **kid, uint8_t *buf);   /* kid[0] AND NOT kid[1] */
/* the ascending distinct gids of a LIST cursor that has not been used (out holds sift_blob_n entries); the cursor must be rewound afterwards */
uint32_t sift_cur_gids(sift_cur *c, uint32_t *out);
void sift_cur_gid_span(const sift_cur *c, uint32_t *lo, uint32_t *hi);
/* the rows (SIFT_GROUP_PAGES of w bytes, a bit for each offset) of one superblock where docs are dead; bits NULL: none */
typedef struct { const uint8_t *bits; uint32_t w; } sift_dead_sb;
/* the postings of an unused LIST cursor on a bit set in dead[gid - lo] (nsb entries), all ones for another cursor */
uint64_t sift_cur_count_in(sift_cur *c, const sift_dead_sb *dead, uint32_t lo, uint32_t nsb);
static inline const sift_grp *sift_seek(sift_cur *c, uint32_t gid) { return c->seek(c, gid); }
/* the group of exactly gid or NULL; operators do not look past gid, so a miss costs no leapfrogging */
const sift_grp *sift_seek_eq(sift_cur *c, uint32_t gid);
/* the kids of an OR cursor and their number, NULL for any other cursor */
sift_cur **sift_cur_or_kids(const sift_cur *c, int *k);
static inline const sift_grp *sift_next(sift_cur *c) { return c->seek(c, c->state ? c->g.gid + 1 : 0); }

/* whole-stream helpers */
uint64_t sift_count(sift_cur *c);                  /* 2-way AND/OR use count kernels, no materialization */
size_t sift_tids(sift_cur *c, sift_tid *out);      /* all tids in heap order */

/* group kernels (dispatched); o->off = buf, buf may equal a->off (in place) for and/andnot */
uint64_t sift_grp_count(const sift_grp *g);
uint64_t sift_grp_count_mask(const sift_grp *g, const uint64_t mask[4]);
uint32_t sift_grp_tids(const sift_grp *g, const uint64_t *mask, sift_tid *out);  /* mask may be NULL */
uint64_t sift_grp_and_count(const sift_grp *a, const sift_grp *b);
uint64_t sift_popcount(const uint8_t *p, size_t n);

/* term frequency: rank inside src's term of each posting of sub (a subset of src), in heap order */
uint32_t sift_grp_ranks(const sift_grp *src, const sift_grp *sub, uint32_t *rank);
static inline uint32_t sift_tf_at(const sift_cur *c, uint32_t rank) { return c->tf[rank >> 1] >> ((rank & 1) * 4) & 15; }
static inline uint32_t sift_grp_ub(const sift_cur *c) { return c->ub ? c->ub[c->gr.i] : c->ub1; }
/* bound of 16-page block b (0..15) of the current group, in ub units (0..255) */
static inline float sift_blk_ub(const sift_cur *c, int b)
{
	return c->blk ? (float)c->ub[c->gr.i] * (float)(c->blk[c->gr.i * 8 + b / 2] >> (b & 1) * 4 & 15) / 15.0f : (float)sift_grp_ub(c);
}

/*
 * Chunk API: one term x one 256-page group as a
 * standalone container: u32 (count << 8 | ctype), then the GROUPED payload.
 * Canonical masks: u64[4] page mask, u64[8] offset mask per page.
 */
#ifndef SIFT_PAGEMASK_WORDS		/* core/sift.h defines it too, as SIFT_SB_PAGES / 64 */
#define SIFT_PAGEMASK_WORDS 4
#endif
#define SIFT_OFFMASK_WORDS  8
size_t sift_chunk_bound(uint32_t npages);   /* for SIFT_CT_AUTO / SIFT_CT_BITMAP */
size_t sift_chunk_encode(const uint64_t pagemask[4], const uint64_t *offmasks, int ctype, uint8_t *out);
const char *sift_chunk_check(const uint8_t *chunk, uint32_t len);   /* NULL, or what is wrong; the readers below take no length: check first */
uint32_t sift_chunk_count(const uint8_t *chunk);
uint32_t sift_chunk_width(const uint8_t *chunk);   /* bytes of offsets of a page, the same for every page */
void sift_chunk_pagemask(const uint8_t *chunk, uint64_t pagemask[4]);
uint32_t sift_chunk_offsets(const uint8_t *chunk, const uint64_t pagesel[4], uint64_t *offmasks);  /* returns pages written */
void sift_chunk_grp(const uint8_t *chunk, uint32_t gid, sift_grp *g, uint8_t *buf);              /* zero-copy for bitmaps */
size_t sift_keylist_encode(const sift_tid *t, uint32_t n, uint8_t *out);                          /* a LIST blob */
uint32_t sift_keylist_decode(const uint8_t *blob, size_t len, sift_tid *out);

/* canonical offset-mask kernels over npages masks of 8 words; o may alias a or b */
void sift_offmask_and(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t npages);
void sift_offmask_or(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t npages);
void sift_offmask_andnot(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t npages);
uint64_t sift_offmask_popcount(const uint64_t *a, size_t npages);

#endif
