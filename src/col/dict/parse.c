/*
 * col/dict/parse.c - a dictionary read back from its blocks, in place or into memory
 * the plain form read straight into the arrays, and the view the readers search
 * the inverse of serialize.c
 */
#include "core/host.h"
#include "col/compress.h"
#include "col/dict/dict_int.h"

/* ... and sorted for the rows of a top-N once the scans have compared this many times its entries as bytes (a sort costs about that many compares) */
#define DICT_ORDER_ROWS		4

/* The plain form read straight into the arrays of the dictionary (sift_cdict_plain_*): the offsets first, then the bytes they end at, then the checks cdict_from makes */
SiftCDict *
sift_cdict_plain_begin(MemoryContext mcxt, uint32 n)
{
	SiftCDict  *d = sift_cdict_create(mcxt);

	pfree(d->off);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = MemoryContextAlloc(mcxt, ((Size) d->cap + 1) * sizeof(uint32));
	return d;
}

bool
sift_cdict_plain_bytes(SiftCDict *d, MemoryContext mcxt, uint64 room)
{
	if (d->off[0] != 0 || d->off[d->n] > room)
		return false;
	d->nbytes = d->off[d->n];
	d->bytescap = Max(d->nbytes, 256);
	pfree(d->bytes);
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	return true;
}

bool
sift_cdict_plain_valid(const SiftCDict *d)
{
	for (uint32 c = 0; c < d->n; c++)
		if (d->off[c + 1] < d->off[c] || d->off[c + 1] - d->off[c] > SIFT_COL_DICT_MAX_ENTRY)
			return false;
	return true;
}

/* A dictionary of n strings that takes the arrays off[n + 1] (room for Max(n, DICT_MINCAP) + 1) and bytes (room for Max(nbytes, 256)) allocated in mcxt */
static SiftCDict *
cdict_adopt(MemoryContext mcxt, uint32 n, uint32 *off, char *bytes, Size nbytes)
{
	SiftCDict  *d;

	if (off[0] != 0)
		return NULL;
	d = sift_cdict_create(mcxt);
	pfree(d->off);
	pfree(d->bytes);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = off;
	d->nbytes = off[n];
	d->bytescap = Max(nbytes, 256);
	d->bytes = bytes;
	if (d->nbytes != nbytes || !sift_cdict_plain_valid(d))
	{
		d->off = NULL;
		d->bytes = NULL;
		pfree(d);
		return NULL;
	}
	return d;
}

/* A dictionary of n strings: off[n + 1] and bytes[off[n]] are copied */
static SiftCDict *
cdict_from(MemoryContext mcxt, uint32 n, const uint32 *off, const char *bytes)
{
	SiftCDict  *d;

	if (off[0] != 0)
		return NULL;
	d = sift_cdict_create(mcxt);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = MemoryContextAlloc(mcxt, ((Size) d->cap + 1) * sizeof(uint32));
	memcpy(d->off, off, ((Size) n + 1) * sizeof(uint32));
	d->nbytes = d->off[n];
	for (uint32 c = 0; c < n; c++)
		if (d->off[c + 1] < d->off[c] || cdict_len(d, c) > SIFT_COL_DICT_MAX_ENTRY)
			return NULL;
	d->bytescap = Max(d->nbytes, 256);
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	memcpy(d->bytes, bytes, d->nbytes);
	return d;
}

/* The dictionary of a blocks form in memory, with its order for DICT_LOOKUP (the codes are checked: the order is trusted to find and match, sift.col_verify checks that it is one) */
static SiftCDict *
parse_blocks(MemoryContext mcxt, const uint8 *p, Size len, uint32 form, Size *used)
{
	uint32		head[4],
				n,
				fixed,
				nbytes,
				nblk,
				pos = 0;
	uint64		blk0,
				total,
				perm_end;
	uint32	   *boff,
			   *off;
	uint8	   *raw;
	char	   *bytes;
	SiftCDict  *d = NULL;
	bool		ok = true,
				hashed = (form & DICT_HASHED) != 0,
				packed = (form & DICT_PERM_PACKED) != 0;

	form &= ~DICT_FLAGS;
	if (len < DICT_HEAD_LOOKUP)
		return NULL;
	memcpy(head, p, DICT_HEAD_LOOKUP);
	n = head[1];
	fixed = head[2];
	nbytes = head[3];
	if (n == 0 || fixed > SIFT_COL_DICT_MAX_ENTRY || (fixed != 0 && (uint64) n * fixed != nbytes))
		return NULL;
	nblk = (n + DICT_BLOCK - 1) / DICT_BLOCK;
	blk0 = DICT_HEAD_LOOKUP + ((uint64) nblk + 1) * sizeof(uint32);
	if (blk0 > len)
		return NULL;
	boff = palloc(((Size) nblk + 1) * sizeof(uint32));
	memcpy(boff, p + DICT_HEAD_LOOKUP, ((Size) nblk + 1) * sizeof(uint32));
	total = blk0 + boff[nblk];
	if (packed && form != DICT_LOOKUP && form != DICT_LOOKUP_COUNT)
	{
		pfree(boff);
		return NULL;
	}
	if (form == DICT_LOOKUP || form == DICT_LOOKUP_COUNT)
		total = (total + sizeof(uint32) - 1) / sizeof(uint32) * sizeof(uint32) + sift_cdict_perm_bytes(n, packed ? sift_cdict_perm_bits(n) : 0);
	perm_end = total;
	if (form == DICT_LOOKUP_COUNT)
	{
		uint32		last;

		if (total + DICT_COUNT_HEAD(n) > len)
		{
			pfree(boff);
			return NULL;
		}
		memcpy(&last, p + total + (uint64) 8 * DICT_COUNT_NB(n) + sizeof(uint32), sizeof(last));
		total += DICT_COUNT_HEAD(n) + last;
	}
	if (hashed)
	{
		if (form == DICT_BLOCKS)
			total = (total + sizeof(uint32) - 1) / sizeof(uint32) * sizeof(uint32);
		total += (uint64) SIFT_CDICT_HASH_SLOTS(n) * sizeof(uint32);
	}
	if (boff[0] != 0 || blk0 + boff[nblk] > len || total > len)
	{
		pfree(boff);
		return NULL;
	}
	/* the arrays are the dictionary's own (cdict_adopt), at the sizes a dictionary of its codes has, so that nothing is copied again */
	off = MemoryContextAlloc(mcxt, ((Size) Max(n, DICT_MINCAP) + 1) * sizeof(uint32));
	bytes = MemoryContextAlloc(mcxt, Max(nbytes, 256));
	raw = palloc(DICT_BLOCK_RAW_MAX);
	for (uint32 b = 0; b < nblk && ok; b++)
	{
		uint32		lo = b * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, n - lo),
					rawlen,
					clen,
					sum = 0;
		const uint8 *h = p + blk0 + boff[b],
				   *src;

		if (boff[b + 1] < boff[b] + DICT_BLOCK_HEAD)
		{
			ok = false;
			break;
		}
		clen = boff[b + 1] - boff[b] - DICT_BLOCK_HEAD;
		rawlen = h[2] | (uint32) h[3] << 8;
		src = h + DICT_BLOCK_HEAD;
		if (rawlen > DICT_BLOCK_RAW_MAX)
			ok = false;
		else if (h[0] == 0)
			ok = clen == rawlen;
		else
		{
			ok = (h[0] == SIFT_CC_ALG_ZSTD || h[0] == SIFT_CC_ALG_LZ4) && sift_colz_available(h[0]) && sift_colz_decompress(h[0], src, clen, raw, rawlen);
			src = raw;
		}
		if (ok && fixed != 0)
		{
			ok = rawlen == cnt * fixed;
			for (uint32 i = 0; ok && i < cnt; i++)
				off[lo + i] = pos + i * fixed;
			sum = rawlen;
		}
		else if (ok)
		{
			ok = rawlen >= cnt;
			for (uint32 i = 0; ok && i < cnt; i++)
			{
				off[lo + i] = pos + sum;
				sum += src[i];
				ok = src[i] <= SIFT_COL_DICT_MAX_ENTRY;
			}
			ok = ok && sum == rawlen - cnt;
			src += cnt;
		}
		if (ok)
			ok = (uint64) pos + sum <= nbytes;
		if (ok)
		{
			memcpy(bytes + pos, src, sum);
			pos += sum;
		}
	}
	if (ok && pos == nbytes)
	{
		off[n] = pos;
		d = cdict_adopt(mcxt, n, off, bytes, nbytes);
	}
	pfree(boff);
	pfree(raw);
	if (d == NULL)
	{
		pfree(off);
		pfree(bytes);
		return NULL;
	}
	d->blocked = true;
	if (form == DICT_BLOCKS)
	{
		*used = total;
		return d;
	}
	d->stored_order = true;
	d->sorted = MemoryContextAlloc(mcxt, (Size) n * sizeof(uint32));
	if (packed)
	{
		uint32		bits = sift_cdict_perm_bits(n);
		uint64		bytes_of_perm = sift_cdict_perm_bytes(n, bits);

		for (uint32 i = 0; i < n; i++)
			d->sorted[i] = sift_ckeyed_get(p + (perm_end - bytes_of_perm), bytes_of_perm, (uint64) i * bits, bits);
	}
	else
		memcpy(d->sorted, p + (perm_end - (uint64) n * sizeof(uint32)), (Size) n * sizeof(uint32));
	for (uint32 i = 0; i < n; i++)
		if (d->sorted[i] >= n)
			return NULL;
	*used = total;
	return d;
}

SiftCDict *
sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used)
{
	const uint8 *p = data;
	uint32		head[2],
				nbytes;

	if (len < DICT_HEAD)
		return NULL;
	memcpy(head, p, DICT_HEAD);
	if ((head[0] & ~DICT_FLAGS) == DICT_LOOKUP || (head[0] & ~DICT_FLAGS) == DICT_LOOKUP_COUNT || (head[0] & ~DICT_FLAGS) == DICT_BLOCKS)
		return parse_blocks(mcxt, p, len, head[0], used);
	p += DICT_HEAD;
	len -= DICT_HEAD;
	if (head[0] == DICT_STRS)
	{
		uint32	   *off;
		char	   *bytes;
		SiftCDict  *d;

		sift_cc_decode_strs(mcxt, p, len, head[1], &off, &bytes);
		d = cdict_from(mcxt, head[1], off, bytes);
		pfree(off);
		pfree(bytes);
		*used = DICT_HEAD + len;
		return d;
	}
	if (head[0] != DICT_PLAIN || len / sizeof(uint32) < (Size) head[1] + 1)
		return NULL;
	memcpy(&nbytes, p + sizeof(uint32) * head[1], sizeof(nbytes));
	if (len - sizeof(uint32) * ((Size) head[1] + 1) < nbytes)
		return NULL;
	*used = DICT_HEAD + sizeof(uint32) * ((Size) head[1] + 1) + nbytes;
	return cdict_from(mcxt, head[1], (const uint32 *) p, (const char *) (p + sizeof(uint32) * ((Size) head[1] + 1)));
}

static const uint32 *
cdict_order(const void *octx, uint64 rows)
{
	SiftCDict  *d = (SiftCDict *) octx;

	if (d->sorted == NULL && (d->orows += rows) >= (uint64) DICT_ORDER_ROWS * d->n)
		cdict_sort(d);
	return d->sorted;
}

void
sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view)
{
	view->n = d->n;
	view->off = d->off;
	view->bytes = d->bytes;
	view->id = id;
	view->order = cdict_order;
	view->octx = d;
	view->get = NULL;
	view->gctx = NULL;
}
