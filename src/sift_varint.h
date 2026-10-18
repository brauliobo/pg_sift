/*
 * sift_varint.h - LEB128 varints and zigzag, the one codec of the byte formats (posting positions, term blocks, bitmap lists,
 * mutable row records). Portable (C library only).
 */
#ifndef SIFT_VARINT_H
#define SIFT_VARINT_H

#include <stdint.h>

/* Writes x at p (at most 10 bytes); returns the end */
static inline uint8_t *
sift_put_varint(uint8_t *p, uint64_t x)
{
	for (; x >= 0x80; x >>= 7)
		*p++ = (uint8_t) (x | 0x80);
	*p++ = (uint8_t) x;
	return p;
}

/* Reads a varint known to be complete; returns the end */
static inline const uint8_t *
sift_get_varint(const uint8_t *p, uint64_t *x)
{
	*x = 0;
	for (int s = 0;; s += 7)
	{
		*x |= (uint64_t) (*p & 0x7f) << s;
		if (!(*p++ & 0x80))
			return p;
	}
}

/* Reads a varint of at most maxshift / 7 + 1 bytes from [p, end); NULL when it runs past end or is longer */
static inline const uint8_t *
sift_get_varint_upto(const uint8_t *p, const uint8_t *end, uint64_t *x, int maxshift)
{
	*x = 0;
	for (int s = 0; p < end && s < maxshift; s += 7)
	{
		*x |= (uint64_t) (*p & 0x7f) << s;
		if (!(*p++ & 0x80))
			return p;
	}
	return NULL;
}

/* A varint of an unsigned value of at most 63 bits (posting lists: keys and counts) */
static inline const uint8_t *
sift_get_varint_n(const uint8_t *p, const uint8_t *end, uint64_t *x)
{
	return sift_get_varint_upto(p, end, x, 63);
}

/* A varint of any 64 bit value (zigzag of a signed integer takes ten bytes at the extremes) */
static inline const uint8_t *
sift_get_varint64_n(const uint8_t *p, const uint8_t *end, uint64_t *x)
{
	return sift_get_varint_upto(p, end, x, 64);
}

/* Signed integers as unsigned ones that stay small near zero */
static inline uint64_t
sift_zigzag(int64_t v)
{
	return ((uint64_t) v << 1) ^ (uint64_t) (v >> 63);
}

static inline int64_t
sift_unzigzag(uint64_t z)
{
	return (int64_t) (z >> 1) ^ -(int64_t) (z & 1);
}

#endif							/* SIFT_VARINT_H */
