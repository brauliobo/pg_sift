/*
 * col/ver.c - the codec of the version vectors of a column segment (col/ver.h, docs/COL-TAM.md section 7).
 */
#include "sift_host.h"

#include "col/ver.h"

#define MAX_BITS	33			/* a step between two uint32 values is within +-(2^32 - 1): a residual is below 2^33 */

static inline uint32
bits_of(uint64 x)
{
	uint32		b = 0;

	while (x >> b)
		b++;
	return b;
}

void
sift_cver_encode(const uint32 *v, uint32 n, StringInfo out)
{
	int64		mind = 0;
	uint64		maxres = 0;
	uint32		bits;
	uint64		acc = 0;
	uint32		nacc = 0;
	bool		same = true;

	Assert(n >= 1);
	for (uint32 i = 1; i < n && same; i++)
		same = v[i] == v[0];
	if (same)
	{
		appendStringInfoChar(out, SIFT_CVER_CONST);
		appendBinaryStringInfo(out, (const char *) &v[0], sizeof(uint32));
		return;
	}
	mind = (int64) v[1] - (int64) v[0];
	for (uint32 i = 2; i < n; i++)
		mind = Min(mind, (int64) v[i] - (int64) v[i - 1]);
	for (uint32 i = 1; i < n; i++)
		maxres = Max(maxres, (uint64) ((int64) v[i] - (int64) v[i - 1] - mind));
	bits = bits_of(maxres);
	appendStringInfoChar(out, SIFT_CVER_DELTA);
	appendBinaryStringInfo(out, (const char *) &v[0], sizeof(uint32));
	appendBinaryStringInfo(out, (const char *) &mind, sizeof(int64));
	appendStringInfoChar(out, (char) bits);
	if (bits == 0)
		return;
	for (uint32 i = 1; i < n; i++)
	{
		uint64		res = (uint64) ((int64) v[i] - (int64) v[i - 1] - mind);

		acc |= res << nacc;
		nacc += bits;
		while (nacc >= 8)
		{
			appendStringInfoChar(out, (char) (acc & 0xFF));
			acc >>= 8;
			nacc -= 8;
		}
		/* a residual of up to 33 bits shifted by up to 7 pending bits fits 64 bits */
	}
	if (nacc > 0)
		appendStringInfoChar(out, (char) (acc & 0xFF));
}

bool
sift_cver_decode(const uint8 *p, Size len, uint32 n, uint32 *out)
{
	int64		mind;
	uint32		bits;
	uint64		acc = 0,
				mask;
	uint32		nacc = 0;
	Size		pos;

	if (len < 1 + sizeof(uint32) || n == 0)
		return false;
	if (p[0] == SIFT_CVER_CONST)
	{
		uint32		x;

		if (len != 1 + sizeof(uint32))
			return false;
		memcpy(&x, p + 1, sizeof(x));
		for (uint32 i = 0; i < n; i++)
			out[i] = x;
		return true;
	}
	if (p[0] != SIFT_CVER_DELTA || len < 1 + sizeof(uint32) + sizeof(int64) + 1)
		return false;
	memcpy(&out[0], p + 1, sizeof(uint32));
	memcpy(&mind, p + 1 + sizeof(uint32), sizeof(int64));
	bits = p[1 + sizeof(uint32) + sizeof(int64)];
	pos = 1 + sizeof(uint32) + sizeof(int64) + 1;
	if (bits > MAX_BITS || len != pos + (Size) (((uint64) (n - 1) * bits + 7) / 8))
		return false;
	mask = bits == 0 ? 0 : (UINT64CONST(1) << bits) - 1;
	for (uint32 i = 1; i < n; i++)
	{
		int64		x;

		while (nacc < bits)
		{
			acc |= (uint64) p[pos++] << nacc;
			nacc += 8;
		}
		x = (int64) out[i - 1] + mind + (int64) (acc & mask);
		if (x < 0 || x > (int64) PG_UINT32_MAX)
			return false;
		out[i] = (uint32) x;
		acc >>= bits;
		nacc -= bits;
	}
	return true;
}
