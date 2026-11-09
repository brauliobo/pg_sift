/*
 * col/seg/gorder_str.c - the strings of a text global order: the collation, the string of a rank and the rank of a string
 */
#include "core/cmp.h"
#include "col/seg_int.h"

/* The collation of the strings of a text block (0: the leaf is not text, or the segment has no block) */
uint32
sift_cseg_gorder_collation(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	return g == NULL ? 0 : g->hdr.collid;
}

/* The number of strings of a text block, read on first use; the start of its strings in the segment */
uint64
seg_gord_strs(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	uint64		seg_sec = g->at + g->bytes - g->hdr.strbytes;

	if (g->sbno == 0 && g->nstr == 0)
	{
		uint8		head[16];
		uint32		nblk;

		seg_read(seg, seg_sec, sizeof(head), head);
		if (g->hdr.strperm ? !sift_cgord_perm_head(head, &g->hdr, &g->nstr, &g->pbits) : !sift_cgord_str_head(head, &g->hdr, &g->nstr, &nblk))
			corrupt(seg, "The strings of the global order of leaf %d are malformed.", col);
		g->sbno = UINT32_MAX;
	}
	return seg_sec;
}

/* The dictionary code of the string of rank r in a block of version 5 */
uint32
seg_gord_perm(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 r)
{
	uint64		bit = (uint64) r * g->pbits;
	uint8		buf[8];
	Size		nb = (Size) (((bit & 7) + g->pbits + 7) / 8);

	seg_read(seg, seg_gord_strs(seg, g, col) + 16 + bit / 8, nb, buf);
	return sift_ckeyed_get(buf, nb, bit & 7, g->pbits);
}

/* The string of rank r of a text block, valid until the next call for the leaf; *len its bytes */
const char *
sift_cseg_gorder_str(SiftColSegment *seg, int col, uint32 r, uint32 *len)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		b = r / SIFT_CGORD_STR_BLK;
	uint32		off[2];
	uint64		seg_sec;
	char	   *str;

	if (g == NULL || g->hdr.strbytes == 0)
		corrupt(seg, "The global order of leaf %d has no strings.", col);
	seg_sec = seg_gord_strs(seg, g, col);
	if (r >= g->nstr)
		corrupt(seg, "The global order of leaf %d has no string %u.", col, r);
	if (g->hdr.strperm)
	{
		uint32		code = seg_gord_perm(seg, g, col, r);

		if (code >= g->nstr)
			corrupt(seg, "The global order of leaf %d names the code %u of %u strings.", col, code, g->nstr);
		/* the strings of a few ranks are read in place; the dictionary is parsed, and kept, once the backend's reads of it have cost about what the parse does */
		if (g->sdict == NULL)
			g->sdict = sift_cseg_dict_peek(seg, col);
		if (g->sdict == NULL && sift_cseg_dict_spent(seg, col, 0) >= (uint64) g->nstr * DICT_PARSE_COST && !sift_cseg_dict_nokeep(seg, col))
			g->sdict = sift_cseg_dict(seg, col);
		if (g->sdict != NULL)
			return sift_cdict_str(g->sdict, code, len);
		if (g->sblk == NULL)
			g->sblk = MemoryContextAlloc(seg->mcxt, SIFT_CGORD_STR_MAX);
		sift_cseg_dict_spent(seg, col, DICT_ZSTD_COST + DICT_READ_COST);
		return sift_cseg_dict_get(seg, col, code, len, (char *) g->sblk);
	}
	if (g->sbno != b)
	{
		/* the offsets of the block and of the next, read where they are: a segment of 200,000 strings has 12,000 blocks */
		seg_read(seg, seg_sec + 16 + (uint64) b * sizeof(uint32), sizeof(off), off);
		if (off[0] > off[1] || off[1] > g->hdr.strbytes || off[1] - off[0] > (uint32) SIFT_CGORD_STR_BLK * (SIFT_CGORD_STR_MAX + 8))
			corrupt(seg, "The strings of the global order of leaf %d have a malformed block %u.", col, b);
		if (g->sblk == NULL)
			g->sblk = MemoryContextAlloc(seg->mcxt, (Size) SIFT_CGORD_STR_BLK * (SIFT_CGORD_STR_MAX + 8) + SIFT_CGORD_STR_MAX);
		seg_read(seg, seg_sec + off[0], off[1] - off[0], g->sblk);
		g->sbno = b;
		g->sblen = off[1] - off[0];
		g->spos = (SiftCStrPos) {0, 0, 0};
	}
	str = (char *) g->sblk + (Size) SIFT_CGORD_STR_BLK * (SIFT_CGORD_STR_MAX + 8);
	if (!sift_cgord_str_get(g->sblk, g->sblen, &g->spos, r % SIFT_CGORD_STR_BLK, str, len))
		corrupt(seg, "The strings of the global order of leaf %d have a malformed block %u.", col, b);
	return str;
}

/* the order of the string of rank r against s */
static int
gord_str_vs(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 r, const char *s, uint32 slen)
{
	uint32		len;
	const char *str = sift_cseg_gorder_str(seg, col, r, &len);

	return sift_cgord_str_order(str, len, s, slen, g->hdr.collid);
}

uint32
sift_cseg_gorder_strrank(SiftColSegment *seg, int col, const char *s, uint32 slen, bool after)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		lo = 0,
				hi;

	if (g == NULL || g->hdr.strbytes == 0)
		corrupt(seg, "The global order of leaf %d has no strings.", col);
	(void) seg_gord_strs(seg, g, col);
	hi = g->nstr;
	SIFT_BISECT(uint32, mid, lo, hi, after ? gord_str_vs(seg, g, col, mid, s, slen) <= 0 : gord_str_vs(seg, g, col, mid, s, slen) < 0);
	return lo;
}
