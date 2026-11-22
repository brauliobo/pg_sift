/*
 * tests/vk_v6_common.h - what the test (vk_v6_test.c) and the benchmark (vk_v6_bench.c) share: logical data of granules, the builder of the
 * chunks (the real column codec writes them), a tiny assembler of programs, and the scalar oracle that computes what a job must give.
 */
#ifndef VK_V6_COMMON_H
#define VK_V6_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk.h"
#include "agg/vk/layout.h"

typedef __int128 i128;

/* ---- the generator ---- */
typedef struct Rng { uint64_t s; } Rng;

static inline uint64_t
rng_next(Rng *r)
{
	uint64_t	x = r->s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return r->s = x;
}

static inline int64_t
rng_range(Rng *r, int64_t lo, int64_t hi)
{
	return lo + (int64_t) (rng_next(r) % (uint64_t) (hi - lo + 1));
}

static inline double
rng_unit(Rng *r)
{
	return (double) (rng_next(r) >> 11) / 9007199254740992.0;
}

/* the order-preserving key of a double (col/api/leaf.h) */
static inline int64_t
f8_key(double f)
{
	uint64_t	b;

	if (f != f)
		b = UINT64_C(0x7FF8000000000000);
	else
		memcpy(&b, &f, 8);
	return (int64_t) (b ^ ((uint64_t) ((int64_t) b >> 63) >> 1));
}

static inline double
f8_from_key(int64_t key)
{
	uint64_t	b = (uint64_t) key ^ ((uint64_t) (key >> 63) >> 1);
	double		f;

	memcpy(&f, &b, 8);
	return f;
}

typedef struct TCol
{
	int64_t    *v;				/* the value of each row (a double: its key); 0 where not valid */
	uint8_t    *ok;				/* 1 valid */
} TCol;

typedef struct TGran
{
	uint32_t	id,
				nrows;
	uint32_t	dead[256];
	bool		has_dead;
	TCol		col[SIFT_VK_MAXCOLS];
	SiftVkChunk chunk[SIFT_VK_MAXCOLS];
	bool		used[SIFT_VK_MAXCOLS];
	void	   *buf[SIFT_VK_MAXCOLS];
	char		desc[SIFT_VK_MAXCOLS][64];
} TGran;

typedef struct TData
{
	uint32_t	ngran,
				ncols;
	TGran	   *g;
	SiftVkGranule *vg;
} TData;

enum
{
	NULLS_AUTO,					/* all valid, else a bitmap */
	NULLS_BITMAP,
	NULLS_SPARSE_NULL,			/* a list of the rows that are null */
	NULLS_SPARSE_VALUE			/* a list of the rows that hold a value */
};

typedef struct ChunkSpec
{
	const char *policy;			/* of the codec: auto, none, for, delta, rle, dict, scale, alp, "rle+lz4", ... */
	uint32_t	enc;			/* SIFT_VK_ENC_* */
	int			nulls;
	bool		lz4;			/* the record is wrapped in an LZ4 block */
	int			tz;				/* ALP_RD: the low zero bits dropped */
	bool		w0;				/* a column of one value is held as the constant base (w = 0) */
	const char *why;			/* when the chunk could not be built */
} ChunkSpec;

void td_init(TData *d, uint32_t ngran, uint32_t ncols, uint32_t rows);
void td_free(TData *d);
void td_publish(TData *d);
/* builds the chunk of column c of granule g from its values with the real codec; false (spec->why) when the data do not suit the encoding */
bool build_chunk(TGran *g, uint32_t c, const ChunkSpec *spec, uint32_t part_id);
void codec_init(void);
/* what the codec's own decoder gives for a chunk: the value of each row and its validity (the oracle of the decoder of the device) */
bool cpu_decode(const SiftVkChunk *ch, int64_t *vals, uint8_t *valid);

/* ---- programs ---- */
typedef struct Prog
{
	uint32_t	w[64 * SIFT_VK_OP_WORDS];
	uint32_t	n;
	uint32_t	pool[8192];
	uint32_t	npool;
} Prog;

static inline void
p_op(Prog *p, uint32_t op, uint32_t ty, uint32_t tt, uint32_t fl, uint32_t dst, uint32_t a, uint32_t b, uint32_t c, uint32_t act, int scale, int32_t aux,
	 int64_t imm)
{
	uint32_t   *o = &p->w[p->n++ * SIFT_VK_OP_WORDS];

	o[0] = op | ty << 8 | tt << 16 | fl << 24;
	o[1] = dst | a << 8 | b << 16 | c << 24;
	o[2] = act | ((uint32_t) scale & 0xFFFF) << 8;
	o[3] = (uint32_t) aux;
	o[4] = (uint32_t) (uint64_t) imm;
	o[5] = (uint32_t) ((uint64_t) imm >> 32);
	o[6] = o[7] = 0;
}

#define P_COL(p, dst, col, ty)			p_op(p, SIFT_VK_OP_LOAD_COL, ty, 0, 0, dst, col, 0, 0, 0, 0, 0, 0)
#define P_CONST(p, dst, v)				p_op(p, SIFT_VK_OP_LOAD_CONST, 0, 0, 0, dst, 0, 0, 0, 0, 0, 0, v)
#define P_BIN(p, op, ty, dst, a, b, act) p_op(p, op, ty, 0, 0, dst, a, b, 0, act, 0, 0, 0)
#define P_BINC(p, op, ty, dst, a, v, act) p_op(p, op, ty, 0, 1, dst, a, 0, 0, act, 0, 0, v)
#define P_CMP(p, ty, slot, a, b, cmp)	p_op(p, SIFT_VK_OP_CMP, ty, 0, 0, slot, a, b, 0, 0, 0, cmp, 0)
#define P_CMPC(p, ty, slot, a, v, cmp)	p_op(p, SIFT_VK_OP_CMP, ty, 0, 1, slot, a, 0, 0, 0, 0, cmp, v)
#define P_MAND(p, dst, a, b)			p_op(p, SIFT_VK_OP_MASK_AND, 0, 0, 0, dst, a, b, 0, 0, 0, 0, 0)
#define P_MOR(p, dst, a, b)				p_op(p, SIFT_VK_OP_MASK_OR, 0, 0, 0, dst, a, b, 0, 0, 0, 0, 0)
#define P_MNOT(p, dst, a)				p_op(p, SIFT_VK_OP_MASK_NOT, 0, 0, 0, dst, a, 0, 0, 0, 0, 0, 0)

static inline void
p_inset(Prog *p, uint32_t slot, uint32_t reg, const int64_t *vals, uint32_t n)
{
	int64_t		lo = vals[0],
				hi = vals[0];
	uint32_t   *o;

	for (uint32_t i = 1; i < n; i++)
	{
		lo = vals[i] < lo ? vals[i] : lo;
		hi = vals[i] > hi ? vals[i] : hi;
	}
	p_op(p, SIFT_VK_OP_IN_SET, 0, 0, 0, slot, reg, 0, 0, 0, 0, 0, 0);
	o = &p->w[(p->n - 1) * SIFT_VK_OP_WORDS];
	o[6] = p->npool;
	if ((uint64_t) (hi - lo) < 4096)
	{
		uint32_t	words = (uint32_t) ((hi - lo) / 32 + 1);

		o[0] |= 1u << 24;
		o[4] = (uint32_t) (uint64_t) lo;
		o[5] = (uint32_t) ((uint64_t) lo >> 32);
		o[7] = words;
		memset(&p->pool[p->npool], 0, words * 4);
		for (uint32_t i = 0; i < n; i++)
			p->pool[p->npool + (vals[i] - lo) / 32] |= 1u << ((vals[i] - lo) % 32);
		p->npool += words;
	}
	else
	{
		int64_t    *s = malloc(n * 8);
		uint32_t	m = 0;

		memcpy(s, vals, n * 8);
		for (uint32_t i = 0; i < n; i++)
			for (uint32_t j = i + 1; j < n; j++)
				if (s[j] < s[i])
				{
					int64_t t = s[i];
					s[i] = s[j];
					s[j] = t;
				}
		for (uint32_t i = 0; i < n; i++)
			if (i == 0 || s[i] != s[i - 1])
			{
				p->pool[p->npool + 2 * m] = (uint32_t) (uint64_t) s[i];
				p->pool[p->npool + 2 * m + 1] = (uint32_t) ((uint64_t) s[i] >> 32);
				m++;
			}
		o[7] = m;
		p->npool += 2 * m;
		free(s);
	}
}

/* ---- the oracle ---- */
typedef struct RAgg
{
	int64_t		cnt;			/* the values counted (w2; for a COUNT: w0) */
	i128		sum;
	double		fsum;
	int64_t		mm;
} RAgg;

typedef struct RGroup
{
	int64_t		key[SIFT_VK_MAXKEYS];
	uint32_t	nullmask;
	int64_t		rows;
	RAgg		a[SIFT_VK_MAXAGGS];
} RGroup;

typedef struct RefResult
{
	RGroup	   *g;
	uint32_t	n,
				cap;
	bool		has_err;
	uint32_t	err_gran,
				err_row,
				err_op,
				err_code;
	uint64_t	rows_in;
} RefResult;

/* the scalar oracle of a job over the logical data; false when the job is not one it knows */
bool ref_run(const TData *d, const SiftVkJob *job, RefResult *out);
void ref_free(RefResult *r);
/* compares the result of the device with the oracle's: the number of differences (printed up to a few) */
uint64_t ref_compare(const SiftVkJob *job, const SiftVkResult *res, const RefResult *ref, const char *what, double ftol);
typedef struct RefCand
{
	int64_t		key;
	uint32_t	gran,
				row;
	bool		isnull;
} RefCand;

/* the best K candidates of the oracle for a top N job, in the order the device must give them (ties of the key in any order) */
uint32_t ref_topn(const TData *d, const SiftVkJob *job, RefCand *out, uint32_t cap);
/* the sort key of one row (to check the row a candidate names) */
bool ref_row_key(const TData *d, const SiftVkJob *job, uint32_t gran, uint32_t row, int64_t *key, bool *isnull);

#define CHECK(cond, ...) \
	do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } } while (0)

extern int	failures;

#endif
