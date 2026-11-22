/*
 * col/granule_vec.c - a range of rows of a chunk decoded straight into the native vector of the aggregation engine (agg/vec.h), and the layout of the lengths of an array column.
 */
#include "core/host.h"

#include "col/granule/granule_int.h"
#include "core/damage.h"
#include "core/popcount.h"

/* Bits [from, from + n) of the validity bitmap as words of a vector: bit i of the result is row from + i; the bits past n are zero */
static void
validity_slice(const SiftChunk *c, int from, int n, uint64 *words)
{
	memset(words, 0, SIFT_VEC_WORDS * sizeof(uint64));
	if (c->ref->flags & SIFT_CHUNK_ALL_NULL)
		return;
	if (c->validity == NULL)
	{
		for (int i = 0; i < n; i++)
			words[i >> 6] |= UINT64CONST(1) << (i & 63);
		return;
	}
	for (int i = 0; i < n; i++)
		if ((c->validity[(from + i) >> 3] >> ((from + i) & 7)) & 1)
			words[i >> 6] |= UINT64CONST(1) << (i & 63);
}

static inline bool
word_bit(const uint64 *words, int i)
{
	return (words[i >> 6] >> (i & 63)) & 1;
}

static void
want(const SiftChunk *c, bool ok)
{
	if (!ok)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift: chunk encoding %u cannot be decoded to the vector type asked for", c->enc),
					errhint("The part was written by another pg_sift release: REINDEX the index.")));
}

static bool
is_int_enc(int enc)
{
	return enc == SIFT_CENC_INT_FOR || enc == SIFT_CENC_INT_DELTA || enc == SIFT_CENC_INT_CONST || enc == SIFT_CENC_RLE || enc == SIFT_CENC_NUM64 || enc == SIFT_CENC_BOOL_BITS ||
		enc == SIFT_CENC_LIST_LEN || enc == SIFT_CENC_KEY_PRESENT || enc == SIFT_CENC_ALL_NULL;
}

static bool
is_f8_enc(int enc)
{
	return enc == SIFT_CENC_FLOAT_RAW || enc == SIFT_CENC_ALP || enc == SIFT_CENC_FLOAT_BSS || enc == SIFT_CENC_ALP_RD || enc == SIFT_CENC_ALL_NULL;
}

static bool
is_str_enc(int enc)
{
	return enc == SIFT_CENC_STR_INLINE || enc == SIFT_CENC_STR_BLOCK || enc == SIFT_CENC_STR_FSST || enc == SIFT_CENC_DICT_CODE || enc == SIFT_CENC_STR_VIEW || enc == SIFT_CENC_UUID16 ||
		enc == SIFT_CENC_NUM128 || enc == SIFT_CENC_INTERVAL16 || enc == SIFT_CENC_FIXED16 || enc == SIFT_CENC_STR_PROMO || enc == SIFT_CENC_ALL_NULL;
}

static void
fill_view(SiftStrView *w, const char *p, uint32 len)
{
	memset(w, 0, sizeof(*w));
	w->len = len;
	memcpy(w->prefix, p, Min(len, 4));
	if (len <= 12)
		memcpy(w->u.tail, p + Min(len, 4), len - Min(len, 4));
	else
		w->u.ptr = p;
}

static uint64
load_be64(const char *p)
{
	uint64		v;

	memcpy(&v, p, 8);
	return __builtin_bswap64(v);
}

static SiftVariantExcStream *
exc_stream(const SiftChunk *c)
{
	SiftVariantExcStream *x;

	if (c->exc == NULL || !(c->ref->flags & SIFT_CHUNK_HAS_OVERFLOW))
		return NULL;
	x = palloc0(sizeof(SiftVariantExcStream));
	x->n_entries = c->exc->n_entries;
	x->payload_bytes = c->exc->payload_bytes;
	x->exc_mask = sift_chunk_exc_mask(c);
	x->entries = sift_exc_entries(c->exc);
	x->payload_buf = (const uint8 *) (x->entries + x->n_entries + 1);
	return x;
}

static void
decode_dict32(const SiftChunk *c, int k0, int nn, int nrows, SiftVec *dst, const uint64 *valid)
{
	SiftGranuleDict *d = palloc(sizeof(SiftGranuleDict));
	const char *codes;
	uint32		codes_len;
	int64	   *code = palloc(Max(nn, 1) * sizeof(int64));
	uint32	   *o = (uint32 *) dst->data;
	int			k = 0;

	want(c, c->enc == SIFT_CENC_DICT_CODE);
	sift_chunk_dict(c, d, &codes, &codes_len);
	sift_cc_blob_ints(codes, codes_len, c->nnonnull, k0, k0 + nn, code);
	for (int r = 0; r < nrows; r++)
		o[r] = (dst->all_valid || word_bit(valid, r)) ? (uint32) code[k++] : 0;
	dst->dict = d;
	pfree(code);
}

void
sift_granule_decode_vec(const SiftGranuleView *v, int col, uint16 row_off, uint16 nrows, SiftVec *dst, const SiftVariantExcStream **exc_out)
{
	SiftChunk	c;
	int			k0,
				k1,
				nn;
	uint64		local[SIFT_VEC_WORDS];
	const uint64 *valid = local;
	size_t		esz;

	if (col < 0 || col + 1 >= v->hdr->nchunks || nrows > SIFT_VEC_LEN)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: rows [%d, %d) of column %d are not in the granule", row_off, row_off + nrows, col)));
	sift_chunk_open(v, col + 1, &c);
	if (row_off + nrows > c.nrows)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: rows [%d, %d) of column %d are not in the granule", row_off, row_off + nrows, col)));
	k0 = sift_chunk_rank(&c, row_off);
	k1 = sift_chunk_rank(&c, row_off + nrows);
	nn = k1 - k0;
	validity_slice(&c, row_off, nrows, local);
	dst->len = nrows;
	dst->dense = true;
	dst->sel = NULL;
	dst->scale = 0;
	dst->dict = NULL;
	dst->all_valid = nn == nrows;
	if (!dst->all_valid)
		memcpy(dst->valid, local, sizeof(local));
	else
		valid = NULL;
	if (exc_out != NULL)
		*exc_out = exc_stream(&c);

	switch (dst->type)
	{
		case SVEC_BOOL:
		case SVEC_I32:
		case SVEC_I64:
		case SVEC_NUM64:
			{
				int64	   *vals = palloc(Max(nn, 1) * sizeof(int64));

				want(&c, is_int_enc(c.enc));
				sift_chunk_ints(&c, k0, k1, vals);
				if (c.enc == SIFT_CENC_NUM64)
					dst->scale = (int16) *(const int64 *) c.body;
				if (dst->type == SVEC_BOOL)
				{
					uint8	   *o = dst->data;
					int			k = 0;

					for (int r = 0; r < nrows; r++)
						o[r] = (dst->all_valid || word_bit(valid, r)) ? vals[k++] != 0 : 0;
				}
				else if (dst->type == SVEC_I32)
				{
					int32	   *o = dst->data;
					int			k = 0;

					for (int r = 0; r < nrows; r++)
						o[r] = (dst->all_valid || word_bit(valid, r)) ? (int32) vals[k++] : 0;
				}
				else
				{
					int64	   *o = dst->data;
					int			k = 0;

					for (int r = 0; r < nrows; r++)
						o[r] = (dst->all_valid || word_bit(valid, r)) ? vals[k++] : 0;
				}
				pfree(vals);
			}
			break;
		case SVEC_F64:
			{
				double	   *vals = palloc(Max(nn, 1) * sizeof(double));
				double	   *o = dst->data;
				int			k = 0;

				want(&c, is_f8_enc(c.enc));
				sift_chunk_f8(&c, k0, k1, vals);
				for (int r = 0; r < nrows; r++)
					o[r] = (dst->all_valid || word_bit(valid, r)) ? vals[k++] : 0;
				pfree(vals);
			}
			break;
		case SVEC_DICT32:
			decode_dict32(&c, k0, nn, nrows, dst, valid);
			break;
		default:
			{
				const char **p = palloc(Max(nn, 1) * sizeof(char *));
				uint32	   *len = palloc(Max(nn, 1) * sizeof(uint32));
				int			k = 0;

				want(&c, is_str_enc(c.enc));
				sift_chunk_strs(&c, k0, k1, p, len);
				if (dst->type == SVEC_STRVIEW)
				{
					SiftStrView *o = dst->data;

					for (int r = 0; r < nrows; r++)
					{
						if (dst->all_valid || word_bit(valid, r))
						{
							fill_view(&o[r], p[k], len[k]);
							k++;
						}
						else
							memset(&o[r], 0, sizeof(o[r]));
					}
				}
				else
				{
					want(&c, dst->type == SVEC_NUM128 || dst->type == SVEC_UUID || dst->type == SVEC_INTERVAL);
					for (int r = 0; r < nrows; r++)
					{
						uint64	   *o = (uint64 *) ((char *) dst->data + (size_t) r * 16);

						if (!(dst->all_valid || word_bit(valid, r)))
							o[0] = o[1] = 0;
						else if (len[k] != 16)
							SIFT_DAMAGE_DATA(NULL, errmsg("sift: a 16-byte chunk holds a value of %u bytes", len[k]));
						else if (dst->type == SVEC_UUID)
						{
							o[0] = load_be64(p[k]);
							o[1] = load_be64(p[k] + 8);
							k++;
						}
						else
							memcpy(o, p[k++], 16);
					}
				}
				pfree(p);
				pfree(len);
			}
			break;
	}
	esz = dst->type == SVEC_BOOL ? 1 : dst->type == SVEC_I32 ? 4 : dst->type == SVEC_DICT32 ? 4 : dst->type == SVEC_I64 || dst->type == SVEC_NUM64 || dst->type == SVEC_F64 ? 8 : 16;
	memset((char *) dst->data + (size_t) nrows * esz, 0, SIFT_VEC_PAD);
}

bool
sift_granule_array_layout(const SiftGranuleView *v, int col, SiftArrayLayout *out)
{
	SiftChunk	c;
	int			n = v->hdr->nrows,
				nb = (n + 63) / 64 + 1;
	int64	   *vals;
	uint32	   *lengths,
			   *offsets;
	int			k = 0;

	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	sift_chunk_open(v, col + 1, &c);
	if (c.enc != SIFT_CENC_LIST_LEN)
		return false;
	vals = palloc(Max(c.nnonnull, 1) * sizeof(int64));
	lengths = palloc(n * sizeof(uint32));
	offsets = palloc((n + 1) * sizeof(uint32));
	sift_chunk_ints(&c, 0, c.nnonnull, vals);
	offsets[0] = 0;
	for (int i = 0; i < n; i++)
	{
		lengths[i] = sift_chunk_row_valid(&c, i) ? (uint32) vals[k++] : 0;
		offsets[i + 1] = offsets[i] + lengths[i];
	}
	*out = (SiftArrayLayout) {.n_parents = n,.n_elems = offsets[n],.block_elem_off = (const uint32 *) (c.body + 8),.lengths = lengths,.offsets = offsets};
	(void) nb;
	pfree(vals);
	return true;
}

SiftMaskState
sift_granule_decode_key_present(const SiftGranuleView *v, int col, uint16 row_off, uint16 nrows, uint64 *out_mask)
{
	const SiftChunkRef *ref;
	SiftChunk	c;
	int			hits = 0;

	if (col < 0 || col + 1 >= v->hdr->nchunks || nrows > SIFT_VEC_LEN || (uint32) row_off + nrows > v->hdr->nrows)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: rows %u..%u of column %d are not in the granule", row_off, row_off + nrows, col));
	ref = &v->hdr->chunks[col + 1];
	if (!(ref->enc & SIFT_ENC_KEY_ANY))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d of the granule keeps no rows of a key", col)));
	if (ref->enc & SIFT_ENC_KEY_ALL_ABSENT)
		return SIFT_MASK_ALL_ZERO;
	if (ref->enc & SIFT_ENC_KEY_ALL_PRESENT)
		return SIFT_MASK_ALL_ONE;
	sift_chunk_open(v, col + 1, &c);
	if (ref->enc & SIFT_ENC_KEY_EQ_VALID)
		validity_slice(&c, row_off, nrows, out_mask);
	else if (c.keyp->mode == SIFT_KP_BITMAP)
	{
		const uint8 *bm = (const uint8 *) (c.keyp + 1);

		memset(out_mask, 0, SIFT_VEC_WORDS * sizeof(uint64));
		for (int i = 0; i < nrows; i++)
			if ((bm[(row_off + i) >> 3] >> ((row_off + i) & 7)) & 1)
				out_mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	else
	{
		const uint16 *rows = (const uint16 *) (c.keyp + 1);
		bool		listed_present = c.keyp->mode == SIFT_KP_ARRAY16;

		memset(out_mask, 0, SIFT_VEC_WORDS * sizeof(uint64));
		if (!listed_present)
			for (int i = 0; i < nrows; i++)
				out_mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		for (int k = 0; k < c.keyp->count; k++)
		{
			int			i = (int) rows[k] - row_off;

			if (rows[k] >= v->hdr->nrows)
				SIFT_DAMAGE_DATA(NULL, errmsg("sift: a row %u of a key is past the %d rows of its granule", rows[k], v->hdr->nrows));
			if (i >= 0 && i < nrows)
				out_mask[i >> 6] ^= UINT64CONST(1) << (i & 63);
		}
	}
	for (int w = 0; w < SIFT_VEC_WORDS; w++)
		hits += sift_pc64(out_mask[w]);
	return hits == 0 ? SIFT_MASK_ALL_ZERO : hits == nrows ? SIFT_MASK_ALL_ONE : SIFT_MASK_BITS;
}
