/*
 * tests/vk_v6_gen.c - the chunks of the tests: the real column codec (col/codec.h of the integration tree) encodes the values, the split-bits
 * doubles (ALP_RD) have an encoder of their own here (the layout of granule_enc.c put_alp_rd), and the record gets its validity, and an LZ4
 * wrapper when asked, as a granule stores a chunk. cpu_decode is the decoder of the codec on the same bytes, the oracle of the device's.
 */
#include "postgres.h"

#include <lz4.h>

#include "col/codec.h"
#include "ccshim.h"
#include "vk_v6_common.h"

#define MAXALIGN8(x) (((x) + 7) & ~(size_t) 7)

int			failures = 0;
uint64_t	cov_root[16];
uint64_t	cov_inner[16];

void
codec_init(void)
{
	cc_init();
}

void
td_init(TData *d, uint32_t ngran, uint32_t ncols, uint32_t rows)
{
	memset(d, 0, sizeof(*d));
	d->ngran = ngran;
	d->ncols = ncols;
	d->g = calloc(ngran, sizeof(TGran));
	d->vg = calloc(ngran, sizeof(SiftVkGranule));
	for (uint32_t i = 0; i < ngran; i++)
	{
		d->g[i].id = i;
		d->g[i].nrows = rows;
		for (uint32_t c = 0; c < ncols; c++)
		{
			d->g[i].col[c].v = calloc(SIFT_VK_GRANULE_ROWS, 8);
			d->g[i].col[c].ok = calloc(SIFT_VK_GRANULE_ROWS, 1);
		}
	}
}

void
td_free(TData *d)
{
	for (uint32_t i = 0; i < d->ngran; i++)
		for (uint32_t c = 0; c < SIFT_VK_MAXCOLS; c++)
		{
			free(d->g[i].col[c].v);
			free(d->g[i].col[c].ok);
			free(d->g[i].buf[c]);
		}
	free(d->g);
	free(d->vg);
	memset(d, 0, sizeof(*d));
}

void
td_publish(TData *d)
{
	static uint64_t serial;

	serial++;
	for (uint32_t i = 0; i < d->ngran; i++)
	{
		TGran	   *g = &d->g[i];

		d->vg[i].id = g->id;
		d->vg[i].nrows = g->nrows;
		d->vg[i].dead = g->has_dead ? g->dead : NULL;
		for (uint32_t c = 0; c < SIFT_VK_MAXCOLS; c++)
			d->vg[i].col[c] = g->used[c] ? &g->chunk[c] : NULL;
		for (uint32_t c = 0; c < SIFT_VK_MAXCOLS; c++)
			g->chunk[c].key.part_gen = serial;
	}
}

/* ---- the bit streams of ALP_RD: bit i of the stream is bit (i & 7) of byte (i >> 3) ---- */
static void
put_bits(uint8_t *p, uint64_t at, uint64_t v, int w)
{
	for (int i = 0; i < w; i++)
		if ((v >> i) & 1)
			p[(at + i) >> 3] |= (uint8_t) (1u << ((at + i) & 7));
}

static uint64_t
get_bits(const uint8_t *p, uint64_t at, int w)
{
	uint64_t	v = 0;

	for (int i = 0; i < w; i++)
		v |= (uint64_t) ((p[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
	return v;
}

/* the body of a split-bits chunk of the doubles; false when no split of at most 8 distinct left parts leaves at most nn / 16 exceptions */
static bool
put_alp_rd(uint8_t **out, size_t *len, const double *vals, uint32_t nn, int tz)
{
	for (int lb = 16; lb >= 8; lb--)
	{
		uint16_t	dict[8] = {0};
		uint32_t	cnt[8] = {0},
					nd = 0,
					n_exc = 0;
		int			rb = 64 - lb - tz;

		if (rb < 1 || rb > 52)
			continue;
		for (uint32_t i = 0; i < nn; i++)
		{
			uint64_t	u;
			uint16_t	left;
			uint32_t	d;

			memcpy(&u, &vals[i], 8);
			left = (uint16_t) (u >> (64 - lb));
			for (d = 0; d < nd && dict[d] != left; d++)
				;
			if (d == nd && nd < 8)
			{
				dict[nd] = left;
				cnt[nd++] = 0;
			}
			if (d < nd)
				cnt[d]++;
			else
				n_exc++;
		}
		if (n_exc > nn / 16)
			continue;
		{
			size_t		clen = MAXALIGN8(((size_t) nn * 3 + 7) / 8),
						rlen = MAXALIGN8(((size_t) nn * rb + 7) / 8),
						elen = MAXALIGN8((size_t) n_exc * 4);
			uint8_t    *b = calloc(1, 24 + clen + rlen + elen + 8);
			uint16_t   *ep = malloc((n_exc + 1) * 2),
					   *el = malloc((n_exc + 1) * 2);
			uint32_t	x = 0;

			b[0] = (uint8_t) lb;
			b[1] = (uint8_t) nd;
			b[2] = (uint8_t) tz;
			b[3] = n_exc ? 1 : 0;
			memcpy(b + 4, &n_exc, 4);
			memcpy(b + 8, dict, 16);
			for (uint32_t i = 0; i < nn; i++)
			{
				uint64_t	u;
				uint16_t	left;
				uint32_t	d;

				memcpy(&u, &vals[i], 8);
				left = (uint16_t) (u >> (64 - lb));
				for (d = 0; d < nd && dict[d] != left; d++)
					;
				if (d == nd)
				{
					ep[x] = (uint16_t) i;
					el[x++] = left;
					d = 0;
				}
				put_bits(b + 24, (uint64_t) i * 3, d, 3);
				put_bits(b + 24 + clen, (uint64_t) i * rb, (u >> tz) & ((UINT64_C(1) << rb) - 1), rb);
			}
			memcpy(b + 24 + clen + rlen, ep, n_exc * 2);
			memcpy(b + 24 + clen + rlen + n_exc * 2, el, n_exc * 2);
			free(ep);
			free(el);
			*out = b;
			*len = 24 + clen + rlen + elen;
			return true;
		}
	}
	return false;
}

static void
alp_rd_decode(const uint8_t *body, uint32_t nn, double *out)
{
	int			lb = body[0],
				nd = body[1],
				tz = body[2],
				rb = 64 - lb - tz;
	uint32_t	n_exc;
	uint16_t	dict[8];
	size_t		clen = MAXALIGN8(((size_t) nn * 3 + 7) / 8),
				rlen = MAXALIGN8(((size_t) nn * rb + 7) / 8);
	const uint8_t *codes = body + 24,
			   *rights = codes + clen,
			   *exc = rights + rlen;
	uint32_t	x = 0;

	memcpy(&n_exc, body + 4, 4);
	memcpy(dict, body + 8, 16);
	(void) nd;
	for (uint32_t i = 0; i < nn; i++)
	{
		uint64_t	left = dict[get_bits(codes, (uint64_t) i * 3, 3)],
					u;
		uint16_t	pos;

		if (x < n_exc)
		{
			memcpy(&pos, exc + 2 * (size_t) x, 2);
			if (pos == i)
			{
				uint16_t	l;

				memcpy(&l, exc + 2 * (size_t) n_exc + 2 * (size_t) x, 2);
				left = l;
				x++;
			}
		}
		u = ((left << rb) | get_bits(rights, (uint64_t) i * rb, rb)) << tz;
		memcpy(&out[i], &u, 8);
	}
}

/* the block of the codec for the keys: [u32 length][u32 0][blob] */
static bool
cc_block(uint8_t **out, size_t *len, SiftCcKind kind, const int64_t *keys, uint32_t n, const char *policy, const char **why)
{
	SiftCcPolicy pol;
	StringInfoData si;
	SiftCcInfo	info;
	const char *err = sift_cc_policy_parse(policy, &pol);
	uint8_t    *b;

	if (err)
	{
		*why = err;
		return false;
	}
	initStringInfo(&si);
	sift_cc_encode(&si, kind, keys, n, NULL, &pol, &info);
	cov_root[info.tag & 15]++;
	cov_inner[info.inner & 15]++;
	b = calloc(1, 8 + MAXALIGN8(si.len) + 8);
	memcpy(b, &si.len, 4);
	memcpy(b + 8, si.data, si.len);
	*out = b;
	*len = 8 + MAXALIGN8(si.len);
	pfree(si.data);
	return true;
}

bool
build_chunk(TGran *g, uint32_t c, const ChunkSpec *spec, uint32_t part_id)
{
	uint32_t	nrows = g->nrows,
				nn = 0,
				nnull;
	int64_t    *cv = malloc((nrows + 1) * 8);
	size_t		vlen = 0,
				blen = 0,
				rawlen;
	uint8_t    *vb = NULL,
			   *body = NULL,
			   *raw;
	uint32_t	flags = 0;
	SiftVkChunk *ch = &g->chunk[c];
	int			nm = spec->nulls;
	const char *why = NULL;
	bool		fl = spec->enc == SIFT_VK_ENC_ALP || spec->enc == SIFT_VK_ENC_ALP_RD || spec->enc == SIFT_VK_ENC_FLOAT_RAW;
	int64_t		mn = INT64_MAX,
				mx = INT64_MIN;

	for (uint32_t r = 0; r < nrows; r++)
		if (g->col[c].ok[r])
		{
			cv[nn++] = g->col[c].v[r];
			mn = g->col[c].v[r] < mn ? g->col[c].v[r] : mn;
			mx = g->col[c].v[r] > mx ? g->col[c].v[r] : mx;
		}
	nnull = nrows - nn;
	memset(ch, 0, sizeof(*ch));
	if (nm == NULLS_AUTO)
		nm = nnull == 0 ? -1 : NULLS_BITMAP;
	if (nn == 0)
		flags |= SIFT_VK_CF_ALL_NULL;
	else if (nm == -1)
		flags |= SIFT_VK_CF_ALL_VALID;
	else if (nm == NULLS_BITMAP)
	{
		vlen = MAXALIGN8((nrows + 7) / 8);
		vb = calloc(1, vlen + 8);
		for (uint32_t r = 0; r < nrows; r++)
			if (g->col[c].ok[r])
				vb[r >> 3] |= (uint8_t) (1u << (r & 7));
	}
	else if (nm == NULLS_SPARSE_NULL || nm == NULLS_SPARSE_VALUE)
	{
		bool		nl = nm == NULLS_SPARSE_NULL;
		uint32_t	listed = nl ? nnull : nn,
					k = 0;
		uint16_t	cnt;

		if (listed > nrows / 16)
		{
			free(cv);
			((ChunkSpec *) spec)->why = "too many rows for a sparse list";
			return false;
		}
		vlen = MAXALIGN8(2 + 2 * (size_t) listed);
		vb = calloc(1, vlen + 8);
		cnt = (uint16_t) listed;
		memcpy(vb, &cnt, 2);
		for (uint32_t r = 0; r < nrows; r++)
			if (g->col[c].ok[r] != nl)
			{
				uint16_t	row = (uint16_t) r;

				memcpy(vb + 2 + 2 * k++, &row, 2);
			}
		flags |= nl ? SIFT_VK_CF_NULL_SPARSE : SIFT_VK_CF_NONNULL_SPARSE;
	}
	if (nn && spec->enc == SIFT_VK_ENC_INT_CONST)
	{
		if (mn != mx)
		{
			free(cv);
			free(vb);
			((ChunkSpec *) spec)->why = "not a constant";
			return false;
		}
		blen = 8;
		body = calloc(1, 16);
		memcpy(body, &mn, 8);
	}
	else if (nn && spec->enc == SIFT_VK_ENC_BOOL_BITS)
	{
		blen = MAXALIGN8((nn + 7) / 8);
		body = calloc(1, blen + 8);
		for (uint32_t k = 0; k < nn; k++)
			if (cv[k])
				body[k >> 3] |= (uint8_t) (1u << (k & 7));
	}
	else if (nn && spec->enc == SIFT_VK_ENC_FLOAT_RAW)
	{
		blen = (size_t) nn * 8;
		body = calloc(1, blen + 8);
		for (uint32_t k = 0; k < nn; k++)
		{
			double		d = f8_from_key(cv[k]);

			memcpy(body + 8 * (size_t) k, &d, 8);
		}
	}
	else if (nn && spec->enc == SIFT_VK_ENC_ALP_RD)
	{
		double	   *dv = malloc(nn * 8);

		for (uint32_t k = 0; k < nn; k++)
			dv[k] = f8_from_key(cv[k]);
		if (!put_alp_rd(&body, &blen, dv, nn, spec->tz))
			why = "no split-bits layout fits these doubles";
		free(dv);
	}
	else if (nn && (spec->enc == SIFT_VK_ENC_NUM64 || spec->enc == SIFT_VK_ENC_ALP || spec->enc == SIFT_VK_ENC_INT_FOR || spec->enc == SIFT_VK_ENC_INT_DELTA ||
					spec->enc == SIFT_VK_ENC_RLE || spec->enc == SIFT_VK_ENC_DICT_CODE))
	{
		uint8_t    *blk;
		size_t		bl;
		size_t		skip = spec->enc == SIFT_VK_ENC_NUM64 ? 8 : 0;

		if (!cc_block(&blk, &bl, spec->enc == SIFT_VK_ENC_ALP ? SIFT_CC_F8 : SIFT_CC_INT, cv, nn, spec->policy, &why))
			blk = NULL;
		if (blk)
		{
			body = calloc(1, skip + bl + 8);
			if (skip)
			{
				uint64_t	scale = 2;

				memcpy(body, &scale, 8);
			}
			memcpy(body + skip, blk, bl);
			blen = skip + bl;
			free(blk);
		}
	}
	free(cv);
	if (nn && !body)
	{
		free(vb);
		((ChunkSpec *) spec)->why = why ? why : "unknown encoding";
		return false;
	}
	rawlen = vlen + blen;
	raw = calloc(1, rawlen + 8);
	if (vlen)
		memcpy(raw, vb, vlen);
	if (blen)
		memcpy(raw + vlen, body, blen);
	free(vb);
	free(body);
	if (spec->lz4 && rawlen)
	{
		int			cap = LZ4_compressBound((int) rawlen);
		uint8_t    *z = calloc(1, 8 + (size_t) cap + 8);
		int			zl = LZ4_compress_default((const char *) raw, (char *) z + 8, (int) rawlen, cap);
		uint32_t	alg = 1,
					rl = (uint32_t) rawlen;

		memcpy(z, &alg, 4);
		memcpy(z + 4, &rl, 4);
		free(raw);
		raw = z;
		ch->len = (uint32_t) (8 + zl);
		flags |= SIFT_VK_CF_LZ4;
	}
	else
		ch->len = (uint32_t) rawlen;
	free(g->buf[c]);
	g->buf[c] = raw;
	ch->bytes = raw;
	ch->key = (SiftVkKey) {.rel = 1, .part_id = part_id, .col_id = c, .part_gen = 1, .granule_id = g->id};
	ch->enc = nn ? spec->enc : SIFT_VK_ENC_ALL_NULL;
	ch->flags = flags;
	ch->nrows = nrows;
	ch->nnonnull = nn;
	ch->vlen = (uint32_t) vlen;
	ch->rawlen = (uint32_t) rawlen;
	if (nn == 0)
		ch->w = 0;
	else if (fl)
		ch->w = 8;
	else
	{
		uint64_t	range = (uint64_t) mx - (uint64_t) mn;

		ch->base = mn;
		ch->w = (spec->w0 && mn == mx) ? 0 : range <= 0xFF ? 1 : range <= 0xFFFF ? 2 : range <= 0xFFFFFFFFu ? 4 : 8;
	}
	snprintf(g->desc[c], sizeof(g->desc[c]), "%s enc %u nulls %d lz4 %d w %u", spec->policy ? spec->policy : "-", ch->enc, spec->nulls, spec->lz4, ch->w);
	g->used[c] = true;
	return true;
}

bool
cpu_decode(const SiftVkChunk *ch, int64_t *vals, uint8_t *valid)
{
	size_t		raw_len = ch->rawlen;
	uint8_t    *raw = aligned_alloc(64, ((ch->rawlen ? ch->rawlen : 8) + 63) / 64 * 64 + 64);
	uint32_t	nrows = ch->nrows,
				nn = ch->nnonnull;
	int64_t    *cv = calloc(nn + 128, 8);
	const uint8_t *body;
	uint32_t	k = 0;

	if (ch->flags & SIFT_VK_CF_LZ4)
	{
		if (LZ4_decompress_safe((const char *) ch->bytes + 8, (char *) raw, (int) (ch->len - 8), (int) raw_len) != (int) raw_len)
			return false;
	}
	else
		memcpy(raw, ch->bytes, ch->len);
	for (uint32_t r = 0; r < nrows; r++)
	{
		if (ch->flags & SIFT_VK_CF_ALL_NULL)
			valid[r] = 0;
		else if (ch->flags & SIFT_VK_CF_ALL_VALID)
			valid[r] = 1;
		else if (ch->flags & SIFT_VK_CF_NULL_SPARSE)
			valid[r] = 1;
		else if (ch->flags & SIFT_VK_CF_NONNULL_SPARSE)
			valid[r] = 0;
		else
			valid[r] = (raw[r >> 3] >> (r & 7)) & 1;
	}
	if (ch->flags & (SIFT_VK_CF_NULL_SPARSE | SIFT_VK_CF_NONNULL_SPARSE))
	{
		uint16_t	cnt;

		memcpy(&cnt, raw, 2);
		for (uint32_t i = 0; i < cnt; i++)
		{
			uint16_t	row;

			memcpy(&row, raw + 2 + 2 * i, 2);
			valid[row] = (ch->flags & SIFT_VK_CF_NONNULL_SPARSE) ? 1 : 0;
		}
	}
	body = raw + ch->vlen;
	if (nn)
	{
		if (ch->enc == SIFT_VK_ENC_INT_CONST)
			for (uint32_t i = 0; i < nn; i++)
				memcpy(&cv[i], body, 8);
		else if (ch->enc == SIFT_VK_ENC_BOOL_BITS)
			for (uint32_t i = 0; i < nn; i++)
				cv[i] = (body[i >> 3] >> (i & 7)) & 1;
		else if (ch->enc == SIFT_VK_ENC_FLOAT_RAW)
			for (uint32_t i = 0; i < nn; i++)
			{
				double		d;

				memcpy(&d, body + 8 * (size_t) i, 8);
				cv[i] = f8_key(d);
			}
		else if (ch->enc == SIFT_VK_ENC_ALP_RD)
		{
			double	   *dv = calloc(nn + 8, 8);

			alp_rd_decode(body, nn, dv);
			for (uint32_t i = 0; i < nn; i++)
				cv[i] = f8_key(dv[i]);
			free(dv);
		}
		else
		{
			const uint8_t *blk = body + (ch->enc == SIFT_VK_ENC_NUM64 ? 8 : 0);
			uint32_t	len;
			SiftCc	   *cc;

			memcpy(&len, blk, 4);
			cc = sift_cc_open_rows(CurrentMemoryContext, blk + 8, len, true, nn);
			sift_cc_decode(cc, cv);
			sift_cc_close(cc);
		}
	}
	for (uint32_t r = 0; r < nrows; r++)
		vals[r] = valid[r] ? cv[k++] : 0;
	free(cv);
	free(raw);
	return k == nn;
}
