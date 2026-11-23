/*
 * agg/kern6_hash.c - the hash of the bytes of a wide group key or a string (agg/kern6.h).
 *
 * The bytes are read as 8-byte words (the last one zero padded): the first word seeds one CRC32C lane, the others go to three lanes in turn (24 bytes a round, three CRC instructions that do not wait for
 * one another), and the three 32-bit lanes and the length are mixed by sift_aggk6_hash1. The value is that of the function, whatever runs it: the CRC32C instruction where the CPU has it (SSE4.2 on x86-64,
 * the function that holds it is picked at run time, the rest of the library stays baseline), else the same CRC32C by tables (AArch64 stays on the tables: its baseline check allows no CRC32 instruction).
 */
#include "postgres.h"

#include "agg/kern6.h"

#if defined(__x86_64__)
#include <nmmintrin.h>
#define SIFT_CRC_X86	1
#define SIFT_CRC_TARGET	__attribute__((target("sse4.2")))
#define CRC_HW(c, w)	((uint32) _mm_crc32_u64((c), (w)))
#endif

#define HASH_S0		UINT32_C(0x9E3779B9)
#define HASH_S1		UINT32_C(0x85EBCA6B)
#define HASH_S2		UINT32_C(0xC2B2AE35)

static uint32 crc_tab[8][256];
static bool crc_tab_ready;

static void
crc_tab_init(void)
{
	for (uint32 i = 0; i < 256; i++)
	{
		uint32		c = i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ UINT32_C(0x82F63B78) : c >> 1;
		crc_tab[0][i] = c;
	}
	for (uint32 i = 0; i < 256; i++)
		for (int t = 1; t < 8; t++)
			crc_tab[t][i] = (crc_tab[t - 1][i] >> 8) ^ crc_tab[0][crc_tab[t - 1][i] & 0xff];
	crc_tab_ready = true;
}

static inline uint32
crc_sw(uint32 c, uint64 w)
{
	w ^= c;
	return crc_tab[7][w & 0xff] ^ crc_tab[6][(w >> 8) & 0xff] ^ crc_tab[5][(w >> 16) & 0xff] ^ crc_tab[4][(w >> 24) & 0xff] ^
		crc_tab[3][(w >> 32) & 0xff] ^ crc_tab[2][(w >> 40) & 0xff] ^ crc_tab[1][(w >> 48) & 0xff] ^ crc_tab[0][w >> 56];
}

static inline uint64
word_at(const uint8 *p, uint32 n)
{
	uint64		w = 0;

	memcpy(&w, p, Min(n, 8));
	return w;
}

/* w0, the first word of the key, and the rest bytes after it, of a key of total bytes */
#define HASH_CORE(NAME, ATTR, CRC) \
	ATTR static inline __attribute__((always_inline)) uint64 \
	NAME(uint64 w0, const uint8 *r, uint32 nrest, uint32 total) \
	{ \
		uint32		c0 = CRC(HASH_S0, w0), \
					c1 = HASH_S1, \
					c2 = HASH_S2; \
		for (; nrest >= 24; nrest -= 24, r += 24) \
		{ \
			uint64		a, b, c; \
			memcpy(&a, r, 8); \
			memcpy(&b, r + 8, 8); \
			memcpy(&c, r + 16, 8); \
			c1 = CRC(c1, a); \
			c2 = CRC(c2, b); \
			c0 = CRC(c0, c); \
		} \
		if (nrest > 0) \
			c1 = CRC(c1, word_at(r, nrest)); \
		if (nrest > 8) \
			c2 = CRC(c2, word_at(r + 8, nrest - 8)); \
		if (nrest > 16) \
			c0 = CRC(c0, word_at(r + 16, nrest - 16)); \
		return sift_aggk6_hash1(((uint64) c1 << 32 | c0) ^ ((uint64) c2 * UINT64CONST(0x9E3779B97F4A7C15)) ^ total); \
	}

HASH_CORE(core_sw, , crc_sw)
#ifdef SIFT_CRC_TARGET
HASH_CORE(core_hw, SIFT_CRC_TARGET, CRC_HW)
#endif

static inline bool
have_crc(void)
{
#if defined(SIFT_CRC_X86)
	static int	has = -1;

	if (has < 0)
	{
		__builtin_cpu_init();
		has = __builtin_cpu_supports("sse4.2") != 0;
	}
	return has;
#else
	return false;
#endif
}

/* the first word of the bytes p of len: the length of a key with a length in front takes the first 4 bytes of it */
#define BYTES_HASH(CORE, p, len) CORE(word_at((p), (len)), (const uint8 *) (p) + 8, (len) > 8 ? (len) - 8 : 0, (len))
#define LENPFX_HASH(CORE, p, len) CORE((uint64) (len) | (word_at((p), Min((len), 4)) << 32), (const uint8 *) (p) + 4, (len) > 4 ? (len) - 4 : 0, (len) + 4)

static inline const uint8 *
view_bytes(const SiftStrView *v)
{
	return v->len <= 12 ? (const uint8 *) v + offsetof(SiftStrView, prefix) : (const uint8 *) v->u.ptr;
}

#define HASH_FUNCS(SUFFIX, ATTR, CORE) \
	ATTR static uint64 \
	bytes_##SUFFIX(const void *p, uint32 len) \
	{ \
		return BYTES_HASH(CORE, p, len); \
	} \
	ATTR static uint64 \
	lenpfx_##SUFFIX(const void *p, uint32 len) \
	{ \
		return LENPFX_HASH(CORE, p, len); \
	} \
	ATTR static void \
	hash_wide_##SUFFIX(uint64 *h, const uint8 *buf, const uint32 *off, const uint32 *len, const uint64 *nulls, uint16 n) \
	{ \
		for (uint16 k = 0; k < n; k++) \
			h[k] = sift_aggk6_hash1(BYTES_HASH(CORE, buf + off[k], len[k]) ^ nulls[k]); \
	} \
	ATTR static void \
	hash_str_##SUFFIX(uint64 *h, const SiftStrView *v, const uint64 *valid, const uint16 *sel, uint16 n) \
	{ \
		for (uint16 k = 0; k < n; k++) \
		{ \
			const SiftStrView *s = &v[sel[k]]; \
			if (valid == NULL || ((valid[sel[k] >> 6] >> (sel[k] & 63)) & 1)) \
				h[k] = sift_aggk6_hash1(LENPFX_HASH(CORE, view_bytes(s), s->len)); \
		} \
	}

HASH_FUNCS(sw, , core_sw)
#ifdef SIFT_CRC_TARGET
HASH_FUNCS(hw, SIFT_CRC_TARGET, core_hw)
#endif

uint64
sift_aggk6_hash_bytes(const void *p, uint32 len)
{
#ifdef SIFT_CRC_TARGET
	if (have_crc())
		return bytes_hw(p, len);
#endif
	if (!crc_tab_ready)
		crc_tab_init();
	return bytes_sw(p, len);
}

uint64
sift_aggk6_hash_lenpfx(const void *p, uint32 len)
{
#ifdef SIFT_CRC_TARGET
	if (have_crc())
		return lenpfx_hw(p, len);
#endif
	if (!crc_tab_ready)
		crc_tab_init();
	return lenpfx_sw(p, len);
}

void
sift_aggk6_hash_wide_batch(uint64 *h, const uint8 *buf, const uint32 *off, const uint32 *len, const uint64 *nulls, uint16 n)
{
#ifdef SIFT_CRC_TARGET
	if (have_crc())
		return hash_wide_hw(h, buf, off, len, nulls, n);
#endif
	if (!crc_tab_ready)
		crc_tab_init();
	hash_wide_sw(h, buf, off, len, nulls, n);
}

void
sift_aggk6_hash_str_batch(uint64 *h, const SiftStrView *v, const uint64 *valid, const uint16 *sel, uint16 n)
{
#ifdef SIFT_CRC_TARGET
	if (have_crc())
		return hash_str_hw(h, v, valid, sel, n);
#endif
	if (!crc_tab_ready)
		crc_tab_init();
	hash_str_sw(h, v, valid, sel, n);
}
