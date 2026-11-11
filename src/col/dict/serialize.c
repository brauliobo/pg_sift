/*
 * col/dict/serialize.c - a dictionary as its blocks: counts, hashes, signatures, compression
 * the compact form and the signatures
 * written by the segment writer
 */
#include "col/compress.h"
#include "col/dict/dict_int.h"

/*
 * A serialized dictionary starts with u32 form, u32 n. The plain form (0) goes on with u32 off[n + 1] and the bytes; the string list form
 * (1) with the codec's list (the lengths as an integer cascade, the bytes under the cold wrapper rule), which starts 8-aligned.
 */

/* The rows of the codes in byte order, by block: the layout of DICT_LOOKUP_COUNT (col/enc.h) */
static void
put_counts(const SiftCDict *d, StringInfo out)
{
	uint32		nb = DICT_COUNT_NB(d->n),
				cum = 0;
	uint32	   *rec = palloc0(((Size) nb + 1) * 2 * sizeof(uint32));
	uint8	   *cmin = palloc0(((Size) nb + 3) / 4 * 4);
	StringInfoData data;

	initStringInfo(&data);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		lo = b * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, d->n - lo),
					least = UINT32_MAX,
					most = 0,
					w = 0,
					at;
		uint8	   *bits;

		for (uint32 i = 0; i < cnt; i++)
			least = Min(least, d->rows[d->sorted[lo + i]]);
		cmin[b] = (uint8) Min(least, 255);
		for (uint32 i = 0; i < cnt; i++)
			most = Max(most, d->rows[d->sorted[lo + i]] - cmin[b]);
		while (most >> w)
			w++;
		rec[2 * b] = cum;
		rec[2 * b + 1] = data.len;
		at = data.len;
		for (uint32 i = 0; i < DICT_BLOCK * w / 8; i++)
			appendStringInfoChar(&data, '\0');
		bits = (uint8 *) data.data + at;
		for (uint32 i = 0; i < cnt; i++)
		{
			uint32		x = d->rows[d->sorted[lo + i]] - cmin[b];

			cum += d->rows[d->sorted[lo + i]];
			for (uint32 k = 0; k < w; k++)
				if (x >> k & 1)
					bits[(i * w + k) / 8] |= 1 << ((i * w + k) % 8);
		}
	}
	rec[2 * nb] = cum;
	rec[2 * nb + 1] = data.len;
	put_bytes(out, rec, ((Size) nb + 1) * 2 * sizeof(uint32));
	put_bytes(out, cmin, ((Size) nb + 3) / 4 * 4);
	put_bytes(out, data.data, data.len);
	pfree(rec);
	pfree(cmin);
	pfree(data.data);
}

static void put_tsig(const SiftCDict *d, uint32 nblk, StringInfo out, int start);

/*
 * The forms read in place: the strings in blocks of DICT_BLOCK codes, each compressed on its own when the policy allows and it is smaller (a string
 * is one read of its block), and for DICT_LOOKUP the codes in byte order of the strings (the work of a bisection is a few block reads)
 */
uint64
sift_cdict_hash(const char *s, uint32 len)
{
	return hash_bytes_extended((const unsigned char *) s, (int) len, 0);
}

/* The hash of the strings to their codes, DICT_HASHED */
static void
put_hash(const SiftCDict *d, StringInfo out)
{
	uint32		ns = SIFT_CDICT_HASH_SLOTS(d->n),
				cb = sift_cdict_hash_cb(d->n);
	uint32	   *slot = palloc0((Size) ns * sizeof(uint32));

	for (uint32 c = 0; c < d->n; c++)
	{
		uint64		h = sift_cdict_hash(d->bytes + d->off[c], cdict_len(d, c));

		for (uint32 i = sift_cdict_hash_pos(h, ns);; i = i + 1 == ns ? 0 : i + 1)
			if (slot[i] == 0)
			{
				slot[i] = sift_cdict_hash_slot(h, cb, c);
				break;
			}
	}
	put_bytes(out, slot, (Size) ns * sizeof(uint32));
	pfree(slot);
}

void
sift_cdict_put_hash(const SiftCDict *d, StringInfo out)
{
	put_hash(d, out);
}

static void
serialize_blocks(SiftCDict *d, const SiftCcPolicy *pol, uint32 form, StringInfo out)
{
	uint32		fixed = d->n > 0 ? cdict_len(d, 0) : 0,
				nbytes = (uint32) d->nbytes,
				nblk = (d->n + DICT_BLOCK - 1) / DICT_BLOCK,
				zero = 0;
	int			start = out->len,
				tab;
	uint32	   *boff = palloc((nblk + 1) * sizeof(uint32));
	uint8	   *raw = palloc(DICT_BLOCK_RAW_MAX),
			   *z = NULL,
			   *l = NULL;
	Size		zcap = sift_colz_bound(SIFT_CC_ALG_ZSTD, DICT_BLOCK_RAW_MAX),
				lcap = sift_colz_bound(SIFT_CC_ALG_LZ4, DICT_BLOCK_RAW_MAX);
	bool		zip = pol != NULL && pol->mode != SIFT_CCM_NONE && sift_colz_available(SIFT_CC_ALG_ZSTD);
	/* a dictionary the cache cannot hold is unwrapped by every test that reads it: its blocks take LZ4 where the default formula says so (codec.h) */
	SiftCcScanSum chosen = {0},
				kept = {0};
	bool		fast = zip && d->scan && pol->mode == SIFT_CCM_AUTO && pol->alg == SIFT_CC_ALG_AUTO && sift_colz_available(SIFT_CC_ALG_LZ4);

	for (uint32 c = 1; c < d->n && fixed != 0; c++)
		if (cdict_len(d, c) != fixed)
			fixed = 0;
	if (form == DICT_LOOKUP && d->sorted == NULL)
		cdict_sort(d);
	if (zip)
		z = palloc(zcap);
	if (fast)
		l = palloc(lcap);
	if (form == DICT_LOOKUP && d->rows != NULL)
		form = DICT_LOOKUP_COUNT;
	if (d->hashed && (form == DICT_LOOKUP || form == DICT_LOOKUP_COUNT || form == DICT_BLOCKS))
		form |= DICT_HASHED;
	if (d->packperm && ((form & ~DICT_FLAGS) == DICT_LOOKUP || (form & ~DICT_FLAGS) == DICT_LOOKUP_COUNT))
		form |= DICT_PERM_PACKED;
	put_val(out, form);
	put_val(out, d->n);
	put_val(out, fixed);
	put_val(out, nbytes);
	tab = out->len;
	for (uint32 b = 0; b <= nblk; b++)
		put_val(out, zero);
	boff[0] = 0;
	for (uint32 b = 0; b < nblk; b++)
	{
		uint32		lo = b * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, d->n - lo),
					rawlen = 0,
					clen = 0;
		uint8		hdr[DICT_BLOCK_HEAD] = {0};

		if (fixed == 0)
			for (uint32 i = 0; i < cnt; i++)
				raw[rawlen++] = (uint8) cdict_len(d, lo + i);
		memcpy(raw + rawlen, d->bytes + d->off[lo], d->off[lo + cnt] - d->off[lo]);
		rawlen += d->off[lo + cnt] - d->off[lo];
		if (zip)
			clen = (uint32) sift_colz_compress(SIFT_CC_ALG_ZSTD, 1, raw, rawlen, z, zcap);
		hdr[2] = (uint8) rawlen;
		hdr[3] = (uint8) (rawlen >> 8);
		if (fast && clen > 0 && clen < rawlen)
		{
			SiftCcScan	scan = {.seen = true,.raw = rawlen,.z = clen};
			Size		llen = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, raw, rawlen, l, lcap);

			scan.l = llen;
			if (sift_cc_scan_lz4(clen, llen, rawlen))
			{
				sift_cc_scan_add(&chosen, &scan);
				hdr[0] = SIFT_CC_ALG_LZ4;
				put_bytes(out, hdr, sizeof(hdr));
				put_bytes(out, l, llen);
				d->lz4_blocks = true;
				boff[b + 1] = out->len - tab - (nblk + 1) * sizeof(uint32);
				continue;
			}
			sift_cc_scan_add(&kept, &scan);
		}
		if (clen > 0 && clen < rawlen)
		{
			hdr[0] = SIFT_CC_ALG_ZSTD;
			put_bytes(out, hdr, sizeof(hdr));
			put_bytes(out, z, clen);
		}
		else
		{
			put_bytes(out, hdr, sizeof(hdr));
			put_bytes(out, raw, rawlen);
		}
		boff[b + 1] = out->len - tab - (nblk + 1) * sizeof(uint32);
	}
	memcpy(out->data + tab, boff, (nblk + 1) * sizeof(uint32));
	if ((form & ~DICT_FLAGS) == DICT_LOOKUP || (form & ~DICT_FLAGS) == DICT_LOOKUP_COUNT)
	{
		while ((out->len - start) % sizeof(uint32))
			appendStringInfoChar(out, '\0');
		if (form & DICT_PERM_PACKED)
		{
			put_packed(out, d->sorted, d->n, sift_cdict_perm_bits(d->n));
			while ((out->len - start) % sizeof(uint32))
				appendStringInfoChar(out, '\0');
		}
		else
			put_bytes(out, d->sorted, (Size) d->n * sizeof(uint32));
		if ((form & ~DICT_FLAGS) == DICT_LOOKUP_COUNT)
			put_counts(d, out);
		if (form & DICT_HASHED)
			put_hash(d, out);
	}
	else if (form == (DICT_BLOCKS | DICT_HASHED))
	{
		while ((out->len - start) % sizeof(uint32))
			appendStringInfoChar(out, '\0');
		put_hash(d, out);
	}
	if (fast && d->leaf1 > 0 && sift_cenc_scan_hook)
		sift_cenc_scan_hook(d->leaf1 - 1, SIFT_SCAN_DICT, &chosen, &kept);
	if (d->tsig)
		put_tsig(d, nblk, out, start);
	pfree(boff);
	pfree(raw);
	if (z)
		pfree(z);
}

/* The bits of a block's signature, from the distinct trigrams of a sample of blocks: about as many bits as trigrams (the signature fills to 60%) */
static uint32
tsig_lg(const SiftCDict *d, uint32 nblk)
{
	uint32		ns = Min(nblk, 16),
				lg = DICT_TSIG_LO_LG;
	uint64		distinct = 0;
	uint8	   *seen = palloc0((Size) 1 << 21);

	for (uint32 k = 0; k < ns; k++)
	{
		uint32		lo = (uint32) ((uint64) k * nblk / ns) * DICT_BLOCK,
					hi = Min(lo + DICT_BLOCK, d->n);

		for (int pass = 0; pass < 2; pass++)
			for (uint32 c = lo; c < hi; c++)
				for (uint32 p = d->off[c]; p + 3 <= d->off[c + 1]; p++)
				{
					uint32		t = (uint8) d->bytes[p] | (uint8) d->bytes[p + 1] << 8 | (uint8) d->bytes[p + 2] << 16;

					if (pass == 0 && !(seen[t >> 3] >> (t & 7) & 1))
						distinct++;
					if (pass == 0)
						seen[t >> 3] |= 1 << (t & 7);
					else
						seen[t >> 3] = 0;
				}
	}
	pfree(seen);
	distinct /= ns;
	while (lg < DICT_TSIG_HI_LG && ((uint64) 1 << lg) < distinct)
		lg++;
	return lg;
}

void
sift_cdict_tsig_fill(const SiftCDict *d, uint32 lg, uint8 *sig)
{
	uint32		bytes = (1u << lg) / 8;

	memset(sig, 0, (Size) ((d->n + DICT_BLOCK - 1) / DICT_BLOCK) * bytes);
	for (uint32 c = 0; c < d->n; c++)
	{
		uint8	   *s = sig + (Size) (c / DICT_BLOCK) * bytes;

		for (uint32 p = d->off[c]; p + 3 <= d->off[c + 1]; p++)
		{
			uint32		bit = sift_tsig_bit((const uint8 *) d->bytes + p, lg);

			s[bit >> 3] |= 1 << (bit & 7);
		}
	}
}

static void
put_tsig(const SiftCDict *d, uint32 nblk, StringInfo out, int start)
{
	uint32		lg = tsig_lg(d, nblk),
				bytes = (1u << lg) / 8,
				magic;
	uint8	   *sig = palloc((Size) nblk * bytes);

	sift_cdict_tsig_fill(d, lg, sig);
	while ((out->len - start) % 8)
		appendStringInfoChar(out, '\0');
	magic = DICT_TSIG_MAGIC;
	put_val(out, magic);
	put_val(out, lg);
	put_bytes(out, sig, (Size) nblk * bytes);
	pfree(sig);
}

#define BLOCKS_SAMPLE	16				/* blocks spread over the dictionary that sift_cdict_blocks_extra compresses */

#define COMPACT_SAMPLE	(1 << 20)		/* bytes from the start of the dictionary that it encodes in the compact form */

Size
sift_cdict_blocks_extra(SiftCDict *d, const SiftCcPolicy *pol, Size *compact_bytes)
{
	uint32		nblk = (d->n + DICT_BLOCK - 1) / DICT_BLOCK,
				ns = Min(nblk, BLOCKS_SAMPLE),
				fixed = d->n > 0 ? cdict_len(d, 0) : 0,
				pn = 0;
	uint8	   *raw = palloc(DICT_BLOCK_RAW_MAX),
			   *z = NULL;
	Size		zcap = sift_colz_bound(SIFT_CC_ALG_ZSTD, DICT_BLOCK_RAW_MAX),
				blocks = 0,
				plain,
				compact;
	bool		zip = pol != NULL && pol->mode != SIFT_CCM_NONE && sift_colz_available(SIFT_CC_ALG_ZSTD);

	for (uint32 c = 1; c < d->n && fixed != 0; c++)
		if (cdict_len(d, c) != fixed)
			fixed = 0;
	if (zip)
		z = palloc(zcap);
	for (uint32 k = 0; k < ns; k++)
	{
		uint32		lo = (uint32) ((uint64) k * nblk / ns) * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, d->n - lo),
					rawlen = 0,
					clen = 0;

		if (fixed == 0)
			for (uint32 i = 0; i < cnt; i++)
				raw[rawlen++] = (uint8) cdict_len(d, lo + i);
		memcpy(raw + rawlen, d->bytes + d->off[lo], d->off[lo + cnt] - d->off[lo]);
		rawlen += d->off[lo + cnt] - d->off[lo];
		if (zip)
			clen = (uint32) sift_colz_compress(SIFT_CC_ALG_ZSTD, 1, raw, rawlen, z, zcap);
		blocks += sizeof(uint32) + DICT_BLOCK_HEAD + (clen > 0 && clen < rawlen ? clen : rawlen);
	}
	/* a block is the same size wherever it lies; the compact form is not (it compresses better with its length), so it is encoded from the start for as many bytes as it is cheap to */
	while (pn < d->n && d->off[pn] < COMPACT_SAMPLE)
		pn++;
	plain = sizeof(uint32) * (pn + 1) + d->off[pn];
	compact = plain;
	if (zip && plain >= SIFT_CC_MIN_WRAP)
	{
		StringInfoData e;

		initStringInfo(&e);
		sift_cc_encode_strs(&e, d->off, d->bytes, pn, pol, NULL);
		compact = Min(compact, (Size) e.len);
		pfree(e.data);
	}
	compact = (Size) ((double) compact * d->nbytes / Max(d->off[pn], 1));
	blocks = (Size) ((double) blocks * nblk / ns);
	*compact_bytes = compact;
	pfree(raw);
	if (z)
		pfree(z);
	return blocks > compact ? blocks - compact : 0;
}

void
sift_cdict_serialize(SiftCDict *d, const SiftCcPolicy *pol, uint32 blocks, StringInfo out)
{
	Size		plain = sizeof(uint32) * (d->n + 1) + d->nbytes;
	uint32		form = DICT_PLAIN;
	StringInfoData z;

	if (blocks != 0 && d->n > 0)
	{
		serialize_blocks(d, pol, blocks, out);
		return;
	}
	z.data = NULL;
	if (pol != NULL && pol->mode != SIFT_CCM_NONE && d->n <= SIFT_CC_MAXROWS && plain >= SIFT_CC_MIN_WRAP)
	{
		SiftCcScan	scan;

		initStringInfo(&z);
		sift_cc_encode_strs(&z, d->off, d->bytes, d->n, pol, d->scan ? &scan : NULL);
		if (d->scan && scan.seen && d->leaf1 > 0 && sift_cenc_scan_hook)
		{
			SiftCcScanSum chosen = {0},
						kept = {0};

			sift_cc_scan_add(scan.lz4 ? &chosen : &kept, &scan);
			sift_cenc_scan_hook(d->leaf1 - 1, SIFT_SCAN_DICT, &chosen, &kept);
		}
		if ((Size) z.len < plain)
			form = DICT_STRS;
	}
	put_val(out, form);
	put_val(out, d->n);
	if (form == DICT_STRS)
		put_bytes(out, z.data, z.len);
	else
	{
		put_bytes(out, d->off, (d->n + 1) * sizeof(uint32));
		put_bytes(out, d->bytes, d->nbytes);
	}
	if (z.data)
		pfree(z.data);
}
