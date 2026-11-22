/*
 * col/gpu_unwrap.c - a blob of the column codec as the device reads it (col/gpu.h): the device unwraps LZ4, so a node wrapped in zstd (what a cold part writes for a stream that shrinks by a
 * few percent: low cardinality columns, dates, small integers) is replaced here by the node it holds, and the nodes a cold part writes under that layer only (DELTA_BYTE, BSS) by a
 * frame of reference of whole bytes, which is what they decode to. The layout of the nodes is the codec's (col/codec_write.c): a node is a multiple of 8 bytes
 * and the byte lengths of the children of RLE, DICT, ALP and SCALE are in the node, so no leaf has to be measured.
 */
#include "postgres.h"

#include "lib/stringinfo.h"

#include "col/codec.h"
#include "col/compress.h"
#include "col/gpu.h"

#define BLOB_HEAD	SIFT_CC_HDRLEN
#define BLOB_TAIL	8				/* the checksum of the blob and its padding */
#define NODE_HEAD	16				/* the head of a node of a leaf: tag, width, parameter, then the first 8 byte value */
#define ROW_BLOCK	1024			/* the rows between two checkpoints of a DELTA_BYTE node */
#define RUP8(x)		(((x) + 7) & ~(uint64) 7)

static uint32
u32_at(const uint8 *p)
{
	uint32		v;

	memcpy(&v, p, sizeof(v));
	return v;
}

typedef struct Unwrap
{
	StringInfo	out;			/* NULL: only look for a zstd wrapper */
	bool		found;
	uint32		rows;			/* the values of the node being read: the head of its parent says how many */
	const char *why;
} Unwrap;

static bool
fail(Unwrap *u, const char *why)
{
	u->why = why;
	return false;
}

static void
put(Unwrap *u, const uint8 *p, uint32 n)
{
	if (u->out != NULL)
		appendBinaryStringInfo(u->out, (const char *) p, (int) n);
}

static bool node(Unwrap *u, const uint8 *p, uint32 size);

static uint32
bytes_for(uint64 range)
{
	return range <= 0xFF ? 1 : range <= 0xFFFF ? 2 : range <= 0xFFFFFFFFu ? 4 : 8;
}

/* DELTA_BYTE and BSS of a whole blob as the values they hold */
static bool
byte_values(Unwrap *u, const uint8 *p, uint32 size, int64 *v)
{
	const uint32 n = u->rows,
				B = p[1];
	uint64		base;

	memcpy(&base, p + 8, sizeof(base));
	if (p[0] == SIFT_CCN_BSS)
	{
		if (B < 2 || B > 8 || (uint64) NODE_HEAD + RUP8((uint64) n * B) > size)
			return fail(u, "a byte plane node of a chunk is shorter than its rows");
		for (uint32 i = 0; i < n; i++)
		{
			uint64		x = 0;

			for (uint32 b = 0; b < B; b++)
				x |= (uint64) p[NODE_HEAD + (uint64) b * n + i] << (8 * b);
			v[i] = (int64) (base + x);
		}
		return true;
	}
	if (B < 1 || B > 4 || (uint64) NODE_HEAD + 8 + 8 * (uint64) ((n - 1) / ROW_BLOCK) + RUP8((uint64) n * B) > size)
		return fail(u, "a byte delta node of a chunk is shorter than its rows");
	{
		const uint8 *res = p + NODE_HEAD + 8 + 8 * ((n - 1) / ROW_BLOCK);
		uint64		mind,
					cur = base;

		memcpy(&mind, p + NODE_HEAD, sizeof(mind));
		v[0] = (int64) cur;
		for (uint32 i = 1; i < n; i++)
		{
			uint64		x = 0;

			memcpy(&x, res + (uint64) i * B, B);
			cur += mind + x;
			v[i] = (int64) cur;
		}
	}
	return true;
}

static bool
node_bytes(Unwrap *u, const uint8 *p, uint32 size)
{
	int64	   *v;
	int64		lo,
				hi;
	uint32		B;
	uint64		zero = 0;

	if (u->rows < 2)
		return fail(u, "a chunk has a byte node of fewer than two values");
	u->found = true;
	if (u->out == NULL)
		return true;
	/* check-memory: allow the values of one chunk of a granule, at most SIFT_GRANULE_ROWS */
	v = palloc(u->rows * sizeof(int64));
	if (!byte_values(u, p, size, v))
		return false;
	lo = hi = v[0];
	for (uint32 i = 1; i < u->rows; i++)
	{
		lo = Min(lo, v[i]);
		hi = Max(hi, v[i]);
	}
	B = bytes_for((uint64) hi - (uint64) lo);
	put(u, (const uint8 *) &(char[8]) {SIFT_CCN_FORBYTE, (char) B}, 8);
	put(u, (const uint8 *) &lo, 8);
	for (uint32 i = 0; i < u->rows; i++)
	{
		uint64		x = (uint64) v[i] - (uint64) lo;

		put(u, (const uint8 *) &x, B);
	}
	put(u, (const uint8 *) &zero, (uint32) (RUP8((uint64) u->rows * B) - (uint64) u->rows * B));
	pfree(v);
	return true;
}

/* The children of a node of two (RLE, DICT, ALP: head, then the length of each child) or of one (SCALE: the length of its child is the parameter of the head) */
static bool
node_children(Unwrap *u, const uint8 *p, uint32 size, uint32 head, uint32 len_off, bool two, uint32 rows_a, uint32 rows_b)
{
	uint32		rows = u->rows,
				na = u32_at(p + len_off),
				nb = two ? u32_at(p + len_off + 4) : 0,
				start = u->out != NULL ? u->out->len - head : 0,
				mark;

	if ((uint64) head + na + nb > size)
		return fail(u, "a node of a chunk is longer than its parent says");
	u->rows = rows_a;
	mark = u->out != NULL ? u->out->len : 0;
	if (!node(u, p + head, na))
		return false;
	if (u->out != NULL)
	{
		uint32		fresh = u->out->len - mark;

		memcpy(u->out->data + start + len_off, &fresh, sizeof(fresh));
	}
	if (two)
	{
		u->rows = rows_b;
		mark = u->out != NULL ? u->out->len : 0;
		if (!node(u, p + head + na, nb))
			return false;
		if (u->out != NULL)
		{
			uint32		fresh = u->out->len - mark;

			memcpy(u->out->data + start + len_off + 4, &fresh, sizeof(fresh));
		}
	}
	u->rows = rows;
	put(u, p + head + na + nb, size - head - na - nb);
	return true;
}

static bool
node_wrap(Unwrap *u, const uint8 *p, uint32 size)
{
	uint32		rawlen = u32_at(p + 4),
				clen = u32_at(p + 8);
	uint8	   *buf;
	bool		ok;

	if (p[1] == SIFT_CC_ALG_LZ4)
	{
		put(u, p, size);
		return true;
	}
	if (p[1] != SIFT_CC_ALG_ZSTD)
		return fail(u, "a chunk has a wrapper that is neither LZ4 nor zstd");
	if (16 + (uint64) clen > size)
		return fail(u, "a wrapper of a chunk is longer than its parent says");
	u->found = true;
	if (u->out == NULL)
		return true;
	if (!sift_colz_available(SIFT_CC_ALG_ZSTD))
		return fail(u, "a chunk is wrapped in zstd and this build has no zstd");
	buf = palloc(Max(rawlen, 1));
	ok = sift_colz_decompress(SIFT_CC_ALG_ZSTD, p + 16, clen, buf, rawlen) && node(u, buf, rawlen);
	if (!ok && u->why == NULL)
		u->why = "a zstd wrapper of a chunk does not unwrap";
	pfree(buf);
	return ok;
}

static bool
node(Unwrap *u, const uint8 *p, uint32 size)
{
	switch (p[0])
	{
		case SIFT_CCN_CONST:
		case SIFT_CCN_FORBIT:
		case SIFT_CCN_FORBYTE:
		case SIFT_CCN_DELTA:
			put(u, p, size);
			return true;
		case SIFT_CCN_RLE:
		case SIFT_CCN_DICT:
			put(u, p, 16);
			return node_children(u, p, size, 16, 8, true, u32_at(p + 4), p[0] == SIFT_CCN_RLE ? u32_at(p + 4) : u->rows);
		case SIFT_CCN_ALP:
			put(u, p, 32);
			return node_children(u, p, size, 32, 8, true, u->rows, u32_at(p + 4));
		case SIFT_CCN_SCALE:
			put(u, p, 24);
			return node_children(u, p, size, 24, 4, false, u->rows, 0);
		case SIFT_CCN_WRAP:
			return node_wrap(u, p, size);
		case SIFT_CCN_DELTA_BYTE:
		case SIFT_CCN_BSS:
			return node_bytes(u, p, size);
		default:
			return fail(u, "a chunk has a node the device does not know");
	}
}

bool
sift_gpu_blob_unwrap(const uint8 *blob, uint32 blen, StringInfo out, const char **why)
{
	Unwrap		look = {.rows = u32_at(blob + 4)},
				work = {.out = out,.rows = u32_at(blob + 4)};
	uint32		root = blen - BLOB_HEAD - BLOB_TAIL;

	if (blen < BLOB_HEAD + 16 + BLOB_TAIL)
	{
		*why = "a blob of a chunk is shorter than its header";
		return false;
	}
	if (!node(&look, blob + BLOB_HEAD, root))
	{
		*why = look.why;
		return false;
	}
	if (!look.found)
		return true;
	appendBinaryStringInfo(out, (const char *) blob, BLOB_HEAD);
	if (!node(&work, blob + BLOB_HEAD, root))
	{
		*why = work.why;
		return false;
	}
	appendBinaryStringInfo(out, (const char *) blob + blen - BLOB_TAIL, BLOB_TAIL);
	return true;
}
