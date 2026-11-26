/*
 * col/granule_vec.c - a range of rows of a chunk decoded straight into the native vector of the aggregation engine (agg/vec.h), and the layout of the lengths of an array column.
 */
#include "core/host.h"

#include "col/granule/int.h"
#include "core/cmp.h"
#include "core/damage.h"
#include "core/popcount.h"

/* The 8 bytes at byte at of a bitmap of nbytes bytes, those past its end zero */
static inline uint64
load_word(const uint8 *bits, uint32 nbytes, uint32 at)
{
	uint64		w = 0;

	if (at < nbytes)
		memcpy(&w, bits + at, Min(8, nbytes - at));
	return w;
}

/* Bits [from, from + n) of a bitmap of nbytes bytes as words of a vector (bit i of the result is bit from + i; the bits past n are zero), a word at a time */
static void
bits_slice(const uint8 *bits, uint32 nbytes, int from, int n, uint64 *words)
{
	memset(words, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (int w = 0; w * 64 < n; w++)
	{
		uint32		bit = from + 64 * w,
					sh = bit & 7;
		uint64		lo = load_word(bits, nbytes, bit >> 3),
					v = sh == 0 ? lo : (lo >> sh) | (load_word(bits, nbytes, (bit >> 3) + 8) << (64 - sh));

		words[w] = v;
	}
	if (n & 63)
		words[n >> 6] &= (UINT64CONST(1) << (n & 63)) - 1;
}

/* The validity of rows [from, from + n) of a chunk as the words of a vector */
static void
validity_slice(const SiftChunk *c, int from, int n, uint64 *words)
{
	if (c->ref->flags & SIFT_CHUNK_ALL_NULL)
		memset(words, 0, SIFT_VEC_WORDS * sizeof(uint64));
	else if (c->validity == NULL)
	{
		memset(words, 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(words, 0xFF, (size_t) (n >> 6) * 8);
		if (n & 63)
			words[n >> 6] = (UINT64CONST(1) << (n & 63)) - 1;
	}
	else
		bits_slice(c->validity, (c->nrows + 7) >> 3, from, n, words);
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


static void
free_if(void *p)
{
	if (p != NULL)
		pfree(p);
}

static void
cursor_bad_rows(const SiftGranVecCursor *cur, int col_row_off, int nrows)
{
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: rows [%d, %d) of column %d are not in the granule", col_row_off, col_row_off + nrows, cur->chunk.idx - 1)));
}

void
sift_vec_cursor_open(const SiftGranuleView *v, int col, SiftVecType vtype, SiftGranVecCursor *cur)
{
	*cur = (SiftGranVecCursor) {.vtype = vtype};
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d is not in the granule", col)));
	sift_chunk_open(v, col + 1, &cur->chunk);
	cur->open = true;
}

void
sift_vec_cursor_close(SiftGranVecCursor *cur)
{
	if (cur->cc != NULL)
		sift_cc_close(cur->cc);
	if (cur->dict_ready && !cur->dict.part)
	{
		free_if((void *) cur->dict.off);
		free_if((void *) cur->dict.bytes);
	}
	free_if(cur->alp_all);
	free_if(cur->str_p);
	free_if(cur->str_len);
	sift_str_own_free(&cur->str_own);
	free_if(cur->exc);
	free_if(cur->exc_mask_own);
	free_if(cur->num);
	free_if(cur->batch_p);
	free_if(cur->batch_len);
	if (cur->open)
		sift_chunk_close(&cur->chunk);
	*cur = (SiftGranVecCursor) {0};
}

/* The exceptions of the chunk as the stream the aggregation reads, made once for the granule */
static const SiftVariantExcStream *
cursor_exc(SiftGranVecCursor *cur)
{
	const SiftChunk *c = &cur->chunk;
	SiftVariantExcStream *x;

	if (cur->exc != NULL || c->exc == NULL || !(c->ref->flags & SIFT_CHUNK_HAS_OVERFLOW))
		return cur->exc;
	x = palloc0(sizeof(SiftVariantExcStream));
	x->n_entries = c->exc->n_entries;
	x->payload_bytes = c->exc->payload_bytes;
	x->exc_mask = sift_chunk_exc_mask(c);
	if (!(c->exc->flags & SIFT_EXC_MASKED))
		cur->exc_mask_own = (uint64 *) x->exc_mask;
	x->entries = sift_exc_entries(c->exc);
	x->payload_buf = (const uint8 *) (x->entries + x->n_entries + 1);
	cur->exc = x;
	return x;
}

/* The values of one batch: SIFT_VEC_LEN of them and the pad the kernels write past them */
static void *
cursor_num(SiftGranVecCursor *cur)
{
	if (cur->num == NULL)
		cur->num = palloc((SIFT_VEC_LEN + SIFT_COL_PAD / 8 + 8) * sizeof(int64));
	return cur->num;
}

/* Integers [k0, k1) of the nonnull values: through the cascade opened once for the granule when the body is one */
static void
cursor_ints(SiftGranVecCursor *cur, int k0, int k1, int64 *out)
{
	const SiftChunk *c = &cur->chunk;
	uint32		idx[SIFT_VEC_LEN];

	if (!cur->prepared)
	{
		cur->cc = sift_chunk_cc_open(c);
		cur->prepared = true;
	}
	if (cur->cc == NULL)
		sift_chunk_ints(c, k0, k1, out);
	else if (k1 - k0 == c->nnonnull)
		sift_cc_decode(cur->cc, out);
	else
	{
		for (int i = 0; i < k1 - k0; i++)
			idx[i] = k0 + i;
		sift_cc_decode_sel(cur->cc, idx, k1 - k0, out);
	}
}

static void
cursor_dict_codes(SiftGranVecCursor *cur, int k0, int k1, int64 *out)
{
	const SiftChunk *c = &cur->chunk;
	uint32		idx[SIFT_VEC_LEN];

	if (!cur->prepared)
	{
		const char *codes;
		uint32		codes_len;

		want(c, c->enc == SIFT_CENC_DICT_CODE);
		sift_chunk_dict(c, &cur->dict, &codes, &codes_len);
		cur->dict_ready = true;
		cur->cc = sift_cc_blob_open(codes, codes_len, c->nnonnull, NULL);
		cur->prepared = true;
	}
	for (int i = 0; i < k1 - k0; i++)
		idx[i] = k0 + i;
	if (k1 > k0)
		sift_cc_decode_sel(cur->cc, idx, k1 - k0, out);
}

/* Doubles [k0, k1): an ALP chunk is decoded whole once, the others by the range */
static const double *
cursor_f8(SiftGranVecCursor *cur, int k0, int k1)
{
	const SiftChunk *c = &cur->chunk;
	double	   *vals = cursor_num(cur);

	want(c, is_f8_enc(c->enc));
	if (c->enc != SIFT_CENC_ALP)
	{
		sift_chunk_f8(c, k0, k1, vals);
		return vals;
	}
	if (cur->alp_all == NULL)
	{
		cur->alp_all = palloc((c->nnonnull + SIFT_COL_PAD / 8 + 8) * sizeof(double));
		sift_chunk_f8(c, 0, c->nnonnull, cur->alp_all);
	}
	return cur->alp_all + k0;
}

/* The strings [k0, k1) as pointers and lengths: those of a block, a symbol table, a dictionary or a 16-byte chunk are decoded whole once and kept for the granule */
static void
cursor_strs(SiftGranVecCursor *cur, int k0, int k1, const char ***p, uint32 **len)
{
	const SiftChunk *c = &cur->chunk;
	int			nn = c->nnonnull;

	want(c, is_str_enc(c->enc));
	if (!cur->windowed && (c->enc == SIFT_CENC_STR_BLOCK || c->enc == SIFT_CENC_STR_FSST || c->enc == SIFT_CENC_FIXED16 || c->enc == SIFT_CENC_DICT_CODE))
	{
		if (cur->str_p == NULL)
		{
			/* check-memory: allow the strings of one chunk of a granule, at most SIFT_GRANULE_ROWS */
			cur->str_p = palloc(Max(nn, 1) * sizeof(char *));
			/* check-memory: allow the strings of one chunk of a granule, at most SIFT_GRANULE_ROWS */
			cur->str_len = palloc(Max(nn, 1) * sizeof(uint32));
			sift_chunk_strs_own(c, 0, nn, cur->str_p, cur->str_len, &cur->str_own);
		}
		*p = cur->str_p + k0;
		*len = cur->str_len + k0;
		return;
	}
	if (cur->batch_p == NULL)
	{
		cur->batch_p = palloc(SIFT_VEC_LEN * sizeof(char *));
		cur->batch_len = palloc(SIFT_VEC_LEN * sizeof(uint32));
	}
	sift_chunk_strs(c, k0, k1, cur->batch_p, cur->batch_len);
	*p = cur->batch_p;
	*len = cur->batch_len;
}

static void
cursor_fill_ints(SiftGranVecCursor *cur, int k0, int k1, int nrows, SiftVec *dst, const uint64 *valid)
{
	const SiftChunk *c = &cur->chunk;
	int64	   *vals = cursor_num(cur);
	int			k = 0;

	want(c, is_int_enc(c->enc));
	cursor_ints(cur, k0, k1, vals);
	if (c->enc == SIFT_CENC_NUM64)
		dst->scale = (int16) *(const int64 *) c->body;
	for (int r = 0; r < nrows; r++)
	{
		bool		ok = dst->all_valid || word_bit(valid, r);

		if (dst->type == SVEC_BOOL)
			((uint8 *) dst->data)[r] = ok ? vals[k++] != 0 : 0;
		else if (dst->type == SVEC_I32)
			((int32 *) dst->data)[r] = ok ? (int32) vals[k++] : 0;
		else
			((int64 *) dst->data)[r] = ok ? vals[k++] : 0;
	}
}

static void
cursor_fill_strs(SiftGranVecCursor *cur, int k0, int k1, int nrows, SiftVec *dst, const uint64 *valid)
{
	const char **p;
	uint32	   *len;
	int			k = 0;

	cursor_strs(cur, k0, k1, &p, &len);
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
		return;
	}
	want(&cur->chunk, dst->type == SVEC_NUM128 || dst->type == SVEC_UUID || dst->type == SVEC_INTERVAL);
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

void
sift_vec_cursor_decode(SiftGranVecCursor *cur, uint32 row_off, uint16 nrows, SiftVec *dst, const SiftVariantExcStream **exc_out)
{
	const SiftChunk *c = &cur->chunk;
	int			k0,
				k1;
	uint64		local[SIFT_VEC_WORDS];
	const uint64 *valid = local;
	size_t		esz;

	if (nrows > SIFT_VEC_LEN || row_off + nrows > c->nrows)
		cursor_bad_rows(cur, row_off, nrows);
	if (row_off < cur->rank_word * 64)
		cur->rank_word = cur->rank_sum = 0;
	k0 = sift_chunk_rank_advance(c, &cur->rank_word, &cur->rank_sum, row_off);
	k1 = sift_chunk_rank_advance(c, &cur->rank_word, &cur->rank_sum, row_off + nrows);
	validity_slice(c, row_off, nrows, local);
	dst->len = nrows;
	dst->dense = true;
	dst->sel = NULL;
	dst->scale = 0;
	dst->dict = NULL;
	dst->all_valid = k1 - k0 == nrows;
	if (!dst->all_valid)
		memcpy(dst->valid, local, sizeof(local));
	else
		valid = NULL;
	if (exc_out != NULL)
		*exc_out = cursor_exc(cur);

	switch (dst->type)
	{
		case SVEC_BOOL:
		case SVEC_I32:
		case SVEC_I64:
		case SVEC_NUM64:
			cursor_fill_ints(cur, k0, k1, nrows, dst, valid);
			break;
		case SVEC_F64:
			{
				const double *vals = cursor_f8(cur, k0, k1);
				double	   *o = dst->data;
				int			k = 0;

				for (int r = 0; r < nrows; r++)
					o[r] = (dst->all_valid || word_bit(valid, r)) ? vals[k++] : 0;
			}
			break;
		case SVEC_DICT32:
			{
				int64	   *code = cursor_num(cur);
				uint32	   *o = (uint32 *) dst->data;
				int			k = 0;

				cursor_dict_codes(cur, k0, k1, code);
				for (int r = 0; r < nrows; r++)
					o[r] = (dst->all_valid || word_bit(valid, r)) ? (uint32) code[k++] : 0;
				dst->dict = &cur->dict;
			}
			break;
		default:
			cursor_fill_strs(cur, k0, k1, nrows, dst, valid);
			break;
	}
	esz = dst->type == SVEC_BOOL ? 1 : dst->type == SVEC_I32 ? 4 : dst->type == SVEC_DICT32 ? 4 : dst->type == SVEC_I64 || dst->type == SVEC_NUM64 || dst->type == SVEC_F64 ? 8 : 16;
	memset((char *) dst->data + (size_t) nrows * esz, 0, SIFT_VEC_PAD);
}

/*
 * One batch of a column on its own: the cursor that decoded it is let go but for what dst points to (the dictionary is copied out of it; the chunk, the strings and the exceptions stay
 * while a view or a stream of the batch names them, and are freed when nothing of the batch points into them)
 */
void
sift_granule_decode_vec(const SiftGranuleView *v, int col, uint16 row_off, uint16 nrows, SiftVec *dst, const SiftVariantExcStream **exc_out)
{
	SiftGranVecCursor cur;
	bool		views = dst->type == SVEC_STRVIEW || dst->type == SVEC_UUID || dst->type == SVEC_NUM128 || dst->type == SVEC_INTERVAL;

	sift_vec_cursor_open(v, col, dst->type, &cur);
	cur.windowed = true;
	sift_vec_cursor_decode(&cur, row_off, nrows, dst, exc_out);
	if (cur.dict_ready)
	{
		SiftGranuleDict *d = palloc(sizeof(SiftGranuleDict));

		*d = cur.dict;
		dst->dict = d;
		cur.dict_ready = false;
	}
	if (cur.cc != NULL)
	{
		sift_cc_close(cur.cc);
		cur.cc = NULL;
	}
	if (cur.exc == NULL)
	{
		if (views)
			sift_chunk_release_strs(&cur.chunk);
		else
			sift_chunk_close(&cur.chunk);
	}
	else if (cur.chunk.owned & SIFT_CHUNK_OWN_VALID)
	{
		pfree((void *) cur.chunk.validity);
		cur.chunk.owned &= ~SIFT_CHUNK_OWN_VALID;
	}
	cur.open = false;
	cur.exc = NULL;
	cur.exc_mask_own = NULL;
	if (views)
		cur.str_own = (SiftStrOwn) {0};
	sift_vec_cursor_close(&cur);
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


void
sift_vec_cursor_open_key_present(const SiftGranuleView *v, int col, SiftGranVecCursor *cur)
{
	const SiftChunkRef *ref;

	*cur = (SiftGranVecCursor) {0};
	if (col < 0 || col + 1 >= v->hdr->nchunks)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: column %d is not in the granule", col));
	ref = &v->hdr->chunks[col + 1];
	if (!(ref->enc & SIFT_ENC_KEY_ANY))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: column %d of the granule keeps no rows of a key", col)));
	cur->chunk.view = v;
	cur->chunk.ref = ref;
	cur->chunk.idx = col + 1;
	if (ref->enc & SIFT_ENC_KEY_ALL_ABSENT)
		cur->keyp_const = 1;
	else if (ref->enc & SIFT_ENC_KEY_ALL_PRESENT)
		cur->keyp_const = 2;
	else
	{
		sift_chunk_open(v, col + 1, &cur->chunk);
		cur->open = true;
		if (cur->chunk.keyp != NULL && cur->chunk.keyp->mode != SIFT_KP_BITMAP && cur->chunk.keyp->count > 0 &&
			((const uint16 *) (cur->chunk.keyp + 1))[cur->chunk.keyp->count - 1] >= v->hdr->nrows)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a row of a key is past the %d rows of its granule", v->hdr->nrows));
	}
}

SiftMaskState
sift_vec_cursor_key_present(SiftGranVecCursor *cur, uint16 row_off, uint16 nrows, uint64 *out_mask)
{
	const SiftChunk *c = &cur->chunk;
	int			hits = 0;

	if (nrows > SIFT_VEC_LEN || (uint32) row_off + nrows > c->view->hdr->nrows)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: rows %u..%u of column %d are not in the granule", row_off, row_off + nrows, c->idx - 1));
	if (cur->keyp_const != 0)
		return cur->keyp_const == 1 ? SIFT_MASK_ALL_ZERO : SIFT_MASK_ALL_ONE;
	if (c->ref->enc & SIFT_ENC_KEY_EQ_VALID)
		validity_slice(c, row_off, nrows, out_mask);
	else if (c->keyp->mode == SIFT_KP_BITMAP)
		bits_slice((const uint8 *) (c->keyp + 1), (c->nrows + 7) >> 3, row_off, nrows, out_mask);
	else
	{
		const uint16 *rows = (const uint16 *) (c->keyp + 1);
		uint32		lo = (cur->keyp_idx > 0 && rows[cur->keyp_idx - 1] >= row_off) ? 0 : cur->keyp_idx,
					hi = c->keyp->count;

		memset(out_mask, 0, SIFT_VEC_WORDS * sizeof(uint64));
		if (c->keyp->mode != SIFT_KP_ARRAY16)
			memset(out_mask, 0xFF, (size_t) (nrows >> 6) * 8);
		if (c->keyp->mode != SIFT_KP_ARRAY16 && (nrows & 63))
			out_mask[nrows >> 6] = (UINT64CONST(1) << (nrows & 63)) - 1;
		SIFT_BISECT(uint32, m, lo, hi, rows[m] < row_off);
		for (; lo < c->keyp->count && rows[lo] < row_off + nrows; lo++)
			out_mask[(rows[lo] - row_off) >> 6] ^= UINT64CONST(1) << ((rows[lo] - row_off) & 63);
		cur->keyp_idx = lo;
	}
	for (int w = 0; w < SIFT_VEC_WORDS; w++)
		hits += sift_pc64(out_mask[w]);
	return hits == 0 ? SIFT_MASK_ALL_ZERO : hits == nrows ? SIFT_MASK_ALL_ONE : SIFT_MASK_BITS;
}

SiftMaskState
sift_granule_decode_key_present(const SiftGranuleView *v, int col, uint16 row_off, uint16 nrows, uint64 *out_mask)
{
	SiftGranVecCursor cur;
	SiftMaskState st;

	sift_vec_cursor_open_key_present(v, col, &cur);
	st = sift_vec_cursor_key_present(&cur, row_off, nrows, out_mask);
	sift_vec_cursor_close(&cur);
	return st;
}
