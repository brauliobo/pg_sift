/*
 * col/granule_dec.c - the decoders of the chunks of a granule (col/granule.h): a chunk is opened (decompressed when it is wrapped, its list of nulls made a bit per row), and its body is
 * read back as integers, doubles or strings for a range of the values it holds. The bodies that are blobs of the column codec (col/codec.h) are read with its kernels.
 */
#include "core/host.h"

#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/granule_int.h"
#include "core/damage.h"
#include "core/popcount.h"

/* p moved on by n bytes of the chunk of len bytes that starts at raw: what no part of the chunk reaches past */
static const char *
chunk_skip(const char *raw, uint32 len, const char *p, uint64 n, const char *what)
{
	if (n > len - (uint64) (p - raw))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the %s of a chunk do not fit it", what));
	return p + n;
}

/* The elements of the arrays of column col of the granule: the total its chunk of lengths records (0 for a column that is all NULL) */
static int
group_total(const SiftGranuleView *v, int col)
{
	SiftChunk	g;
	int			total = 0;

	if (col < 0 || col + 1 >= v->hdr->nchunks)
		SIFT_FAIL_CORRUPT("sift: a chunk of elements names column %d, which is not in the granule", col);

	if (v->hdr->chunks[col + 1].flags & SIFT_CHUNK_ELEMS)
		SIFT_FAIL_CORRUPT("sift: the chunk of lengths of column %d holds elements itself", col);
	sift_chunk_open(v, col + 1, &g);
	if (g.enc == SIFT_CENC_LIST_LEN)
		total = (int) ((const uint32 *) g.body)[1];
	sift_chunk_close(&g);
	return total;
}

/* The decode of the values reads n bytes of the body of a chunk */
static void
body_holds(const SiftChunk *c, uint64 n)
{
	if (n > c->body_len)
		SIFT_FAIL_CORRUPT("sift: the %u values of a chunk of encoding %u need %llu bytes of its %u", c->nnonnull, c->enc, (unsigned long long) n, c->body_len);
}

/* The decode of values up to k1 reads those the chunk holds */
static void
values_hold(const SiftChunk *c, int k1)
{
	if (k1 > c->nnonnull)
		SIFT_FAIL_CORRUPT("sift: values up to %d are read of a chunk that holds %d", k1, c->nnonnull);
}

/* The exceptions that hold only kinds, made into the block the entries of the others are read from: the entries in row order, the mask when there are many */
static const SiftExcHeader *
exc_unpack(const SiftExcHeader *stored, int nrows)
{
	int64	   *tags = sift_alloc((nrows + SIFT_COL_PAD / 8 + 8) * sizeof(int64));
	SiftExcHeader *h;
	SiftVariantExcEntry *e;
	uint64	   *mask = NULL;
	uint32		n = 0;
	Size		len = sizeof(SiftExcHeader);

	sift_cc_blob_ints((const char *) (stored + 1), stored->payload_bytes, nrows, 0, nrows, tags);
	for (int i = 0; i < nrows; i++)
	{
		if (tags[i] < 0 || tags[i] > UINT8_MAX)
			SIFT_FAIL_CORRUPT("sift: the kind of a row is %ld", (long) tags[i]);
		n += tags[i] != 0;
	}
	if (n != stored->n_entries)
		SIFT_FAIL_CORRUPT("sift: a chunk lists %u kinds for %u exceptions", n, stored->n_entries);
	len += (n > SIFT_EXC_SPARSE_MAX ? SIFT_EXC_MASK_WORDS * 8 : 0) + ((Size) n + 1) * sizeof(SiftVariantExcEntry);
	h = sift_alloc0(len);
	h->n_entries = n;
	h->flags = n > SIFT_EXC_SPARSE_MAX ? SIFT_EXC_MASKED : 0;
	if (h->flags & SIFT_EXC_MASKED)
		mask = (uint64 *) (h + 1);
	e = (SiftVariantExcEntry *) sift_exc_entries(h);
	for (int i = 0, k = 0; i < nrows; i++)
		if (tags[i] != 0)
		{
			e[k++] = (SiftVariantExcEntry) {.row_idx = i,.kind = sift_exc_kind(tags[i]),.reserved = tags[i]};
			if (mask != NULL)
				mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
	sift_free(tags);
	return h;
}

void
sift_chunk_open(const SiftGranuleView *v, int idx, SiftChunk *c)
{
	const SiftChunkRef *ref = &v->hdr->chunks[idx];
	const char *p = v->rec + ref->off;
	uint32		len = ref->len;

	*c = (SiftChunk) {.ref = ref,.view = v,.idx = idx,.enc = ref->enc & SIFT_ENC_MASK,.nrows = (ref->flags & SIFT_CHUNK_ELEMS) ? group_total(v, ref->flags >> SIFT_CHUNK_GROUP_SHIFT) : v->hdr->nrows,
	.nnonnull = ref->nnonnull};
	if (ref->flags & SIFT_CHUNK_COMPRESSED)
	{
		uint32		hdr[2];
		char	   *raw;

		if (ref->len < 8)
			SIFT_FAIL_CORRUPT("sift: a wrapped chunk is shorter than its header");
		memcpy(hdr, p, 8);
		if (hdr[1] > SIFT_CHUNK_RAW_MAX)
			SIFT_FAIL_CORRUPT("sift: a wrapped chunk claims %u bytes", hdr[1]);
		if (!sift_colz_available(hdr[0]))
			SIFT_FAIL_UNSUPPORTED("Install a pg_sift built with the library, or REINDEX with this version.", "sift: chunk compression %u is not available in this build", hdr[0]);
		raw = sift_alloc(hdr[1] + 8);
		c->owned |= SIFT_CHUNK_OWN_RAW;
		if (!sift_colz_decompress(hdr[0], p + 8, ref->len - 8, raw, hdr[1]))
			SIFT_FAIL_CORRUPT("sift: a wrapped chunk does not decompress to its length");
		p = raw;
		len = hdr[1];
	}
	c->raw = p;
	if (!(ref->flags & (SIFT_CHUNK_ALL_VALID | SIFT_CHUNK_ALL_NULL)))
	{
		if (ref->enc & (SIFT_ENC_NULL_SPARSE | SIFT_ENC_NONNULL_SPARSE))
		{
			bool		nulls = (ref->enc & SIFT_ENC_NULL_SPARSE) != 0;
			uint16		listed;
			uint8	   *bm = sift_alloc((c->nrows + 7) / 8 + 8);

			if (len < 2)
				SIFT_FAIL_CORRUPT("sift: a list of rows is shorter than its count");
			memcpy(&listed, p, 2);
			if (listed > c->nrows || (uint32) 2 + 2 * listed > len || (!nulls && listed != c->nnonnull))
				SIFT_FAIL_CORRUPT("sift: a list of %u rows does not fit its chunk", listed);
			memset(bm, nulls ? 0xFF : 0, (c->nrows + 7) / 8 + 8);
			for (int i = 0; i < listed; i++)
			{
				uint16		row;

				memcpy(&row, p + 2 + 2 * i, 2);
				if (row >= c->nrows)
					SIFT_FAIL_CORRUPT("sift: a listed row %u is past the %d rows of its chunk", row, c->nrows);
				if (nulls)
					bm[row >> 3] &= ~(1 << (row & 7));
				else
					bm[row >> 3] |= 1 << (row & 7);
			}
			c->validity = bm;
			c->owned |= SIFT_CHUNK_OWN_VALID;
			p = chunk_skip(c->raw, len, p, MAXALIGN(2 + 2 * listed), "listed rows");
		}
		else
		{
			if ((uint64) MAXALIGN((c->nrows + 7) / 8) > len)
				SIFT_FAIL_CORRUPT("sift: the validity of a chunk is shorter than its %d rows", c->nrows);
			c->validity = (const uint8 *) p;
			p = chunk_skip(c->raw, len, p, MAXALIGN((c->nrows + 7) / 8), "validity bits");
		}
	}
	if (ref->flags & (SIFT_CHUNK_HAS_OVERFLOW | SIFT_CHUNK_HAS_KINDS))
	{
		const SiftExcHeader *stored = (const SiftExcHeader *) p;

		if ((uint64) (p - c->raw) + sizeof(*stored) > len)
			SIFT_FAIL_CORRUPT("sift: a chunk ends before its exceptions");
		if (stored->n_entries > SIFT_GRANULE_ROWS || c->nrows > SIFT_GRANULE_ROWS)
			SIFT_FAIL_CORRUPT("sift: a chunk has %u exceptions", stored->n_entries);
		c->exc_len = SIFT_EXC_BLOCK_LEN(stored);
		if ((uint64) (p - c->raw) + c->exc_len > len)
			SIFT_FAIL_CORRUPT("sift: the exceptions of a chunk do not fit it");
		if (stored->flags & SIFT_EXC_KINDS_PACKED)
		{
			c->exc = exc_unpack(stored, c->nrows);
			c->owned |= SIFT_CHUNK_OWN_EXC;
		}
		else
			c->exc = stored;
		p += c->exc_len;
	}
	if (ref->enc & SIFT_ENC_KEY_HAS_MASK)
	{
		const SiftKeyPresentHdr *h = (const SiftKeyPresentHdr *) p;
		uint64		bytes;

		if ((uint64) (p - c->raw) + sizeof(*h) > len)
			SIFT_FAIL_CORRUPT("sift: the rows of a key end the chunk before their header");
		bytes = h->mode == SIFT_KP_BITMAP ? MAXALIGN((c->nrows + 7) / 8) : MAXALIGN(2 * (uint64) h->count);
		if ((h->mode != SIFT_KP_ARRAY16 && h->mode != SIFT_KP_NEG_ARRAY16 && h->mode != SIFT_KP_BITMAP) || h->count > c->nrows || h->npresent > c->nrows ||
			(h->mode == SIFT_KP_BITMAP && h->count != c->nrows) || (uint64) (p - c->raw) + sizeof(*h) + bytes > len)
			SIFT_FAIL_CORRUPT("sift: the rows of a key (mode %u, %u of them) do not fit the chunk of %d rows", h->mode, h->count, c->nrows);
		c->keyp = h;
		p += sizeof(*h) + bytes;
	}
	if ((uint64) (p - c->raw) > len)
		SIFT_FAIL_CORRUPT("sift: the validity, exceptions and rows of a key of a chunk do not fit its %u bytes", len);
	c->body = p;
	c->body_len = len - (p - c->raw);
	if (c->nnonnull != ((ref->flags & SIFT_CHUNK_ALL_NULL) || c->enc == SIFT_CENC_ALL_NULL ? 0 : c->validity == NULL ? c->nrows : sift_chunk_rank(c, c->nrows)))
		SIFT_FAIL_CORRUPT("sift: a chunk of %d rows says %d hold a value, its validity another count", c->nrows, c->nnonnull);
}

void
sift_chunk_close(SiftChunk *c)
{
	if (c->owned & SIFT_CHUNK_OWN_EXC)
		sift_free((void *) c->exc);
	if (c->owned & SIFT_CHUNK_OWN_VALID)
		sift_free((void *) c->validity);
	if (c->owned & SIFT_CHUNK_OWN_RAW)
		sift_free((void *) c->raw);
	c->owned = 0;
}

/* The strings of these point into the bytes of the chunk, the others were decoded into buffers of their own */
static bool
strs_borrow_raw(const SiftChunk *c)
{
	return c->enc == SIFT_CENC_STR_INLINE || c->enc == SIFT_CENC_STR_VIEW || c->enc == SIFT_CENC_UUID16 || c->enc == SIFT_CENC_NUM128 || c->enc == SIFT_CENC_INTERVAL16 ||
		c->enc == SIFT_CENC_STR_PROMO;
}

void
sift_chunk_release_strs(SiftChunk *c)
{
	if (c->owned & SIFT_CHUNK_OWN_EXC)
		sift_free((void *) c->exc);
	if (c->owned & SIFT_CHUNK_OWN_VALID)
		sift_free((void *) c->validity);
	if ((c->owned & SIFT_CHUNK_OWN_RAW) && !strs_borrow_raw(c))
		sift_free((void *) c->raw);
	c->owned = 0;
}

int
sift_chunk_rank_advance(const SiftChunk *c, int *cur_word, int *cur_rank, int target_row)
{
	int			tw = target_row >> 6,
				rem = target_row & 63;
	uint64		tail = 0;

	if (c->ref->flags & SIFT_CHUNK_ALL_NULL)
		return 0;
	if (c->validity == NULL)
		return target_row;
	if (*cur_word > tw)
		*cur_word = *cur_rank = 0;
	for (; *cur_word < tw; (*cur_word)++)
	{
		uint64		w;

		memcpy(&w, c->validity + (size_t) *cur_word * 8, 8);
		*cur_rank += (int) sift_pc64(w);
	}
	if (rem == 0)
		return *cur_rank;
	memcpy(&tail, c->validity + (size_t) tw * 8, Min(8, ((c->nrows + 7) >> 3) - tw * 8));
	return *cur_rank + (int) sift_pc64(tail & ((UINT64CONST(1) << rem) - 1));
}

int
sift_chunk_rank(const SiftChunk *c, int row)
{
	int			word = 0,
				rank = 0;

	return sift_chunk_rank_advance(c, &word, &rank, row);
}

/* The bytes the validity of a chunk takes ahead of its exceptions and body, as they are stored */
uint32
sift_chunk_validity_len(const SiftChunkRef *ref, int nrows)
{
	if (ref->flags & (SIFT_CHUNK_ALL_VALID | SIFT_CHUNK_ALL_NULL))
		return 0;
	if (ref->enc & SIFT_ENC_NULL_SPARSE)
		return MAXALIGN(2 + 2 * (uint32) (nrows - ref->nnonnull));
	if (ref->enc & SIFT_ENC_NONNULL_SPARSE)
		return MAXALIGN(2 + 2 * ref->nnonnull);
	return MAXALIGN((nrows + 7) / 8);
}

static uint16
rows_of_bitmap(const uint8 *bm, int nrows, uint16 *out, uint16 cap)
{
	uint16		n = 0;

	for (int r = 0; r < nrows; r++)
		if ((bm[r >> 3] >> (r & 7)) & 1)
		{
			if (n == cap)
				return cap + 1;
			out[n++] = r;
		}
	return n;
}

uint16
sift_chunk_nonnull_rows(const SiftChunkRef *ref, const char *p, uint32 len, int nrows, uint16 *out, uint16 cap)
{
	uint16		n = 0;

	if (ref->flags & SIFT_CHUNK_ALL_NULL)
		return 0;
	if (ref->flags & SIFT_CHUNK_ALL_VALID)
	{
		if (nrows > cap)
			return cap + 1;
		for (int r = 0; r < nrows; r++)
			out[r] = r;
		return nrows;
	}
	if (ref->nnonnull > cap)
		return cap + 1;
	if (len < sift_chunk_validity_len(ref, nrows))
		SIFT_FAIL_CORRUPT("sift: the validity of a chunk is shorter than its rows say");
	if (ref->enc & SIFT_ENC_NONNULL_SPARSE)
	{
		memcpy(out, p + 2, 2 * (size_t) ref->nnonnull);
		for (int i = 0; i < ref->nnonnull; i++)
			if (out[i] >= nrows || (i > 0 && out[i] <= out[i - 1]))
				SIFT_FAIL_CORRUPT("sift: a list of rows with a value is not ascending inside its %d rows", nrows);
		return ref->nnonnull;
	}
	if (ref->enc & SIFT_ENC_NULL_SPARSE)
	{
		int			nnull = nrows - ref->nnonnull,
					i = 0;

		for (int r = 0; r < nrows; r++)
		{
			uint16		null_row = nrows;

			if (i < nnull)
				memcpy(&null_row, p + 2 + 2 * i, 2);
			if (null_row == r)
				i++;
			else
				out[n++] = r;
		}
		return n;
	}
	return rows_of_bitmap((const uint8 *) p, nrows, out, cap);
}

uint16
sift_granule_nonnull_rows(const SiftGranuleView *v, int col, uint16 *out, uint16 cap)
{
	const SiftChunkRef *ref;
	SiftChunk	c;
	uint16		n;

	if (col < 0 || col + 1 >= v->hdr->nchunks)
		SIFT_FAIL_CORRUPT("sift: column %d is not in the granule", col);
	ref = &v->hdr->chunks[col + 1];
	if (ref->flags & SIFT_CHUNK_ELEMS)
		SIFT_FAIL_CORRUPT("sift: column %d holds elements, which have no row of their own", col);
	if (!(ref->flags & SIFT_CHUNK_COMPRESSED))
		return sift_chunk_nonnull_rows(ref, v->rec + ref->off, ref->len, v->hdr->nrows, out, cap);
	sift_chunk_open(v, col + 1, &c);
	if (ref->flags & (SIFT_CHUNK_ALL_VALID | SIFT_CHUNK_ALL_NULL))
		n = sift_chunk_nonnull_rows(ref, NULL, 0, c.nrows, out, cap);
	else
		n = c.nnonnull > cap ? cap + 1 : rows_of_bitmap(c.validity, c.nrows, out, cap);
	sift_chunk_close(&c);
	return n;
}

const uint64 *
sift_chunk_exc_mask(const SiftChunk *c)
{
	uint64	   *mask;
	const SiftVariantExcEntry *e;

	if (c->exc->flags & SIFT_EXC_MASKED)
		return (const uint64 *) ((const char *) c->exc + sizeof(SiftExcHeader));
	mask = sift_alloc0((Size) SIFT_EXC_MASK_WORDS * 8);
	e = sift_exc_entries(c->exc);
	for (int i = 0; i < c->exc->n_entries; i++)
		mask[e[i].row_idx >> 6] |= UINT64CONST(1) << (e[i].row_idx & 63);
	return mask;
}

static void
unsupported(uint16 enc)
{
	SIFT_FAIL_UNSUPPORTED("Upgrade pg_sift, or REINDEX the index with this version.", "sift: chunk encoding %u is not supported by this build", enc);
}

SiftCc *
sift_cc_blob_open(const char *p, uint32 avail, uint32 nvals, uint32 *whole)
{
	uint32		len;

	if (avail < 8)
		SIFT_FAIL_CORRUPT("sift: a codec blob is shorter than its length");
	memcpy(&len, p, 4);
	if (len > avail - 8)
		SIFT_FAIL_CORRUPT("sift: a codec blob of %u bytes does not fit its chunk of %u", len, avail);
	if (8 + MAXALIGN(len) > avail)
		SIFT_FAIL_CORRUPT("sift: a codec blob of %u bytes is not padded inside its chunk of %u", len, avail);
	if (whole != NULL)
		*whole = 8 + MAXALIGN(len);
	return sift_cc_open_rows(NULL, (const uint8 *) p + 8, len, false, nvals);
}

void
sift_cc_blob_ints(const char *p, uint32 avail, uint32 nvals, int k0, int k1, int64 *out)
{
	SiftCc	   *cc = sift_cc_blob_open(p, avail, nvals, NULL);
	int			m = k1 - k0;

	if (m == (int) nvals)
	{
		int64	   *all = sift_alloc((nvals + SIFT_COL_PAD / 8 + 8) * sizeof(int64));

		sift_cc_decode(cc, all);
		memcpy(out, all, (uint64) m * sizeof(int64));
		sift_free(all);
	}
	else if (m > 0)
	{
		uint32	   *idx = sift_alloc(m * sizeof(uint32));

		for (int i = 0; i < m; i++)
			idx[i] = k0 + i;
		sift_cc_decode_sel(cc, idx, m, out);
		sift_free(idx);
	}
	sift_cc_close(cc);
}

void
sift_chunk_ints(const SiftChunk *c, int k0, int k1, int64 *out)
{
	const char *body = c->body;
	int			n = k1 - k0;

	values_hold(c, k1);
	switch (c->enc)
	{
		case SIFT_CENC_ALL_NULL:
			break;
		case SIFT_CENC_KEY_PRESENT:
			for (int i = 0; i < n; i++)
				out[i] = 1;
			break;
		case SIFT_CENC_INT_CONST:
			{
				int64		v;

				body_holds(c, 8);
				memcpy(&v, body, 8);
				for (int i = 0; i < n; i++)
					out[i] = v;
			}
			break;
		case SIFT_CENC_INT_FOR:
		case SIFT_CENC_INT_DELTA:
		case SIFT_CENC_RLE:
			sift_cc_blob_ints(body, c->body_len, c->nnonnull, k0, k1, out);
			break;
		case SIFT_CENC_NUM64:
			body_holds(c, 8);
			sift_cc_blob_ints(body + 8, c->body_len - 8, c->nnonnull, k0, k1, out);
			break;
		case SIFT_CENC_BOOL_BITS:
			body_holds(c, ((uint64) k1 + 7) / 8);
			for (int i = 0; i < n; i++)
				out[i] = ((uint8) body[(k0 + i) >> 3] >> ((k0 + i) & 7)) & 1;
			break;
		case SIFT_CENC_LIST_LEN:
			{
				int			nb = (c->nrows + 63) / 64 + 1;
				uint32		skip = 8 + MAXALIGN((uint64) nb * 4);

				body_holds(c, skip);
				sift_cc_blob_ints(body + skip, c->body_len - skip, c->nnonnull, k0, k1, out);
			}
			break;
		default:
			unsupported(c->enc);
	}
}

SiftCc *
sift_chunk_cc_open(const SiftChunk *c)
{
	uint32		skip;

	switch (c->enc)
	{
		case SIFT_CENC_INT_FOR:
		case SIFT_CENC_INT_DELTA:
		case SIFT_CENC_RLE:
			skip = 0;
			break;
		case SIFT_CENC_NUM64:
			skip = 8;
			break;
		case SIFT_CENC_LIST_LEN:
			skip = 8 + MAXALIGN((uint64) ((c->nrows + 63) / 64 + 1) * 4);
			break;
		default:
			return NULL;
	}
	body_holds(c, skip);
	return sift_cc_blob_open(c->body + skip, c->body_len - skip, c->nnonnull, NULL);
}

void
sift_chunk_ints_sel(const SiftChunk *c, const uint32 *idx, int m, int64 *out)
{
	SiftCc	   *cc = sift_chunk_cc_open(c);

	if (cc == NULL)
	{
		for (int i = 0; i < m; i++)
			sift_chunk_ints(c, idx[i], idx[i] + 1, &out[i]);
		return;
	}
	sift_cc_decode_sel(cc, idx, m, out);
	sift_cc_close(cc);
}

static uint64
get_bits(const uint8 *p, uint32 len, uint64 at, int w)
{
	uint64		acc = 0;
	uint64		byte = at >> 3;

	for (int j = 0; j < 8 && byte + j < len; j++)
		acc |= (uint64) p[byte + j] << (8 * j);
	return (acc >> (at & 7)) & ((UINT64CONST(1) << w) - 1);
}

/* The row of entry k of the list of rows whose left part is not in the table of an ALP_RD chunk */
static uint32
alp_rd_exc_pos(const uint8 *exc, uint32 k)
{
	uint16		pos;

	memcpy(&pos, exc + 2 * (uint64) k, 2);
	return pos;
}

/* Doubles [k0, k1) of an ALP_RD chunk (see put_alp_rd): the left part from its table or the list of exceptions, the right part as stored, the dropped low zero bits put back */
static void
alp_rd_f8(const SiftChunk *c, int k0, int k1, double *out)
{
	const uint8 *body = (const uint8 *) c->body;
	uint32		nn = c->nnonnull,
				n_exc,
				x = 0,
				hi;
	int			lb = body[0],
				nd = body[1],
				tz = body[2],
				flags = body[3],
				rb;
	uint16		dict[SIFT_ALP_RD_DICT];
	uint64		clen,
				rlen,
				elen;
	const uint8 *codes = body + 8 + sizeof(dict),
			   *rights,
			   *exc;

	memcpy(&n_exc, body + 4, 4);
	if (lb < SIFT_ALP_RD_MIN_LEFT || lb > SIFT_ALP_RD_MAX_LEFT || nd > SIFT_ALP_RD_DICT || tz > SIFT_ALPRD_MAX_TZ || lb + tz >= 64 || (flags & ~SIFT_ALPRD_HAS_EXC) ||
		((flags & SIFT_ALPRD_HAS_EXC) == 0 && n_exc != 0) || n_exc > nn)
		SIFT_FAIL_CORRUPT("sift: a split-bits chunk of %u bytes does not hold its %u values of %d left bits", c->body_len, nn, lb);
	rb = 64 - lb - tz;
	clen = MAXALIGN(((uint64) nn * 3 + 7) / 8);
	rlen = MAXALIGN(((uint64) nn * rb + 7) / 8);
	elen = MAXALIGN((uint64) n_exc * 4);
	if (c->body_len < 8 + sizeof(dict) + clen + rlen + elen)
		SIFT_FAIL_CORRUPT("sift: a split-bits chunk of %u bytes does not hold its %u values of %d left bits", c->body_len, nn, lb);
	memcpy(dict, body + 8, sizeof(dict));
	rights = codes + clen;
	exc = rights + rlen;
	for (hi = n_exc; x < hi;)
	{
		uint32		mid = x + (hi - x) / 2;

		if (alp_rd_exc_pos(exc, mid) < (uint32) k0)
			x = mid + 1;
		else
			hi = mid;
	}
	for (int i = k0; i < k1; i++)
	{
		uint64		code = get_bits(codes, clen, (uint64) i * 3, 3),
					left,
					u;

		if (code >= (uint64) nd)
			SIFT_FAIL_CORRUPT("sift: a split-bits chunk holds the code %llu of a table of %d", (unsigned long long) code, nd);
		left = dict[code];
		if (x < n_exc && alp_rd_exc_pos(exc, x) == (uint32) i)
		{
			uint16		l;

			memcpy(&l, exc + 2 * (uint64) n_exc + 2 * (uint64) x, 2);
			left = l;
			x++;
		}
		u = (((left << rb) | get_bits(rights, rlen, (uint64) i * rb, rb)) << tz);
		memcpy(&out[i - k0], &u, 8);
	}
}

void
sift_chunk_f8(const SiftChunk *c, int k0, int k1, double *out)
{
	const char *body = c->body;
	int			n = k1 - k0;

	values_hold(c, k1);
	switch (c->enc)
	{
		case SIFT_CENC_ALL_NULL:
			break;
		case SIFT_CENC_FLOAT_RAW:
			body_holds(c, (uint64) k1 * 8);
			memcpy(out, body + (uint64) k0 * 8, (uint64) n * 8);
			break;
		case SIFT_CENC_ALP:
			{
				SiftCc	   *cc = sift_cc_blob_open(body, c->body_len, c->nnonnull, NULL);
				double	   *all = sift_alloc((c->nnonnull + SIFT_COL_PAD / 8 + 8) * sizeof(double));

				sift_cc_decode_f8(cc, all);
				memcpy(out, all + k0, (uint64) n * 8);
				sift_free(all);
				sift_cc_close(cc);
			}
			break;
		case SIFT_CENC_ALP_RD:
			alp_rd_f8(c, k0, k1, out);
			break;
		case SIFT_CENC_FLOAT_BSS:
			if (sift_chunk_framed(c))
				sift_frames_f8(c, k0, k1, out);
			else
			{
				body_holds(c, (uint64) c->nnonnull * 8);
				for (int i = 0; i < n; i++)
					for (int b = 0; b < 8; b++)
						((uint8 *) &out[i])[b] = (uint8) body[(uint64) b * c->nnonnull + k0 + i];
			}
			break;
		default:
			unsupported(c->enc);
	}
}

void
sift_chunk_f8_sel(const SiftChunk *c, const uint32 *idx, int m, double *out)
{
	double	   *all = NULL;

	if (c->enc == SIFT_CENC_ALP || (c->enc == SIFT_CENC_FLOAT_BSS && sift_chunk_framed(c) && m > (c->nnonnull + SIFT_FRAME_ROWS - 1) / SIFT_FRAME_ROWS))
	{
		all = sift_alloc((c->nnonnull + SIFT_COL_PAD / 8 + 8) * sizeof(double));
		sift_chunk_f8(c, 0, c->nnonnull, all);
	}
	for (int i = 0; i < m; i++)
	{
		if (all != NULL)
			out[i] = all[idx[i]];
		else
			sift_chunk_f8(c, idx[i], idx[i] + 1, &out[i]);
	}
	if (all != NULL)
		sift_free(all);
}

/* The dictionary of the part that codes the chunk, of the kind its encoding says */
static const SiftPartDict *
part_dict(const SiftChunk *c, SiftDictKind kind)
{
	const SiftPartDict *d = sift_part_dict_find(c->view->dicts, c->idx - 1, kind);

	if (d == NULL)
		SIFT_FAIL_CORRUPT("sift: column %d is coded by a dictionary of its part, which the part does not have", c->idx - 1);
	return d;
}

/* The strings of a codec list (its length, then the blob) at p: off and bytes allocated with sift_alloc */
static void
block_strs(const char *p, uint32 avail, uint32 n, uint32 **off, char **bytes, uint32 *whole, const void *ddict)
{
	uint32		len;

	if (avail < 8)
		SIFT_FAIL_CORRUPT("sift: a list of strings is shorter than its length");
	memcpy(&len, p, 4);
	if (len > avail - 8)
		SIFT_FAIL_CORRUPT("sift: a list of strings of %u bytes does not fit its chunk of %u", len, avail);
	sift_cc_decode_strs_dict(NULL, (const uint8 *) p + 8, len, n, n, 0, off, bytes, ddict);
	if (whole != NULL)
		*whole = 8 + MAXALIGN(len);
}

/* Strings [k0, k1) of an FSST chunk (see put_fsst): decoded into one buffer of sift_alloc that p and len point into */
static void
fsst_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len, void **out_own)
{
	bool		shared = (c->ref->enc & SIFT_ENC_PART_DICT) != 0;
	SiftFsst   *t = shared ? part_dict(c, SIFT_DICT_KIND_FSST)->fsst : sift_alloc(sizeof(SiftFsst));
	const uint8 *body = (const uint8 *) c->body;
	uint32		avail = c->body_len,
				at = shared ? 0 : sift_fsst_read_table(t, body, avail),
				hdr[2],
				whole_raw,
			   *off,
			   *outoff;
	uint64		whole,
				cap;
	int64	   *coded;
	uint8	   *out;

	if (k1 <= k0)
		return;
	if (avail - at < 16 || (!shared && at == 0))
		SIFT_FAIL_CORRUPT("sift: the symbol table of a string chunk is damaged");
	memcpy(hdr, body + at, 8);
	memcpy(&whole_raw, body + at + 8, 4);
	whole = 8 + MAXALIGN((uint64) whole_raw);
	at += 8;
	if (whole > avail - at || hdr[1] >= avail - at - whole || (uint64) hdr[1] > (uint64) hdr[0] * 2)
		SIFT_FAIL_CORRUPT("sift: the coded strings of a chunk do not fit it");
	coded = sift_alloc(k1 * sizeof(int64));
	sift_cc_blob_ints((const char *) body + at, avail - at, c->nnonnull, 0, k1, coded);
	off = sift_alloc((k1 + 1) * sizeof(uint32));
	off[0] = 0;
	for (int k = 0; k < k1; k++)
	{
		if (coded[k] < 0 || (uint64) coded[k] > hdr[1] - off[k])
			SIFT_FAIL_CORRUPT("sift: a coded string is past the end of its chunk");
		off[k + 1] = off[k] + (uint32) coded[k];
	}
	if (k1 == c->nnonnull && off[k1] != hdr[1])
		SIFT_FAIL_CORRUPT("sift: the coded strings of a chunk are %u bytes, its header says %u", off[k1], hdr[1]);
	cap = Min((uint64) hdr[0], (uint64) 8 * (off[k1] - off[k0])) + SIFT_FSST_SLACK;
	out = sift_alloc(cap);
	if (out_own != NULL)
		*out_own = out;
	outoff = sift_alloc((k1 - k0 + 1) * sizeof(uint32));
	if (!sift_fsst_decode_strs(t, body + at + whole, off + k0, k1 - k0, out, cap, outoff))
		SIFT_FAIL_CORRUPT("sift: a coded string does not decode");
	for (int k = k0; k < k1; k++)
	{
		p[k - k0] = (const char *) out + outoff[k - k0];
		len[k - k0] = outoff[k - k0 + 1] - outoff[k - k0];
	}
	sift_free(coded);
	sift_free(off);
	if (!shared)
		sift_free(t);
	sift_free(outoff);
}

void
sift_chunk_dict(const SiftChunk *c, SiftGranuleDict *d, const char **codes, uint32 *codes_len)
{
	if (c->ref->enc & SIFT_ENC_PART_DICT)
	{
		const SiftPartDict *pd = part_dict(c, SIFT_DICT_KIND_STR);

		*d = (SiftGranuleDict) {.n = pd->n,.off = pd->off,.bytes = pd->bytes,.part = true};
		*codes = c->body;
		*codes_len = c->body_len;
	}
	else
	{
		uint32		nd,
					whole,
				   *off;
		char	   *bytes;

		if (c->body_len < 8)
			SIFT_FAIL_CORRUPT("sift: a dictionary chunk is shorter than its header");
		memcpy(&nd, c->body, 4);
		if (nd > SIFT_DICT_GRANULE_MAX)
			SIFT_FAIL_CORRUPT("sift: a dictionary of %u strings in a granule", nd);
		block_strs(c->body + 8, c->body_len - 8, nd, &off, &bytes, &whole, NULL);
		*d = (SiftGranuleDict) {.n = nd,.off = off,.bytes = bytes};
		*codes = c->body + 8 + whole;
		*codes_len = c->body_len - 8 - whole;
	}
}

void
sift_chunk_strs_own(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len, SiftStrOwn *own)
{
	const char *body = c->body;
	int			nn = c->nnonnull;

	values_hold(c, k1);
	switch (c->enc)
	{
		case SIFT_CENC_ALL_NULL:
			break;
		case SIFT_CENC_STR_PROMO:
			sift_promo_strs(c, k0, k1, p, len);
			break;
		case SIFT_CENC_STR_INLINE:
			{
				const uint64 table = 4 * ((uint64) nn + 1);
				const char *bytes = body + table;

				body_holds(c, table);
				for (int k = k0; k < k1; k++)
				{
					uint32		range[2];

					memcpy(range, body + 4 * (size_t) k, sizeof(range));
					if (range[1] < range[0] || range[1] > c->body_len - table)
						SIFT_FAIL_CORRUPT("sift: an inline string of a chunk is past its end");
					p[k - k0] = bytes + range[0];
					len[k - k0] = range[1] - range[0];
				}
			}
			break;
		case SIFT_CENC_STR_BLOCK:
			if (sift_chunk_framed(c))
				sift_frames_strs(c, k0, k1, p, len, own != NULL ? &own->a[0] : NULL);
			else
			{
				uint32	   *offs;
				char	   *bytes;

				block_strs(body, c->body_len, nn, &offs, &bytes, NULL, (c->ref->enc & SIFT_ENC_PART_DICT) ? part_dict(c, SIFT_DICT_KIND_ZSTD)->zstd_ddict : NULL);
				if (own != NULL)
				{
					own->a[0] = offs;
					own->a[1] = bytes;
				}
				for (int k = k0; k < k1; k++)
				{
					p[k - k0] = bytes + offs[k];
					len[k - k0] = offs[k + 1] - offs[k];
				}
			}
			break;
		case SIFT_CENC_STR_FSST:
			fsst_strs(c, k0, k1, p, len, own != NULL ? &own->a[0] : NULL);
			break;
		case SIFT_CENC_DICT_CODE:
			{
				SiftGranuleDict d;
				const char *codes;
				uint32		codes_len;
				int64	   *code = sift_alloc(Max(k1 - k0, 1) * sizeof(int64));

				sift_chunk_dict(c, &d, &codes, &codes_len);
				if (own != NULL && !d.part)
				{
					own->a[0] = (void *) d.off;
					own->a[1] = (void *) d.bytes;
				}
				sift_cc_blob_ints(codes, codes_len, nn, k0, k1, code);
				for (int k = k0; k < k1; k++)
				{
					if ((uint64) code[k - k0] >= d.n)
						SIFT_FAIL_CORRUPT("sift: a dictionary code is past its dictionary");
					p[k - k0] = d.bytes + d.off[code[k - k0]];
					len[k - k0] = d.off[code[k - k0] + 1] - d.off[code[k - k0]];
				}
				sift_free(code);
			}
			break;
		case SIFT_CENC_STR_VIEW:
			{
				const char *heap = body + (uint64) nn * sizeof(SiftStrView);

				body_holds(c, (uint64) nn * sizeof(SiftStrView));
				for (int k = k0; k < k1; k++)
				{
					const char *at = body + (uint64) k * sizeof(SiftStrView);
					SiftStrView v;

					memcpy(&v, at, sizeof(v));
					if (v.len > 12 && (uint64) v.u.offset + v.len > c->body_len - (uint64) nn * sizeof(SiftStrView))
						SIFT_FAIL_CORRUPT("sift: a string of a chunk lies outside its %u bytes", c->body_len);
					len[k - k0] = v.len;
					p[k - k0] = v.len <= 12 ? at + offsetof(SiftStrView, prefix) : heap + v.u.offset;
				}
			}
			break;
		case SIFT_CENC_UUID16:
		case SIFT_CENC_NUM128:
		case SIFT_CENC_INTERVAL16:
			body_holds(c, (uint64) k1 * 16);
			for (int k = k0; k < k1; k++)
			{
				p[k - k0] = body + (uint64) k * 16;
				len[k - k0] = 16;
			}
			break;
		case SIFT_CENC_FIXED16:
			{
				char	   *vals = sift_alloc(Max(k1 - k0, 1) * 16);
				uint32		whole;
				SiftCc	   *lane = sift_cc_blob_open(body, c->body_len, nn, &whole);
				int64	   *tmp = sift_alloc((nn + SIFT_COL_PAD / 8 + 8) * sizeof(int64));

				if (own != NULL)
					own->a[0] = vals;
				for (int h = 0; h < 2; h++)
				{
					if (h == 1)
						lane = sift_cc_blob_open(body + whole, c->body_len - whole, nn, NULL);
					sift_cc_decode(lane, tmp);
					for (int k = k0; k < k1; k++)
						memcpy(vals + (uint64) (k - k0) * 16 + 8 * h, &tmp[k], 8);
					sift_cc_close(lane);
				}
				for (int k = k0; k < k1; k++)
				{
					p[k - k0] = vals + (uint64) (k - k0) * 16;
					len[k - k0] = 16;
				}
				sift_free(tmp);
			}
			break;
		default:
			unsupported(c->enc);
	}
}

void
sift_chunk_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len)
{
	sift_chunk_strs_own(c, k0, k1, p, len, NULL);
}

void
sift_str_own_free(SiftStrOwn *own)
{
	for (int i = 0; i < 3; i++)
		if (own->a[i] != NULL)
			sift_free(own->a[i]);
	*own = (SiftStrOwn) {0};
}

void
sift_chunk_block_strs_own(const SiftChunk *c, uint32 **offs, char **bytes)
{
	if (sift_chunk_framed(c))
		sift_frames_block_strs(c, offs, bytes);
	else
		block_strs(c->body, c->body_len, c->nnonnull, offs, bytes, NULL, (c->ref->enc & SIFT_ENC_PART_DICT) ? part_dict(c, SIFT_DICT_KIND_ZSTD)->zstd_ddict : NULL);
}

/* One value of a FIXED16 chunk or of a chunk coded by the dictionary of its part, decoded alone and not with the nn values around it */
void
sift_chunk_str_one(const SiftChunk *c, int k, const char **p, uint32 *len)
{
	uint32		uk = k;
	int			nn = c->nnonnull;

	if (c->enc == SIFT_CENC_FIXED16)
	{
		char	   *val = sift_alloc(16);
		uint32		whole;
		SiftCc	   *lane = sift_cc_blob_open(c->body, c->body_len, nn, &whole);

		for (int h = 0; h < 2; h++)
		{
			int64		half;

			if (h == 1)
				lane = sift_cc_blob_open(c->body + whole, c->body_len - whole, nn, NULL);
			sift_cc_decode_sel(lane, &uk, 1, &half);
			memcpy(val + 8 * h, &half, 8);
			sift_cc_close(lane);
		}
		*p = val;
		*len = 16;
	}
	else if (c->enc == SIFT_CENC_DICT_CODE && (c->ref->enc & SIFT_ENC_PART_DICT))
	{
		const SiftPartDict *pd = part_dict(c, SIFT_DICT_KIND_STR);
		SiftCc	   *cc = sift_cc_blob_open(c->body, c->body_len, nn, NULL);
		int64		code;

		sift_cc_decode_sel(cc, &uk, 1, &code);
		sift_cc_close(cc);
		if ((uint64) code >= pd->n)
			SIFT_FAIL_CORRUPT("sift: a dictionary code is past its dictionary");
		*p = pd->bytes + pd->off[code];
		*len = pd->off[code + 1] - pd->off[code];
	}
	else
		sift_chunk_strs(c, k, k + 1, p, len);
}
