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
 * Encoded term blob (little endian, 4-byte aligned start), see docs/CODEC.md:
 *   u8 kind | flags << 4, then
 *   LIST:    u8 ob, varint n, [u8 max ub if tf], Elias-Fano blocks of 256 postings over
 *            key = blk << ob | (off - 1): varint (x0 - previous block's last
 *            key), varint (u - 1), cnt * l low bits, high-bits bitmap of
 *            cnt + ((u - 1) >> l) bits; l = floor(log2(u / cnt))
 *   GROUPED: u8 0[3], u32 n, u32 ng, u32 key[ng] (gid << 8 | ctype), u32 poff[ng + 1],
 *            u32 base[ng + 1], payload; ctype = w | 0x80 if ARRAY
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

#include "sift_popcount.h"

#define SIFT_GROUP_SHIFT 8
#define SIFT_GROUP_PAGES 256
#define SIFT_MAX_OFF     512
#define SIFT_MAX_W       64
#define SIFT_LIST_BLOCK  256
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
enum { SIFT_ISA_SCALAR, SIFT_ISA_AVX2, SIFT_ISA_AVX512, SIFT_NISA };
enum { SIFT_CT_AUTO, SIFT_CT_BITMAP, SIFT_CT_ARRAY };

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
		struct { const uint8_t *payload; const uint32_t *key, *poff, *base; uint32_t ng, i; } gr;
		struct {
			const sift_tid *t;          /* raw tids, or NULL for a LIST blob */
			const uint8_t *p;           /* next LIST block */
			uint64_t prev;              /* last key of the block before p */
			uint32_t n, i, bi, bn, w, ob;  /* postings, position, block window [bi, bi + bn) */
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

/* cursors: seek returns the first group with gid >= target at or after the current one, NULL at end */
void sift_cur_blob(sift_cur *c, const uint8_t *blob, uint8_t *buf);
void sift_cur_tids(sift_cur *c, const sift_tid *t, uint32_t n, uint8_t *buf);
void sift_cur_and(sift_cur *c, sift_cur **kid, int k, uint8_t *buf);
void sift_cur_or(sift_cur *c, sift_cur **kid, int k, uint8_t *buf);
void sift_cur_andnot(sift_cur *c, sift_cur **kid, uint8_t *buf);   /* kid[0] AND NOT kid[1] */
/* the ascending distinct gids of a LIST cursor that has not been used (out holds sift_blob_n entries); the cursor must be rewound afterwards */
uint32_t sift_cur_gids(sift_cur *c, uint32_t *out);
static inline const sift_grp *sift_seek(sift_cur *c, uint32_t gid) { return c->seek(c, gid); }
/* the group of exactly gid or NULL; operators do not look past gid, so a miss costs no leapfrogging */
const sift_grp *sift_seek_eq(sift_cur *c, uint32_t gid);
static inline const sift_grp *sift_next(sift_cur *c) { return c->seek(c, c->state ? c->g.gid + 1 : 0); }

/* whole-stream helpers */
uint64_t sift_count(sift_cur *c);                  /* 2-way AND/OR use count kernels, no materialization */
size_t sift_tids(sift_cur *c, sift_tid *out);      /* all tids in heap order */

/* group kernels (dispatched); o->off = buf, buf may equal a->off (in place) for and/andnot */
uint64_t sift_grp_count(const sift_grp *g);
uint64_t sift_grp_count_mask(const sift_grp *g, const uint64_t mask[4]);
uint32_t sift_grp_tids(const sift_grp *g, const uint64_t *mask, sift_tid *out);  /* mask may be NULL */
void sift_grp_and(const sift_grp *a, const sift_grp *b, const uint64_t *mask, sift_grp *o, uint8_t *buf);
void sift_grp_or(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf);
void sift_grp_andnot(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf);
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
 * Chunk API (DESIGN.md contract 6): one term x one 256-page group as a
 * standalone container: u32 (count << 8 | ctype), then the GROUPED payload.
 * Canonical masks: u64[4] page mask, u64[8] offset mask per page.
 */
#define SIFT_PAGEMASK_WORDS 4
#define SIFT_OFFMASK_WORDS  8
size_t sift_chunk_bound(uint32_t npages);   /* for SIFT_CT_AUTO / SIFT_CT_BITMAP */
size_t sift_chunk_encode(const uint64_t pagemask[4], const uint64_t *offmasks, int ctype, uint8_t *out);
uint32_t sift_chunk_count(const uint8_t *chunk);
void sift_chunk_pagemask(const uint8_t *chunk, uint64_t pagemask[4]);
uint32_t sift_chunk_offsets(const uint8_t *chunk, const uint64_t pagesel[4], uint64_t *offmasks);  /* returns pages written */
void sift_chunk_grp(const uint8_t *chunk, uint32_t gid, sift_grp *g, uint8_t *buf);              /* zero-copy for bitmaps */
size_t sift_keylist_encode(const sift_tid *t, uint32_t n, uint8_t *out);                          /* a LIST blob */
uint32_t sift_keylist_decode(const uint8_t *blob, sift_tid *out);

/* canonical offset-mask kernels over npages masks of 8 words; o may alias a or b */
void sift_offmask_and(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t npages);
void sift_offmask_or(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t npages);
void sift_offmask_andnot(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t npages);
uint64_t sift_offmask_popcount(const uint64_t *a, size_t npages);

/*
 * Fused BM25 over one result group.  Exact float32 scores with a fixed
 * operation order (build with -ffp-contract=off): for each term, in order,
 * score += w * (tf / (tf + K(dl))), w = boost * idf * (k1 + 1).  K(dl) comes
 * from a segment table indexed by tuple slot: knorm[rowstart[blk - blk0] + off - 1].
 */
typedef struct {
	const uint32_t *rowstart;
	const float *knorm;
	uint32_t blk0;
} sift_norms;

typedef struct {
	sift_cur *cur;       /* source cursor over a blob with tf (SIFT_F_TF) */
	float w;
	int rel;             /* SIFT_REL_*: how the term relates to the result group */
} sift_qterm;
enum { SIFT_REL_ANY, SIFT_REL_SUPERSET /* AND operand */, SIFT_REL_SUBSET /* OR operand, no filter */ };

#define SIFT_SCORE_BUF ((size_t)SIFT_GRP_MAXTIDS * 20 + SIFT_CUR_BUF)
/* scores and tids of res in heap order; terms whose cursor is not on res->gid contribute 0 */
uint32_t sift_grp_score(const sift_grp *res, const sift_qterm *t, int nt, const sift_norms *nm, float *score, sift_tid *tids, uint8_t *buf);

/*
 * Top-k: document-at-a-time over groups with group-level score bounds.
 * A group whose bound sum_t w_t * ub_t / 255 is below the current k-th
 * score is skipped before any offset bitmap or tf is decoded.  filter is an
 * optional extra AND operand (predicate bitmap, liveness, ...).
 */
typedef struct { sift_tid tid; float score; } sift_hit;
typedef struct { uint64_t groups, skipped, scored; } sift_topk_stats;
typedef struct {
	sift_qterm *t;       /* rel is set by sift_topk */
	int nt, conj;        /* conj: AND of all terms, else OR; sets every term's rel */
	sift_cur *filter;
	const sift_norms *nm;
	uint32_t k;
	int exhaustive;      /* disable bound skipping (reference runs) */
} sift_topk_q;
#define SIFT_TOPK_BUF (SIFT_SCORE_BUF + (size_t)SIFT_GRP_MAXTIDS * 12 + 2 * SIFT_CUR_BUF)
uint32_t sift_topk(const sift_topk_q *q, sift_hit *hits, uint8_t *buf, sift_topk_stats *st);  /* hits sorted best first */

/* Facets: counts[v] += |q AND val[v]| for every value bitmap, streaming q once */
void sift_facets(sift_cur *q, sift_cur **val, int nv, uint64_t *counts);

#endif
