/*
 * col/scope_v6/tstride.c - the time strides of a granule's time leaf (a minimum and a maximum for each run of 256 rows) as the bytes of
 * SEC_GRANULE_TSTRIDE: built from a leaf, and written and read delta coded and bit-packed, a granule at a time.
 */
#include "core/host.h"

#include "col/scope_v6/int.h"
#include "col/scope_v6/wire.h"

uint32
sift_tstride_build(const int64 *time, const uint8 *time_null, uint32 nrows, SiftTimeStride *out)
{
	uint32		ns = (nrows + SIFT_TSTRIDE_ROWS - 1) / SIFT_TSTRIDE_ROWS;

	for (uint32 s = 0; s < ns; s++)
	{
		SiftTimeStride t = {PG_INT64_MAX, PG_INT64_MIN};
		uint32		end = Min(nrows, (s + 1) * SIFT_TSTRIDE_ROWS);

		for (uint32 i = s * SIFT_TSTRIDE_ROWS; i < end; i++)
		{
			if (time_null != NULL && time_null[i])
				continue;
			t.time_min = Min(t.time_min, time[i]);
			t.time_max = Max(t.time_max, time[i]);
		}
		out[s] = t;
	}
	return ns;
}

/*
 * A granule's strides, as a block: the count, a flag that says some stride has no value (then the mask of those), the bit widths of the two packed
 * streams, the first minimum, the base of the minimum deltas, the unit every packed value is a multiple of, the minimum deltas less their base
 * and the spans (maximum less minimum), both packed at their widths. A stride with no value repeats the previous minimum and has span 0.
 */
#define TSTRIDE_HAS_EMPTY	1

static bool
stride_empty(const SiftTimeStride *s)
{
	return s->time_min > s->time_max;
}

static uint32
bit_width(uint64 v)
{
	return v == 0 ? 0 : 64 - __builtin_clzll(v);
}

static uint64
gcd64(uint64 a, uint64 b)
{
	while (b != 0)
	{
		uint64		t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static void
bits_put(uint8 *buf, uint64 *pos, uint64 v, uint32 width)
{
	while (width > 0)
	{
		uint32		at = *pos & 7,
					take = Min(width, 8 - at);

		buf[*pos >> 3] |= (uint8) ((v & ((1U << take) - 1)) << at);
		v >>= take;
		width -= take;
		*pos += take;
	}
}

static uint64
bits_get(const uint8 *buf, uint64 *pos, uint32 width)
{
	uint64		v = 0;

	for (uint32 got = 0; got < width;)
	{
		uint32		at = *pos & 7,
					take = Min(width - got, 8 - at);

		v |= (uint64) ((buf[*pos >> 3] >> at) & ((1U << take) - 1)) << got;
		got += take;
		*pos += take;
	}
	return v;
}

static uint64
bits_bytes(uint64 nvalues, uint32 width)
{
	return (nvalues * width + 7) / 8;
}

static void
tstride_block(StringInfo out, const SiftTimeStride *st, uint32 n)
{
	int64		base = 0,
				m[SIFT_GRANULE_STRIDES];
	uint64		d[SIFT_GRANULE_STRIDES],
				sp[SIFT_GRANULE_STRIDES],
				unit = 0,
				maxd = 0,
				maxs = 0;
	uint8		mask[SIFT_GRANULE_STRIDES / 8] = {0};
	uint32		wd,
				ws;
	bool		has_empty = false;
	uint8	   *bits;
	uint64		pos = 0;

	SIFT_INVARIANT(n <= SIFT_GRANULE_STRIDES, "scope strides: %u strides in a granule", n);
	for (uint32 i = 0; i < n; i++)
	{
		bool		empty = stride_empty(&st[i]);

		if (empty)
			mask[i / 8] |= 1 << (i % 8);
		has_empty |= empty;
		m[i] = empty ? (i == 0 ? 0 : m[i - 1]) : st[i].time_min;
		sp[i] = empty ? 0 : (uint64) st[i].time_max - (uint64) st[i].time_min;
	}
	for (uint32 i = 1; i < n; i++)
	{
		d[i] = (uint64) m[i] - (uint64) m[i - 1];
		if (i == 1 || (int64) d[i] < base)
			base = (int64) d[i];
	}
	for (uint32 i = 1; i < n; i++)
	{
		d[i] -= (uint64) base;
		unit = gcd64(unit, d[i]);
		maxd = Max(maxd, d[i]);
	}
	for (uint32 i = 0; i < n; i++)
	{
		unit = gcd64(unit, sp[i]);
		maxs = Max(maxs, sp[i]);
	}
	unit = Max(unit, 1);
	wd = bit_width(maxd / unit);
	ws = bit_width(maxs / unit);

	wire_put_var(out, n);
	wire_put_u8(out, has_empty ? TSTRIDE_HAS_EMPTY : 0);
	if (has_empty)
		appendBinaryStringInfo(out, (char *) mask, (n + 7) / 8);
	wire_put_u8(out, (uint8) wd);
	wire_put_u8(out, (uint8) ws);
	wire_put_var(out, wire_zig(m[0]));
	wire_put_var(out, wire_zig(base));
	wire_put_var(out, unit);
	bits = palloc0(bits_bytes(n, wd) + bits_bytes(n, ws) + 1);
	for (uint32 i = 1; i < n; i++)
		bits_put(bits, &pos, d[i] / unit, wd);
	pos = (pos + 7) & ~(uint64) 7;
	for (uint32 i = 0; i < n; i++)
		bits_put(bits, &pos, sp[i] / unit, ws);
	appendBinaryStringInfo(out, (char *) bits, (pos + 7) / 8);
	pfree(bits);
}

void
sift_tstride_write(StringInfo out, uint32 ngranules, const uint32 *nstrides, const SiftTimeStride *const *strides)
{
	StringInfoData body;
	uint32	   *off = palloc(sizeof(uint32) * (ngranules + 1));
	uint32		data_len;
	int			start = out->len;

	initStringInfo(&body);
	for (uint32 g = 0; g < ngranules; g++)
	{
		off[g] = body.len;
		if (nstrides[g] > 0)
			tstride_block(&body, strides[g], nstrides[g]);
	}
	off[ngranules] = data_len = body.len;
	appendBinaryStringInfo(out, (char *) &ngranules, sizeof(uint32));
	appendBinaryStringInfo(out, (char *) &data_len, sizeof(uint32));
	appendBinaryStringInfo(out, (char *) off, (ngranules + 1) * sizeof(uint32));
	while ((out->len - start) % MAXIMUM_ALIGNOF != 0)
		appendStringInfoChar(out, '\0');
	appendBinaryStringInfo(out, body.data, body.len);
	while ((out->len - start) % MAXIMUM_ALIGNOF != 0)
		appendStringInfoChar(out, '\0');
	pfree(body.data);
	pfree(off);
}

static bool
tstride_unblock(const char *p, uint32 len, SiftTimeStride *out, uint32 *nout)
{
	WireCur		c;
	uint32		n,
				wd,
				ws;
	uint8		flags,
				mask[SIFT_GRANULE_STRIDES / 8] = {0};
	int64		cur,
				base;
	uint64		unit,
				pos = 0,
				nbytes;
	const uint8 *bits;

	wire_cur(&c, p, len);
	n = (uint32) wire_get_var(&c);
	flags = wire_get_u8(&c);
	if (c.bad || n == 0 || n > SIFT_GRANULE_STRIDES || (flags & ~TSTRIDE_HAS_EMPTY))
		return false;
	if (flags & TSTRIDE_HAS_EMPTY)
		wire_get_bytes(&c, mask, (n + 7) / 8);
	wd = wire_get_u8(&c);
	ws = wire_get_u8(&c);
	cur = wire_unzig(wire_get_var(&c));
	base = wire_unzig(wire_get_var(&c));
	unit = wire_get_var(&c);
	if (c.bad || wd > 64 || ws > 64 || unit == 0 || (n == 1 && wd != 0))
		return false;
	nbytes = bits_bytes(n - 1, wd) + bits_bytes(n, ws);
	if ((uint64) (c.end - c.p) != nbytes)
		return false;
	bits = (const uint8 *) c.p;
	for (uint32 i = 0; i < n; i++)
	{
		if (i > 0)
			cur = (int64) ((uint64) cur + (uint64) base + bits_get(bits, &pos, wd) * unit);
		out[i].time_min = cur;
	}
	pos = (pos + 7) & ~(uint64) 7;
	for (uint32 i = 0; i < n; i++)
	{
		uint64		span = bits_get(bits, &pos, ws) * unit;

		if (mask[i / 8] >> (i % 8) & 1)
		{
			if (span != 0)
				return false;
			out[i] = (SiftTimeStride) {PG_INT64_MAX, PG_INT64_MIN};
			continue;
		}
		out[i].time_max = (int64) ((uint64) out[i].time_min + span);
		if (out[i].time_max < out[i].time_min)
			return false;
	}
	*nout = n;
	return true;
}

bool
sift_tstride_read(const char *p, uint64 len, uint32 gidx, SiftTimeStride *out, uint32 *n)
{
	uint32		ng,
				data_len,
				a,
				b;
	uint64		data_off;

	if (len < 8)
		return false;
	memcpy(&ng, p, 4);
	memcpy(&data_len, p + 4, 4);
	data_off = MAXALIGN(8 + (uint64) (ng + 1) * 4);
	if (gidx >= ng || len != data_off + MAXALIGN((uint64) data_len))
		return false;
	memcpy(&a, p + 8 + (uint64) gidx * 4, 4);
	memcpy(&b, p + 8 + (uint64) (gidx + 1) * 4, 4);
	if (a > b || b > data_len)
		return false;
	*n = 0;
	return a == b || tstride_unblock(p + data_off + a, b - a, out, n);
}
