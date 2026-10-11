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
 *   u8 kind, u8 flags, u8 w, u8 0, u32 n
 *   LIST:    blocks of 128 postings: u32 blk, u16 off, u8 bb, u8 ob, then
 *            cnt-1 fields of bb+ob bits ((blk delta << ob) | (off - 1))
 *   GROUPED: u32 ng, u32 key[ng] (gid << 8 | ctype), u32 poff[ng + 1],
 *            u32 base[ng + 1], payload; ctype = w | 0x80 if ARRAY
 *            BITMAP payload: u64 pages[4], popcount(pages) * w bytes
 *            ARRAY payload:  u16 (page << 8 | off - 1) if w <= 32, else
 *                            u32 (page << 16 | off - 1)
 *   tf (flags & SIFT_F_TF): GROUPED adds u8 maxtf[ng]; both add n tf nibbles
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

#define SIFT_GROUP_SHIFT 8
#define SIFT_GROUP_PAGES 256
#define SIFT_MAX_OFF     512
#define SIFT_MAX_W       64
#define SIFT_LIST_BLOCK  128
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
	const uint8_t *tf, *maxtf;
	union {
		struct { const uint8_t *payload; const uint32_t *key, *poff, *base; uint32_t ng, i; } gr;
		struct {
			const sift_tid *t;          /* raw tids, or NULL for a LIST blob */
			const uint8_t *p;           /* next LIST block */
			uint32_t n, i, bi, bn, w;   /* postings, position, block window [bi, bi + bn) */
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
/* tids sorted and unique, tf optional (raw counts); out == NULL returns the exact size */
size_t sift_encode(const sift_tid *tids, const uint8_t *tf, uint32_t n, const sift_enc_opts *o, uint8_t *out);
uint32_t sift_blob_n(const uint8_t *blob);
int sift_blob_kind(const uint8_t *blob);
uint32_t sift_w_for(uint32_t maxoff);
uint32_t sift_tf_quant(uint32_t tf);

/* cursors: seek returns the first group with gid >= target at or after the current one, NULL at end */
void sift_cur_blob(sift_cur *c, const uint8_t *blob, uint8_t *buf);
void sift_cur_tids(sift_cur *c, const sift_tid *t, uint32_t n, uint8_t *buf);
void sift_cur_and(sift_cur *c, sift_cur **kid, int k, uint8_t *buf);
void sift_cur_or(sift_cur *c, sift_cur **kid, int k, uint8_t *buf);
void sift_cur_andnot(sift_cur *c, sift_cur **kid, uint8_t *buf);   /* kid[0] AND NOT kid[1] */
static inline const sift_grp *sift_seek(sift_cur *c, uint32_t gid) { return c->seek(c, gid); }
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
static inline uint32_t sift_grp_maxtf(const sift_cur *c) { return c->maxtf ? c->maxtf[c->gr.i] : 15; }

#endif
