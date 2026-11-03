/*
 * col/codec/codec.c - the chunk codec of the column engine: the writer (statistics, exact-size choice of the lightweight cascade,
 * general-purpose layer under the cost rule), the reader (validation, decoding, filters on the encoded form, random access), the policy
 * parser and the string and bit-vector blobs. Format and rationale:
 * The writer is in codec_write.c, the policy parser in codec_policy.c, the parser of a chunk in codec_parse.c, the blobs in
 * codec_blob.c; what they share is codec_int.h. This file holds decoding, random access and the filters on the encoded form.
 *
 * Every node is 8-byte aligned and a multiple of 8 bytes: a 16-byte header (tag, one parameter byte, a u32 count, 8 bytes of fields) and
 * its data; blobs start 8-byte aligned (the engine's chunks do), so all loads below are aligned. Little-endian hosts only, as every
 * on-disk format of pg_sift.
 */
#include "core/host.h"
#include "col/codec/decode_int.h"

void
sift_cc_set_ones(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		bits[w] = ~UINT64CONST(0);
	clear_tail(bits, n);
}

/* ---- decoding ---- */

/* The B-byte little-endian values of a FORBYTE node plus a base, wrapped to the domain: a loop per width, so the compiler vectorizes it */
#define CC_BYTES_LOOP(T, expr) \
	for (uint32 i = 0; i < n; i++) \
	{ \
		T			x; \
		memcpy(&x, s + (size_t) i * sizeof(T), sizeof(T)); \
		dst[i] = (expr); \
	}

/* A width of 3, 5, 6 or 7 bytes: one 8-byte load and a mask per value (a memcpy of B bytes is a library call per row); the last values, whose load would run past the node, are copied */
#define CC_ODD_LOOP(w, expr) \
	{ \
		const uint32 B_ = (w), \
					fast = n > 8 ? n - (8 + B_ - 1) / B_ : 0; \
		const uint64 mask = (UINT64CONST(1) << (8 * B_)) - 1; \
		uint32		i = 0; \
		for (; i < fast; i++) \
		{ \
			uint64		x; \
			memcpy(&x, s + (size_t) i * B_, 8); \
			x &= mask; \
			dst[i] = (expr); \
		} \
		for (; i < n; i++) \
		{ \
			uint64		x = 0; \
			memcpy(&x, s + (size_t) i * B_, B_); \
			dst[i] = (expr); \
		} \
	}

static void
forbyte_u32(const CcNode *nd, uint32 off, uint32 *dst)
{
	const uint8 *s = nd->words;
	uint32		n = nd->n;

	switch (nd->B)
	{
		case 1:
			CC_BYTES_LOOP(uint8, off + x) break;
		case 2:
			CC_BYTES_LOOP(uint16, off + x) break;
		case 4:
			CC_BYTES_LOOP(uint32, off + x) break;
		case 3:
			CC_ODD_LOOP(3, off + (uint32) x) break;
		default:
			for (uint32 i = 0; i < n; i++)
			{
				uint64		x = 0;

				memcpy(&x, s + (size_t) i * nd->B, nd->B);
				dst[i] = off + (uint32) x;
			}
	}
}

static void
forbyte_i64(const CcNode *nd, int64 *dst)
{
	const uint8 *s = nd->words;
	uint32		n = nd->n;
	uint64		base = (uint64) nd->base;

	switch (nd->B)
	{
		case 1:
			CC_BYTES_LOOP(uint8, (int64) (base + x)) break;
		case 2:
			CC_BYTES_LOOP(uint16, (int64) (base + x)) break;
		case 4:
			CC_BYTES_LOOP(uint32, (int64) (base + x)) break;
		case 8:
			CC_BYTES_LOOP(uint64, (int64) (base + x)) break;
		case 3:
			CC_ODD_LOOP(3, (int64) (base + x)) break;
		case 5:
			CC_ODD_LOOP(5, (int64) (base + x)) break;
		case 6:
			CC_ODD_LOOP(6, (int64) (base + x)) break;
		case 7:
			CC_ODD_LOOP(7, (int64) (base + x)) break;
		default:
			Assert(false);
	}
}

/* the u32 domain: dst[i] = value i - sub, for nodes whose values all lie in [sub, sub + 2^32); dst has CC_CAP32(n) slots */
void
nd_u32(const CcNode *nd, int64 sub, uint32 *dst)
{
	uint32		n = nd->n;
	uint32		off = (uint32) ((uint64) nd->base - (uint64) sub);

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 i = 0; i < n; i++)
				dst[i] = off;
			break;
		case SIFT_CCN_FORBIT:
			sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), off, dst);
			break;
		case SIFT_CCN_FORBYTE:
			forbyte_u32(nd, off, dst);
			break;
		case SIFT_CCN_DELTA:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_cck->prefix32(tmp, n, dst, off, (uint32) nd->mind);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_RLE:
			{
				uint32	   *rv = palloc(sizeof(uint32) * CC_CAP32(nd->nruns)),
						   *rl = palloc(sizeof(uint32) * CC_CAP32(nd->nruns));

				nd_u32(nd->a, sub, rv);
				for (uint32 r = 0; r < nd->nruns; r++)
					rl[r] = nd->rstart[r + 1] - nd->rstart[r];
				sift_cck->rle_expand32(rv, rl, nd->nruns, dst);
				pfree(rv);
				pfree(rl);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32	   *dv = palloc(sizeof(uint32) * CC_CAP32(nd->d)),
						   *codes = palloc(sizeof(uint32) * CC_CAP32(n));

				for (uint32 i = 0; i < nd->d; i++)
					dv[i] = (uint32) ((uint64) nd->dict[i] - (uint64) sub);
				nd_u32(nd->b, 0, codes);
				if (sift_cck->max32(codes, n) >= nd->d)
					CC_CORRUPT("dictionary code out of range");
				sift_colk->gather_u32(dv, codes, n, dst);
				pfree(dv);
				pfree(codes);
				break;
			}
		case SIFT_CCN_SCALE:
			nd_u32(nd->a, 0, dst);
			sift_cck->scale32(dst, n, (uint32) ((uint64) nd->base - (uint64) sub), (uint32) nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_u32(nd->a, sub, dst);
			break;
		default:
			{
				int64	   *tmp = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

				nd_i64(nd, tmp);
				for (uint32 i = 0; i < n; i++)
					dst[i] = (uint32) ((uint64) tmp[i] - (uint64) sub);
				pfree(tmp);
				break;
			}
	}
}

void
nd_i64(const CcNode *nd, int64 *dst)
{
	uint32		n = nd->n;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 i = 0; i < n; i++)
				dst[i] = nd->base;
			break;
		case SIFT_CCN_FORBIT:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_colk->for_i64[SIFT_COLW_32](tmp, n, nd->base, dst);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_FORBYTE:
			forbyte_i64(nd, dst);
			break;
		case SIFT_CCN_DELTA:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_cck->prefix64(tmp, n, dst, nd->base, nd->mind);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_RLE:
			{
				int64	   *rv = palloc(sizeof(int64) * (nd->nruns + SIFT_COL_PAD / 8));

				nd_i64(nd->a, rv);
				for (uint32 r = 0; r < nd->nruns; r++)
					for (uint32 j = nd->rstart[r]; j < nd->rstart[r + 1]; j++)
						dst[j] = rv[r];
				pfree(rv);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32	   *codes = palloc(sizeof(uint32) * CC_CAP32(n));

				nd_u32(nd->b, 0, codes);
				if (sift_cck->max32(codes, n) >= nd->d)
					CC_CORRUPT("dictionary code out of range");
				sift_colk->for_i64_sel[SIFT_COLW_64](nd->dict, codes, n, 0, dst);
				pfree(codes);
				break;
			}
		case SIFT_CCN_ALP:
			{
				uint32	   *dg = palloc(sizeof(uint32) * CC_CAP32(n));

				nd_u32(nd->a, nd->dmin, dg);
				if (nd->kind == SIFT_CC_F8)
					sift_cck->alp_keys_f8(dst, dg, n, nd->dmin, sift_cc_p10[nd->e]);
				else
					sift_cck->alp_keys_f4(dst, dg, n, nd->dmin, sift_cc_p10f[nd->e]);
				for (uint32 k = 0; k < nd->nexc; k++)
					memcpy(dst + nd->pos[k], nd->ekeys + 8 * (size_t) k, 8);
				pfree(dg);
				break;
			}
		case SIFT_CCN_SCALE:
			nd_i64(nd->a, dst);
			sift_cck->scale64(dst, n, nd->base, nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_i64(nd->a, dst);
			break;
	}
}

void
sift_cc_decode(const SiftCc *c, int64 *dst)
{
	if (c->n)
		nd_i64(c->root, dst);
}

/* The first w keys of a node: the nodes that store a row after another (a constant, frame of reference, delta) decode w rows and no more, the others all of them */
static void
nd_i64_head(const CcNode *nd, int64 *dst, uint32 w)
{
	CcNode		h;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
		case SIFT_CCN_FORBIT:
		case SIFT_CCN_FORBYTE:
		case SIFT_CCN_DELTA:
			h = *nd;
			h.n = w;
			nd_i64(&h, dst);
			break;
		case SIFT_CCN_SCALE:
			nd_i64_head(nd->a, dst, w);
			sift_cck->scale64(dst, w, nd->base, nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_i64_head(nd->a, dst, w);
			break;
		default:
			{
				int64	   *all = palloc(sizeof(int64) * ((size_t) nd->n + SIFT_COL_PAD / 8));

				nd_i64(nd, all);
				memcpy(dst, all, sizeof(int64) * w);
				pfree(all);
			}
	}
}

void
sift_cc_decode_head(const SiftCc *c, uint32 w, int64 *dst)
{
	if (w >= c->n)
		sift_cc_decode(c, dst);
	else if (w > 0)
		nd_i64_head(c->root, dst, w);
}

/* The doubles of the keys: an ALP node is divided straight into doubles (its exceptions are keys), any other node decodes the keys and turns them in place */
static void
nd_f8(const CcNode *nd, double *dst)
{
	uint32		n = nd->n;

	if (nd->tag == SIFT_CCN_WRAP)
		nd_f8(nd->a, dst);
	else if (nd->tag == SIFT_CCN_ALP)
	{
		uint32	   *dg = palloc(sizeof(uint32) * CC_CAP32(n));

		nd_u32(nd->a, nd->dmin, dg);
		if (nd->kind == SIFT_CC_F8)
			sift_cck->alp_f8(dst, dg, n, nd->dmin, sift_cc_p10[nd->e]);
		else
			sift_cck->alp_f4(dst, dg, n, nd->dmin, sift_cc_p10f[nd->e]);
		for (uint32 k = 0; k < nd->nexc; k++)
			dst[nd->pos[k]] = sift_col_f8_from_key((int64) get64(nd->ekeys + 8 * (size_t) k));
		pfree(dg);
	}
	else
	{
		nd_i64(nd, (int64 *) dst);
		sift_colk->f8_from_keys((int64 *) dst, n);
	}
}

void
sift_cc_decode_f8(const SiftCc *c, double *dst)
{
	if (c->n)
		nd_f8(c->root, dst);
}

void
sift_cc_decode_u32(const SiftCc *c, uint32 *dst)
{
	if (c->n)
		nd_u32(c->root, 0, dst);
}

bool
sift_cc_decode_off32(const SiftCc *c, uint32 *dst)
{
	if (!(c->flags & SIFT_CCF_OFF32))
		return false;
	if (c->n)
		nd_u32(c->root, c->min, dst);
	return true;
}
