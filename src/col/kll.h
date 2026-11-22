/*
 * col/kll.h - the KLL quantile sketch of sift.approx_percentile and sift.approx_quantiles, over float8 values, and the SEC_KLL (0x020C) section of a part of format major 2.
 * Level h holds items of weight 2^h; a full level keeps every second item (chosen by a seeded generator the sketch owns) and hands them to level h + 1, so the weights always add up to n
 * and a sketch of k = 200 answers any quantile within about 1% of the rank. The answer is a function of the values, the order they came in and the seed, so a stored sketch
 * and a rebuilt one agree. NULL and NaN are not values; -0 is 0.
 *
 * A stored sketch (every integer little endian): a 56-byte header, the item count of each level (u16), padding to 8, then the items of each level, ascending, as float8.
 * A section is a 16-byte header, a directory of 24-byte entries sorted by (column, granule) and the sketches, each at a multiple of 8.
 */
#ifndef SIFT_COL_KLL_H
#define SIFT_COL_KLL_H

#include "core/host.h"

#define SIFT_KLL_MAGIC			0x4B4C4C31U		/* "KLL1" */
#define SIFT_KLL_VERSION		1
#define SIFT_KLL_K_MIN			64
#define SIFT_KLL_K_DEFAULT		200
#define SIFT_KLL_K_MAX			1024
#define SIFT_KLL_MIN_LEVEL_CAP	8
#define SIFT_KLL_MAX_LEVELS		48
#define SIFT_KLL_WIRE_HDR		56
#define SIFT_KLL_WIRE_MAX		(SIFT_KLL_WIRE_HDR + 2 * SIFT_KLL_MAX_LEVELS + 8 + 8 * (3 * SIFT_KLL_K_MAX + SIFT_KLL_MIN_LEVEL_CAP * SIFT_KLL_MAX_LEVELS))

#define SIFT_KLL_SEC_MAGIC		0x31534C4BU		/* "KLS1" */
#define SIFT_KLL_SEC_VERSION	1
#define SIFT_KLL_SEC_HDR		16
#define SIFT_KLL_SEC_ENTRY		24
#define SIFT_KLL_SEC_MAX_ENTRIES 4096
#define SIFT_KLL_WHOLE_PART		UINT32_MAX		/* the granule of an entry that sketches the column of the whole part */

typedef struct SiftKll
{
	MemoryContext cxt;
	uint16		k;
	uint8		nlevels;
	bool		l0_sorted;
	uint64		n;				/* values ingested, the sum of the weights of the items */
	double		min;			/* exact */
	double		max;
	uint64		rng;
	uint64		flips;			/* bit h: the parity level h keeps next; it flips at each compaction of the level, so their errors cancel */
	uint32		cap;			/* length of items */
	uint32		tcap;			/* items kept before a compaction: the sum of the capacities of the levels */
	uint32		tmp_cap;
	double	   *tmp;			/* the items a compaction hands up */
	uint32		lvl[SIFT_KLL_MAX_LEVELS + 1];	/* level h is items[lvl[h] .. lvl[h + 1]); the used items end at the end of the array */
	double	   *items;
} SiftKll;

/* false: k outside SIFT_KLL_K_MIN..SIFT_KLL_K_MAX, nothing initialised */
extern bool sift_kll_init(SiftKll *s, uint16 k, MemoryContext cxt);
extern bool sift_kll_init_seeded(SiftKll *s, uint16 k, uint64 seed, MemoryContext cxt);
extern void sift_kll_free(SiftKll *s);

/* false: NaN, not a value */
extern bool sift_kll_add(SiftKll *s, double v);
/* The values of vals that are not NULL (isnull may be NULL: none is) and not NaN; returns how many were added */
extern uint32 sift_kll_add_batch(SiftKll *s, const double *vals, const bool *isnull, uint32 n);
/* src into dst, which keeps its k and its generator; src is not changed */
extern void sift_kll_merge(SiftKll *dst, const SiftKll *src);
/* A stored sketch into dst; false (dst unchanged) when it is not well formed */
extern bool sift_kll_merge_wire(SiftKll *dst, const uint8 *blob, uint32 len);

/* Whether dirty rows of rows are at most ppm per million, in integers: the share of the rows a sketch may miss (dead, pending, not in a part) and still answer */
extern bool sift_kll_dirty_ok(uint64 rows, uint64 dirty, uint32 ppm);

/* The weight of the items: n by construction, which a test asserts */
extern uint64 sift_kll_weight(const SiftKll *s);
/* The items kept, at most 3 k + 8 * nlevels */
extern uint32 sift_kll_size(const SiftKll *s);

/* The least value whose share of the values (itself included) is at least p, the exact minimum for p = 0 and the exact maximum for p = 1; false for an empty sketch or a p outside [0, 1] */
extern bool sift_kll_quantile(const SiftKll *s, double p, double *out);
extern bool sift_kll_quantiles(const SiftKll *s, const double *ps, uint32 np, double *out);
/* The share of the values that are <= x: 0 below the minimum, 1 from the maximum; false for an empty sketch or NaN */
extern bool sift_kll_rank(const SiftKll *s, double x, double *out);
/* The ranks of the split points, ascending and without NaN; false otherwise */
extern bool sift_kll_cdf(const SiftKll *s, const double *splits, uint32 ns, double *out);

/* The stored form: its size, and its bytes in out (0 when cap is less); the encoding of equal sketches is equal */
extern uint32 sift_kll_wire_size(const SiftKll *s);
extern uint32 sift_kll_encode(const SiftKll *s, uint8 *out, uint32 cap);
/* Whether the bytes are well formed: sizes, checksum, ranges, ascending levels, the weights adding up to n, the items within the bound of memory */
extern bool sift_kll_check(const uint8 *blob, uint32 len);
extern bool sift_kll_decode(SiftKll *s, const uint8 *blob, uint32 len, MemoryContext cxt);
extern uint32 sift_kll_crc32c(const void *data, uint32 len);

typedef struct SiftKllSecIn
{
	uint16		col;
	uint32		granule;
	SiftKll    *sketch;
} SiftKllSecIn;

/* The section of the entries (ascending by column, then granule; an entry of an empty sketch says the column was sketched and had no value), allocated in cxt; NULL for no entry */
extern uint8 *sift_kll_sec_build(const SiftKllSecIn *in, uint32 n, MemoryContext cxt, uint32 *len);
extern bool sift_kll_sec_check(const uint8 *sec, uint32 len);
/* The stored sketch of (col, granule) of a checked section, and its value count */
extern bool sift_kll_sec_find(const uint8 *sec, uint32 len, uint16 col, uint32 granule, const uint8 **blob, uint32 *bloblen, uint64 *n);
extern uint32 sift_kll_sec_entries(const uint8 *sec);

/* The option sketches = 'hll(a, b); kll(c, d(k=100))': the columns of its kll groups, by name (pointers into the option, not terminated) */
typedef struct SiftKllSpecCol
{
	const char *name;
	uint32		len;
	uint16		k;				/* of a kll column */
	bool		kll;			/* false: a column of an hll group */
} SiftKllSpecCol;

/* NULL when the option is well formed; else what is wrong. cols may be NULL; *n counts the columns of both kinds, at most cap are stored */
extern const char *sift_kll_spec_parse(const char *opt, SiftKllSpecCol *cols, uint32 cap, uint32 *n);

#endif							/* SIFT_COL_KLL_H */
