/*
 * col/enc.c - L1 of the column engine: schema, dictionaries, batches, column chunks, zones,
 * chunk tests and unit records. Values and bytes only; the segment writer and reader (col/seg/seg.c) and the sources of rows sit above.
 *
 *   1. schema         2. dictionaries      3. batches
 *   4. zones          5. codec seam        6. chunks
 *   7. chunk tests    8. units
 * The other sections are in schema.c, dict.c, batch.c, chunk.c, ctest.c and unit.c; what they share is in enc_int.h.
 */

#include "core/host.h"

#include "col/enc.h"
#include "col/enc_int.h"

/* Appends n zero bytes */
static void
put_zero(StringInfo out, Size n)
{
	enlargeStringInfo(out, (int) n);
	memset(out->data + out->len, 0, n);
	out->len += (int) n;
	out->data[out->len] = '\0';
}

static void
put_pad(StringInfo out, Size align)
{
	put_zero(out, (align - (Size) out->len % align) % align);
}

/* ---------------------------------------------------------------------------
 * 4. Zones
 * ------------------------------------------------------------------------- */

/* The one definition of a zone: the encoder and the verification of a stored chunk both come through here */
static void
zone_of_keys(const int64 *key, const uint64 *nulls, const uint64 *ovf, uint32 n, SiftColStore store, SiftCZone *z)
{
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN,
				carry = 0;		/* the sum is carry * 2^64 + sum (unsigned): exact, whatever the order of the additions */
	uint64		sum = 0;
	uint32		nnull = 0;
	bool		nosum;

	memset(z, 0, sizeof(*z));
	z->nvals = n;
	for (uint32 i = 0; i < n; i++)
	{
		int64		k = key[i];

		if (sift_col_bit_test(nulls, i))
		{
			nnull++;
			continue;
		}
		mn = Min(mn, k);
		mx = Max(mx, k);
		if (store == SIFT_COLS_INT || store == SIFT_COLS_VAR)		/* VAR: the keys are the lengths, their zone is no use but is exact */
		{
			uint64		before = sum;

			sum += (uint64) k;
			carry += (sum < before) - (k < 0);
		}
		else if (store == SIFT_COLS_STR)
			z->set[(k & 255) >> 6] |= UINT64CONST(1) << (k & 63);
		else
		{
			double		d = sift_col_f8_from_key(k);

			z->sum_f += d;
			z->flags |= d != d ? SIFT_COLZ_NAN : 0;
		}
	}
	z->nnull = nnull;
	if (nnull < n)
	{
		z->min = mn;
		z->max = mx;
		z->flags |= mn == mx ? SIFT_COLZ_CONST : 0;
	}
	nosum = !((carry == 0 && (int64) sum >= 0) || (carry == -1 && (int64) sum < 0));	/* the sum is outside int64 */
	z->sum_i = nosum ? 0 : (int64) sum;
	z->flags |= nosum ? SIFT_COLZ_NOSUM : 0;
	for (uint32 w = 0; ovf != NULL && w < words_of(n); w++)
		z->flags |= ovf[w] ? SIFT_COLZ_OVERFLOW : 0;
}

/* SIFT_COLZ_JNULL when the kinds mask (bit t: some slot is of kind t) has the JSON null */
static inline uint32
zone_jnull(uint32 kmask)
{
	return (kmask >> SIFT_COLTAG_JNULL & 1) ? SIFT_COLZ_JNULL : 0;
}

/* The zone of an inline STR chunk: the prefix keys of the smallest and largest value; no set, so none prunes */
static void
istr_zone(const uint64 *nulls, const uint64 *ovf, uint32 n, const uint32 *off, const char *bytes, SiftCZone *z)
{
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN;

	memset(z, 0, sizeof(*z));
	z->nvals = n;
	for (uint32 i = 0; i < n; i++)
	{
		int64		k;

		if (sift_col_bit_test(nulls, i))
		{
			z->nnull++;
			continue;
		}
		k = sift_col_str_prefix(bytes + off[i], off[i + 1] - off[i]);
		mn = Min(mn, k);
		mx = Max(mx, k);
	}
	if (z->nnull < n)
	{
		z->min = mn;
		z->max = mx;
	}
	z->flags = SIFT_COLZ_INLINE;
	for (uint32 w = 0; ovf != NULL && w < words_of(n); w++)
		z->flags |= ovf[w] ? SIFT_COLZ_OVERFLOW : 0;
	memset(z->set, 0xFF, sizeof(z->set));
}

void
sift_cchunk_zone(const SiftCChunk *c, SiftColStore store, SiftCZone *z)
{
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	if (c->enc == SIFT_COLENC_ISTR)
	{
		uint32	   *off;
		char	   *bytes;

		sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);	/* verified by its decoding */
		istr_zone(c->nulls, c->ovf, c->n, off, bytes, z);
		pfree(off);
		pfree(bytes);
	}
	else
	{
		if (c->enc == SIFT_COLENC_CC)
			sift_cc_close(cc_open(c, true));
		sift_cchunk_decode(c, key);	/* a STRS chunk is verified by its decoding */
		zone_of_keys(key, c->nulls, c->ovf, c->n, store, z);
	}
	z->flags |= zone_jnull(c->kmask);
	pfree(key);
}

/* ---------------------------------------------------------------------------
 * 6. Chunks
 * ------------------------------------------------------------------------- */

/* The smallest encoding that holds values 0..span */
static void
codec_choose(uint64 span, uint32 n, uint8 *enc, uint8 *bits)
{
	uint8		w = span == 0 ? 0 : pg_leftmost_one_pos64(span) + 1,
				fw = w <= 8 ? 8 : w <= 16 ? 16 : w <= 32 ? 32 : 64;

	*enc = SIFT_COLENC_FOR;
	*bits = fw;
	if (w == 0)
	{
		*enc = SIFT_COLENC_CONST;
		*bits = 0;
	}
	else if (codec_valid(SIFT_COLENC_PACK, w) && codec_paylen(SIFT_COLENC_PACK, w, n) < codec_paylen(SIFT_COLENC_FOR, fw, n))
	{
		*enc = SIFT_COLENC_PACK;
		*bits = w;
	}
}

/* Appends the payload of the n unsigned values u[] (each below 2^bits) */
static void
codec_put(StringInfo out, uint8 enc, uint8 bits, const uint64 *u, uint32 n)
{
	Size		len = codec_paylen(enc, bits, n);
	uint8	   *dst;

	enlargeStringInfo(out, (int) len);
	dst = (uint8 *) out->data + out->len;
	if (enc == SIFT_COLENC_FOR)
		for (uint32 i = 0; i < n; i++)
		{
			switch (bits)
			{
				case 8:
					dst[i] = (uint8) u[i];
					break;
				case 16:
					{
						uint16		x = (uint16) u[i];

						memcpy(dst + 2 * i, &x, sizeof(x));
					}
					break;
				case 32:
					{
						uint32		x = (uint32) u[i];

						memcpy(dst + 4 * i, &x, sizeof(x));
					}
					break;
				default:
					memcpy(dst + 8 * i, &u[i], sizeof(uint64));
			}
		}
	else
		for (uint32 s = 0; s < words_of(n) * 64; s += 64)
		{
			uint64		acc = 0;
			int			nb = 0;

			for (uint32 i = s; i < s + 64; i++)
			{
				acc |= (i < n ? u[i] : 0) << nb;
				nb += bits;
				if (nb >= 32)
				{
					uint32		x = (uint32) acc;

					memcpy(dst, &x, sizeof(x));
					dst += 4;
					acc >>= 32;
					nb -= 32;
				}
			}
		}
	out->len += (int) len;
	out->data[out->len] = '\0';
}

/* Appends the bitmap of n bits (NULL: all clear), tail bits cleared */
static void
put_bitmap(StringInfo out, const uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
	{
		uint64		x = bits ? bits[w] : 0;

		if (w == n / 64)
			x &= (UINT64CONST(1) << (n % 64)) - 1;
		put_val(out, x);
	}
}

/* Appends the tag nibble of every overflow slot of the leaf (SIFT_COLC_TAGS), in slot order */
static void
put_tags(StringInfo out, const SiftCVals *vals)
{
	Size		at = out->len;
	uint32		k = 0;

	put_zero(out, tags_bytes(bitmap_count(vals->ovf, vals->n)));
	for (uint32 w = 0; w < words_of(vals->n); w++)
		for (uint64 m = vals->ovf[w]; m; m &= m - 1, k++)
			if (vals->otag != NULL)
				out->data[at + k / 2] |= (char) (vals->otag[w * 64 + pg_rightmost_one_pos64(m)] << (k % 2 * 4));
}

/* The kinds of the leaf: bit t set when some slot that does not overflow has tag t */
static uint32
kinds_mask(const SiftCVals *vals)
{
	uint32		mask = 0;

	for (uint32 i = 0; vals->otag != NULL && i < vals->n; i++)
		if (!sift_col_bit_test(vals->ovf, i))
			mask |= 1U << vals->otag[i];
	return mask & ~(1U | (1U << SIFT_COLTAG_UNSURE));
}

/* Appends the kinds (SIFT_COLC_KINDS): the word of the tags that have a bitmap and the bitmaps */
static void
put_kinds(StringInfo out, const SiftCVals *vals, uint32 mask)
{
	uint64		head = mask;
	uint64	   *bits = palloc(words_of(vals->n) * sizeof(uint64));

	put_bytes(out, &head, sizeof(head));
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
	{
		if (!(mask >> tag & 1))
			continue;
		memset(bits, 0, words_of(vals->n) * sizeof(uint64));
		for (uint32 i = 0; i < vals->n; i++)
			if (vals->otag[i] == tag && !sift_col_bit_test(vals->ovf, i))
				bits[i / 64] |= UINT64CONST(1) << (i % 64);
		put_bitmap(out, bits, vals->n);
	}
	pfree(bits);
}

/* The flags of a chunk of the leaf: NULLs and overflow follow the zone, and the overflow comes with its tags and the kinds (kmask) when the unit is written with them */
static uint8
chunk_flags(const SiftCZone *z, bool tags, uint32 kmask)
{
	bool		ovf = (z->flags & SIFT_COLZ_OVERFLOW) != 0;

	return (z->nnull > 0 ? SIFT_COLC_NULLS : 0) | (ovf ? SIFT_COLC_OVF : 0) | (ovf && tags ? SIFT_COLC_TAGS : 0) | (kmask ? SIFT_COLC_KINDS : 0);
}

/* Appends the chunk of n keys (NULL slots hold 0) and returns its zone; pol: the codec's policy of the leaf, NULL for the plain form */
static void
chunk_encode(StringInfo out, SiftColStore store, const SiftCVals *vals, const int64 *key, const SiftCcPolicy *pol, bool tags, SiftCZone *z)
{
	const uint64 *nulls = vals->nulls,
			   *ovf = vals->ovf;
	uint32		n = vals->n;
	uint8		enc,
				bits,
				flags;
	uint32		kmask = tags ? kinds_mask(vals) : 0;
	uint64		span,
			   *u;
	int64		base;
	uint8		head[8];

	zone_of_keys(key, nulls, ovf, n, store, z);
	z->flags |= zone_jnull(kmask);
	flags = chunk_flags(z, tags, kmask);
	span = (uint64) z->max - (uint64) z->min;
	codec_choose(span, n, &enc, &bits);
	if (pol != NULL && enc != SIFT_COLENC_CONST)
		enc = SIFT_COLENC_CC, bits = 0;
	if (store == SIFT_COLS_VAR)
		enc = SIFT_COLENC_STRS, bits = 0;
	head[0] = enc;
	head[1] = bits;
	head[2] = flags;
	head[3] = 0;
	memcpy(head + 4, &n, sizeof(n));
	put_bytes(out, head, sizeof(head));
	base = enc == SIFT_COLENC_CC || enc == SIFT_COLENC_STRS ? 0 : z->min;
	put_val(out, base);
	if (flags & SIFT_COLC_NULLS)
		put_bitmap(out, nulls, n);
	if (flags & SIFT_COLC_OVF)
		put_bitmap(out, ovf, n);
	if (flags & SIFT_COLC_TAGS)
		put_tags(out, vals);
	if (flags & SIFT_COLC_KINDS)
		put_kinds(out, vals, kmask);
	if (enc == SIFT_COLENC_STRS)
	{
		SiftCcPolicy dp;

		if (pol == NULL)
			sift_cc_policy_default(&dp);
		sift_cc_encode_strs(out, vals->voff, vals->vb, n, pol ? pol : &dp, NULL);
	}
	else if (enc == SIFT_COLENC_CC)
		sift_cc_encode(out, store == SIFT_COLS_F4 ? SIFT_CC_F4 : store == SIFT_COLS_F8 ? SIFT_CC_F8 : SIFT_CC_INT, key, n, nulls, pol, NULL);
	else if (enc != SIFT_COLENC_CONST)
	{
		u = palloc(Max(n, 1) * sizeof(uint64));
		for (uint32 i = 0; i < n; i++)
			u[i] = sift_col_bit_test(nulls, i) ? 0 : (uint64) key[i] - (uint64) z->min;
		codec_put(out, enc, bits, u, n);
		pfree(u);
	}
	put_pad(out, 8);
}

/* The chunk of an STR leaf stored inline: the strings of the rows (the batch's codes looked up in its dictionary bd), no codes of the segment */
/* set by the host while it builds an index, to name the leaf whose strings the formula wrapped otherwise than the cold rule (col/build.c) */
void		(*sift_cenc_scan_hook) (int leaf, int kind, const SiftCcScanSum *chosen, const SiftCcScanSum *kept);

static void
chunk_encode_istr(StringInfo out, const SiftCVals *v, const SiftCDict *bd, const SiftCcPolicy *pol, bool tags, SiftCZone *z, SiftCcScan *scan)
{
	uint32		n = v->n;
	uint32	   *off = palloc((Size) (n + 1) * sizeof(uint32));
	StringInfoData bytes;
	SiftCcPolicy dp;
	uint8		head[8] = {SIFT_COLENC_ISTR, 0, 0, 0};
	uint32		kmask = tags ? kinds_mask(v) : 0;

	initStringInfo(&bytes);
	off[0] = 0;
	for (uint32 i = 0; i < n; i++)
	{
		if (!sift_col_bit_test(v->nulls, i))
		{
			uint32		len;
			const char *s = sift_cdict_str(bd, (uint32) v->key[i], &len);

			appendBinaryStringInfo(&bytes, s, (int) len);
		}
		off[i + 1] = (uint32) bytes.len;
	}
	istr_zone(v->nulls, v->ovf, n, off, bytes.data, z);
	z->flags |= zone_jnull(kmask);
	head[2] = chunk_flags(z, tags, kmask);
	memcpy(head + 4, &n, sizeof(n));
	put_bytes(out, head, sizeof(head));
	put_zero(out, sizeof(int64));		/* base */
	if (head[2] & SIFT_COLC_NULLS)
		put_bitmap(out, v->nulls, n);
	if (head[2] & SIFT_COLC_OVF)
		put_bitmap(out, v->ovf, n);
	if (head[2] & SIFT_COLC_TAGS)
		put_tags(out, v);
	if (head[2] & SIFT_COLC_KINDS)
		put_kinds(out, v, kmask);
	if (pol == NULL)
		sift_cc_policy_default(&dp);
	{
		int64	   *pre = palloc((Size) (n + 1) * sizeof(int64));
		StringInfoData pb;
		uint32		plen;

		for (uint32 i = 0; i < n; i++)
			pre[i] = sift_col_str_prefix(bytes.data + off[i], off[i + 1] - off[i]);
		initStringInfo(&pb);
		sift_cc_encode(&pb, SIFT_CC_INT, pre, n, v->nulls, pol ? pol : &dp, NULL);
		plen = (uint32) pb.len;
		put_val(out, plen);
		put_zero(out, sizeof(uint32));
		put_bytes(out, pb.data, pb.len);
		pfree(pb.data);
		pfree(pre);
	}
	sift_cc_encode_strs(out, off, bytes.data, n, pol ? pol : &dp, scan);
	put_pad(out, 8);
	pfree(off);
	pfree(bytes.data);
}

bool
sift_cbatch_str_inline(const SiftColBatch *b, int col, SiftCDict *seg, Size maxbytes)
{
	const SiftCDict *bd = b->dict[col];
	Size		add = 0;

	if (b->col[col].n == 0)			/* no values (a group without elements): its chunk is empty */
		return false;
	for (uint32 c = 0; c < bd->n; c++)
		if (cdict_len(bd, c) > SIFT_COL_DICT_MAX_ENTRY)
			return true;
	if (seg->nbytes + bd->nbytes <= maxbytes)
		return false;
	for (uint32 c = 0; c < bd->n; c++)
		if (sift_cdict_find(seg, bd->bytes + bd->off[c], cdict_len(bd, c)) < 0)
			add += cdict_len(bd, c);
	return seg->nbytes + add > maxbytes;
}

/* ---------------------------------------------------------------------------
 * 8. Units
 * ------------------------------------------------------------------------- */

/* The row directory of the sorted keys of superblock sb, appended to out */
static void
unit_put_directory(StringInfo out, const SiftKey *keys, uint32 nrows, uint32 sb)
{
	uint32		head[4] = {COLU_MAGIC, nrows, 0, 0};
	uint64		pagemask[SIFT_PAGEMASK_WORDS] = {0};
	uint32	   *rowstart = palloc((SIFT_SB_PAGES + 1) * sizeof(uint32));
	uint16	   *maxoff = palloc(SIFT_SB_PAGES * sizeof(uint16));
	StringInfoData sparse;
	uint32		np = 0;

	initStringInfo(&sparse);
	for (uint32 r = 0, j; r < nrows; r = j)
	{
		uint32		page = sift_key_block(keys[r]) - sb * SIFT_SB_PAGES;
		uint32		mo;

		for (j = r; j < nrows && sift_key_block(keys[j]) == sift_key_block(keys[r]); j++)
			;
		mo = (uint32) (keys[j - 1] & 0xFFFF);
		pagemask[page / 64] |= UINT64CONST(1) << (page % 64);
		rowstart[np] = r;
		maxoff[np++] = (uint16) mo;
		if (mo != j - r)
		{
			Size		at = sparse.len;

			put_zero(&sparse, (mo + 7) / 8);
			for (uint32 k = r; k < j; k++)
				sparse.data[at + ((keys[k] & 0xFFFF) - 1) / 8] |= 1 << (((keys[k] & 0xFFFF) - 1) % 8);
		}
	}
	rowstart[np] = nrows;
	head[2] = np;
	put_val(out, head);
	put_bytes(out, pagemask, sizeof(pagemask));
	put_bytes(out, rowstart, (np + 1) * sizeof(uint32));
	put_bytes(out, maxoff, np * sizeof(uint16));
	put_bytes(out, sparse.data, sparse.len);
	put_pad(out, 4);
	pfree(rowstart);
	pfree(maxoff);
	pfree(sparse.data);
}

/* Appends the n values of v, bits each, LSB first, padded to a byte */
void
put_packed(StringInfo out, const uint32 *v, uint32 n, int bits)
{
	uint64		acc = 0;
	int			nb = 0;

	for (uint32 i = 0; i < n && bits > 0; i++)
	{
		acc |= (uint64) v[i] << nb;
		for (nb += bits; nb >= 8; nb -= 8, acc >>= 8)
			appendStringInfoChar(out, (char) (acc & 0xFF));
	}
	if (nb > 0)
		appendStringInfoChar(out, (char) (acc & 0xFF));
}

static int
bits_for(uint32 v)
{
	return v == 0 ? 0 : 32 - __builtin_clz(v);
}

/*
 * The row directory of a key-cut unit: the TIDs of its rows (ascending) as pages and offsets, not as a window of one superblock. The distances
 * between the blocks, the rows of a page and the offsets are bit-packed to the width of their largest. With body the arrays go to body, which the
 * caller writes after the chunk table, so that the table is in the first bytes of the unit: a scan that needs no TID reads no more of the directory.
 */
static void
unit_put_directory_keyed(StringInfo out, const SiftKey *keys, uint32 nrows, StringInfo body)
{
	uint32	   *delta = palloc(Max(nrows, 1) * sizeof(uint32));
	uint32	   *cnt = palloc(Max(nrows, 1) * sizeof(uint32));
	uint32	   *off = palloc(Max(nrows, 1) * sizeof(uint32));
	uint32		np = 0,
				blk0 = 0,
				prev = 0,
				w[3] = {0, 0, 0},
				head[6];

	for (uint32 r = 0, j; r < nrows; r = j)
	{
		uint32		blk = sift_key_block(keys[r]);

		for (j = r; j < nrows && sift_key_block(keys[j]) == blk; j++)
			off[j] = (uint32) (keys[j] & 0xFFFF) - 1;
		if (np == 0)
			blk0 = blk;
		else
			delta[np - 1] = blk - prev - 1;
		cnt[np++] = j - r - 1;
		prev = blk;
	}
	for (uint32 i = 0; i + 1 < np; i++)
		w[0] = Max(w[0], bits_for(delta[i]));
	for (uint32 i = 0; i < np; i++)
		w[1] = Max(w[1], bits_for(cnt[i]));
	for (uint32 i = 0; i < nrows; i++)
		w[2] = Max(w[2], bits_for(off[i]));
	head[0] = COLK_MAGIC;
	head[1] = nrows;
	head[2] = np;
	head[3] = w[0] | (w[1] << 8) | (w[2] << 16) | (body != NULL ? COLK_TABFIRST : 0);
	head[4] = blk0;
	head[5] = prev;			/* the last block: a scan that finds no page of [blk0, prev] unclean needs no more of the directory */
	put_bytes(out, head, sizeof(head));
	if (body == NULL)
		body = out;
	put_packed(body, delta, np > 0 ? np - 1 : 0, w[0]);
	put_packed(body, cnt, np, w[1]);
	put_packed(body, off, nrows, w[2]);
	put_pad(body, 4);
	pfree(delta);
	pfree(cnt);
	pfree(off);
}

Size
sift_cunit_directory_bytes(const SiftKey *keys, uint32 nrows, bool keyed, uint32 sb, uint32 *pages)
{
	StringInfoData out;
	Size		len;

	initStringInfo(&out);
	if (keyed)
		unit_put_directory_keyed(&out, keys, nrows, NULL);
	else
		unit_put_directory(&out, keys, nrows, sb);
	len = out.len;
	*pages = ((const uint32 *) out.data)[2];	/* the third word of both heads */
	pfree(out.data);
	return len;
}

void
sift_cunit_encode(const SiftColBatch *b, uint32 *const *dictmap, const bool *inl, const SiftCcPolicy *pol, bool tags, bool tabfirst, StringInfo out,
				  Size *start, SiftCZone *zones)
{
	const SiftColSchema *s = b->schema;
	StringInfoData body;
	Size		tab;

	put_pad(out, 8);
	*start = out->len;
	if (b->keyed && tabfirst)
		initStringInfo(&body);
	if (b->keyed)
		unit_put_directory_keyed(out, b->keys, b->nrows, tabfirst ? &body : NULL);
	else
		unit_put_directory(out, b->keys, b->nrows, b->sb);
	tab = out->len;
	put_zero(out, 2 * s->ncols * sizeof(uint32));
	if (b->keyed && tabfirst)
	{
		appendBinaryStringInfo(out, body.data, body.len);
		pfree(body.data);
	}
	for (int col = 0; col < s->ncols; col++)
	{
		const SiftCVals *v = &b->col[col];
		const int64 *key = v->key;
		int64	   *mapped = NULL;
		SiftCZone	zone;
		uint32		off,
					len;

		bool		inline_str = inl && inl[col] && v->n > 0;	/* a leaf without values (a group without elements) has the empty chunk */

		if (dictmap != NULL && dictmap[col] != NULL && !inline_str)
		{
			mapped = palloc(Max(v->n, 1) * sizeof(int64));
			for (uint32 i = 0; i < v->n; i++)
				mapped[i] = sift_col_bit_test(v->nulls, i) ? 0 : dictmap[col][v->key[i]];
			key = mapped;
		}
		put_pad(out, 8);
		off = (uint32) (out->len - *start);
		if (inline_str)
		{
			SiftCcScan	scan;
			SiftCcScanSum chosen = {0},
						kept = {0};

			chunk_encode_istr(out, v, b->dict[col], pol ? &pol[col] : NULL, tags, &zone, &scan);
			if (scan.seen && sift_cenc_scan_hook)
			{
				sift_cc_scan_add(scan.lz4 ? &chosen : &kept, &scan);
				sift_cenc_scan_hook(col, SIFT_SCAN_INLINE, &chosen, &kept);
			}
		}
		else
			chunk_encode(out, s->cols[col].store, v, key, pol ? &pol[col] : NULL, tags, &zone);
		len = (uint32) (out->len - *start) - off;
		memcpy(out->data + tab + col * sizeof(uint32), &off, sizeof(off));
		memcpy(out->data + tab + (s->ncols + col) * sizeof(uint32), &len, sizeof(len));
		if (zones)
			zones[col] = zone;
		if (mapped)
			pfree(mapped);
	}
	/* the kernels read SIFT_COL_PAD bytes past the last chunk: a record parsed in place has them */
	enlargeStringInfo(out, SIFT_COL_PAD);
	memset(out->data + out->len, 0, SIFT_COL_PAD);
}
