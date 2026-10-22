/*
 * col/codec_blob.c - the string list and bit vector blobs of the chunk codec.
 */
#include "core/host.h"

#include "col/codec_int.h"
#include "col/compress.h"
#include "core/popcount.h"

#define CC_STRS_MAGIC	0xC6
#define CC_BITS_MAGIC	0xC7

enum
{
	CCB_ZEROS,
	CCB_ONES,
	CCB_RAW,
	CCB_SET,					/* positions of the set bits */
	CCB_CLEAR					/* positions of the clear bits */
};

/*
 * String list: 8-byte header (magic, version, u32 n, u32 length of the lengths node), the lengths node (an integer chunk node, cold), then
 * the bytes: u32 form (0 raw, else the algorithm id), u32 rawlen, u32 clen, data padded to 8.
 */
void
sift_cc_encode_strs(StringInfo out, const uint32 *off, const char *bytes, uint32 n, const SiftCcPolicy *pol)
{
	CcW			w = {pol, SIFT_CC_INT};
	int64	   *lens = palloc(sizeof(int64) * (n + 1));
	StringInfoData ln;
	uint32		total = n ? off[n] - off[0] : 0;
	CcZTry		z;
	const uint8 *src = (const uint8 *) bytes + (n ? off[0] : 0);

	check_policy(pol);
	for (uint32 i = 0; i < n; i++)
		lens[i] = off[i + 1] - off[i];
	initStringInfo(&ln);
	enc_node(&w, &ln, lens, n, SIFT_CC_MAXDEPTH - 1, CCR_COLD);
	put32(out, (CC_STRS_MAGIC | (SIFT_CC_VERSION << 8)));
	put32(out, n);
	put32(out, (uint32) ln.len);
	put32(out, total);
	appendBinaryStringInfo(out, ln.data, ln.len);
	if (wrap_choose(&w, src, total, CCR_COLD, &z))
	{
		put32(out, (uint32) z.alg);
		put32(out, (uint32) z.len);
		appendBinaryStringInfo(out, z.buf, (int) z.len);
		pfree(z.buf);
	}
	else
	{
		put32(out, 0);
		put32(out, total);
		appendBinaryStringInfo(out, (const char *) src, (int) total);
	}
	pad8(out);
	pfree(lens);
	pfree(ln.data);
}

void
sift_cc_decode_strs(MemoryContext mcxt, const uint8 *blob, Size len, uint32 want, uint32 **off, char **bytes)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift string list", ALLOCSET_SMALL_SIZES);
	uint32		n,
				ll,
				total,
				form,
				clen;
	CcNode	   *nd;
	uint32	   *lens,
			   *o;
	char	   *b;
	size_t		u,
				pos;
	uint64		sum = 0;

	if (len < 24 || (len & 7) || ((uintptr_t) blob & 7) || get32(blob) != (CC_STRS_MAGIC | (SIFT_CC_VERSION << 8)))
		CC_CORRUPT("bad string list header");
	n = get32(blob + 4);
	ll = get32(blob + 8);
	total = get32(blob + 12);
	if (n != want || n > SIFT_CC_MAXROWS || (ll & 7) || (uint64) ll + 16 + 8 > len || total > (uint64) 1 << 31)
		CC_CORRUPT("bad string list sizes");
	nd = parse_node(cx, SIFT_CC_INT, blob + 16, ll, n, 0, &u);
	if (u != ll)
		CC_CORRUPT("string lengths node does not match");
	pos = 16 + (size_t) ll;
	form = get32(blob + pos);
	clen = get32(blob + pos + 4);
	pos += 8;
	if ((uint64) pos + CC_RUP8(clen) != len)
		CC_CORRUPT("string list length does not match");
	if (form == 0 && clen != total)
		CC_CORRUPT("raw string bytes length does not match");
	lens = palloc(sizeof(uint32) * CC_CAP32(n));
	nd_u32(nd, 0, lens);
	o = MemoryContextAlloc(mcxt, sizeof(uint32) * (n + 1));
	b = MemoryContextAlloc(mcxt, (size_t) total + 1);
	o[0] = 0;
	for (uint32 i = 0; i < n; i++)
	{
		sum += lens[i];
		if (sum > total)
			CC_CORRUPT("string lengths exceed the bytes");
		o[i + 1] = (uint32) sum;
	}
	if (sum != total)
		CC_CORRUPT("string lengths do not add up");
	if (form == 0)
		memcpy(b, blob + pos, total);
	else
	{
		if (!sift_colz_available((int) form))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("string list uses compression algorithm %u, which this build does not provide", form)));
		if (!sift_colz_decompress((int) form, blob + pos, clen, b, total))
			CC_CORRUPT("compressed string bytes are damaged");
	}
	b[total] = 0;
	*off = o;
	*bytes = b;
	pfree(lens);
	MemoryContextDelete(cx);
}

/*
 * Bit vector: 8-byte header (magic, form, u16 pad, u32 nbits); raw: the bytes padded to 8; positions: u32 count, u32 node length, the node.
 */
void
sift_cc_encode_bits(StringInfo out, const uint64 *words, uint32 nbits, const SiftCcPolicy *pol)
{
	uint64		set = 0;
	uint32		nw = (nbits + 63) / 64;
	size_t		rawsz = CC_RUP8((nbits + 7) / 8);
	int			form = CCB_RAW;
	int64	   *pos = NULL;
	uint32		npos = 0;
	StringInfoData node;
	CcW			w = {pol, SIFT_CC_INT};
	char		h[8] = {0};

	check_policy(pol);
	for (uint32 i = 0; i < nw; i++)
		set += sift_pc64(i == nw - 1 && nbits % 64 ? words[i] & ((UINT64CONST(1) << (nbits % 64)) - 1) : words[i]);
	node.data = NULL;
	if (set == 0)
		form = CCB_ZEROS;
	else if (set == nbits)
		form = CCB_ONES;
	else if (set * 16 <= nbits || (nbits - set) * 16 <= nbits)
	{
		bool		want = set * 2 <= nbits;

		pos = palloc(sizeof(int64) * (want ? set : nbits - set));
		for (uint32 i = 0; i < nbits; i++)
			if ((((words[i / 64] >> (i % 64)) & 1) != 0) == want)
				pos[npos++] = i;
		initStringInfo(&node);
		enc_node(&w, &node, pos, npos, SIFT_CC_MAXDEPTH - 1, CCR_COLD);
		if (8 + node.len < rawsz)
			form = want ? CCB_SET : CCB_CLEAR;
	}
	h[0] = (char) CC_BITS_MAGIC;
	h[1] = (char) form;
	memcpy(h + 4, &nbits, 4);
	appendBinaryStringInfo(out, h, 8);
	if (form == CCB_RAW)
	{
		uint32		nb = (nbits + 7) / 8;
		char	   *raw = palloc0(rawsz + 8);

		for (uint32 i = 0; i < nb; i++)
			raw[i] = (char) (words[i / 8] >> (8 * (i % 8)));
		if (nbits % 8)
			raw[nb - 1] &= (char) ((1 << (nbits % 8)) - 1);
		appendBinaryStringInfo(out, raw, (int) rawsz);
		pfree(raw);
	}
	else if (form == CCB_SET || form == CCB_CLEAR)
	{
		put32(out, npos);
		put32(out, (uint32) node.len);
		appendBinaryStringInfo(out, node.data, node.len);
	}
	if (pos)
		pfree(pos);
	if (node.data)
		pfree(node.data);
}

void
sift_cc_decode_bits(const uint8 *blob, Size len, uint32 nbits, uint64 *words)
{
	uint32		nw = (nbits + 63) / 64;
	int			form;

	if (len < 8 || (len & 7) || ((uintptr_t) blob & 7) || blob[0] != CC_BITS_MAGIC || get32(blob + 4) != nbits)
		CC_CORRUPT("bad bit vector header");
	form = blob[1];
	memset(words, 0, sizeof(uint64) * nw);
	switch (form)
	{
		case CCB_ZEROS:
			if (len != 8)
				CC_CORRUPT("bit vector length does not match");
			break;
		case CCB_ONES:
			if (len != 8)
				CC_CORRUPT("bit vector length does not match");
			sift_cc_set_ones(words, nbits);
			break;
		case CCB_RAW:
			if (len != 8 + CC_RUP8((nbits + 7) / 8))
				CC_CORRUPT("bit vector length does not match");
			memcpy(words, blob + 8, (nbits + 7) / 8);
			clear_tail(words, nbits);
			break;
		case CCB_SET:
		case CCB_CLEAR:
			{
				MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift bit vector", ALLOCSET_SMALL_SIZES);
				uint32		npos,
							nl;
				CcNode	   *nd;
				int64	   *pos;
				size_t		u;

				need(len, 16);
				npos = get32(blob + 8);
				nl = get32(blob + 12);
				if (npos > nbits || (nl & 7) || (uint64) nl + 16 != len)
					CC_CORRUPT("bit vector positions do not match");
				nd = parse_node(cx, SIFT_CC_INT, blob + 16, nl, npos, 0, &u);
				if (u != nl)
					CC_CORRUPT("bit vector node does not match");
				pos = palloc(sizeof(int64) * ((size_t) npos + SIFT_COL_PAD / 8));
				nd_i64(nd, pos);
				if (form == CCB_CLEAR)
					sift_cc_set_ones(words, nbits);
				for (uint32 i = 0; i < npos; i++)
				{
					if (pos[i] < 0 || pos[i] >= (int64) nbits || (i && pos[i] <= pos[i - 1]))
						CC_CORRUPT("bit vector position out of order");
					if (form == CCB_SET)
						words[pos[i] / 64] |= UINT64CONST(1) << (pos[i] % 64);
					else
						words[pos[i] / 64] &= ~(UINT64CONST(1) << (pos[i] % 64));
				}
				pfree(pos);
				MemoryContextDelete(cx);
				break;
			}
		default:
			CC_CORRUPT("unknown bit vector form");
	}
}
