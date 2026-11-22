/*
 * col/scope_v6/wire.h - the byte forms of the scope summary: varints, zigzag, a bounded reader. Private to col/scope_v6/. Portable.
 */
#ifndef SIFT_COL_SCOPE_V6_WIRE_H
#define SIFT_COL_SCOPE_V6_WIRE_H

#include "core/host.h"

typedef unsigned __int128 SiftU128;

/* A reader over a byte range; a read past the end sets bad and returns zeros, the caller checks bad once */
typedef struct WireCur
{
	const char *p;
	const char *end;
	bool		bad;
} WireCur;

static inline void
wire_cur(WireCur *c, const char *p, uint64 len)
{
	c->p = p;
	c->end = p + len;
	c->bad = false;
}

static inline uint64
wire_zig(int64 v)
{
	return ((uint64) v << 1) ^ (uint64) (v >> 63);
}

static inline int64
wire_unzig(uint64 v)
{
	return (int64) (v >> 1) ^ -(int64) (v & 1);
}

static inline SiftU128
wire_zig128(__int128 v)
{
	return ((SiftU128) v << 1) ^ (SiftU128) (v >> 127);
}

static inline __int128
wire_unzig128(SiftU128 v)
{
	return (__int128) (v >> 1) ^ -(__int128) (v & 1);
}

static inline void
wire_put_u8(StringInfo out, uint8 v)
{
	appendStringInfoChar(out, (char) v);
}

static inline void
wire_put_var(StringInfo out, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(out, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(out, (char) v);
}

static inline void
wire_put_var128(StringInfo out, SiftU128 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(out, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(out, (char) v);
}

static inline uint8
wire_get_u8(WireCur *c)
{
	if (c->p >= c->end)
	{
		c->bad = true;
		return 0;
	}
	return (uint8) *c->p++;
}

static inline uint64
wire_get_var(WireCur *c)
{
	uint64		v = 0;

	for (int shift = 0; shift < 70; shift += 7)
	{
		uint8		b = wire_get_u8(c);

		if (shift < 64)
			v |= (uint64) (b & 0x7F) << shift;
		if (!(b & 0x80))
			return v;
		if (c->bad)
			return 0;
	}
	c->bad = true;
	return 0;
}

static inline SiftU128
wire_get_var128(WireCur *c)
{
	SiftU128	v = 0;

	for (int shift = 0; shift < 133; shift += 7)
	{
		uint8		b = wire_get_u8(c);

		if (shift < 128)
			v |= (SiftU128) (b & 0x7F) << shift;
		if (!(b & 0x80))
			return v;
		if (c->bad)
			return 0;
	}
	c->bad = true;
	return 0;
}

static inline void
wire_get_bytes(WireCur *c, void *dst, Size n)
{
	if ((Size) (c->end - c->p) < n)
	{
		c->bad = true;
		memset(dst, 0, n);
		return;
	}
	memcpy(dst, c->p, n);
	c->p += n;
}

static inline void
wire_pad8(StringInfo out)
{
	while (out->len % 8 != 0)
		appendStringInfoChar(out, '\0');
}

#endif							/* SIFT_COL_SCOPE_V6_WIRE_H */
