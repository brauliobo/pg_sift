/*
 * col/codec_parse.c - the reader's entry: validates a chunk blob and parses it into its node tree, opens and closes it.
 */
#include "sift_host.h"

#include "bm25/analyze.h"
#include "col/codec_int.h"
#include "col/compress.h"

#define CC_TRAILER			8

/* ======================================================================== */
/* reader                                                                   */
/* ======================================================================== */

static void *
rd_alloc(MemoryContext cx, size_t sz)
{
	return MemoryContextAllocZero(cx, sz);
}

void
need(size_t have, size_t want)
{
	if (have < want)
		CC_CORRUPT("node is truncated");
}

static CcNode *
parse_child(MemoryContext cx, SiftCcKind kind, const uint8 *p, size_t len, uint32 n, int depth)
{
	size_t		used;
	CcNode	   *nd;

	if (len & 7)
		CC_CORRUPT("child length is not a multiple of 8");
	nd = parse_node(cx, kind, p, len, n, depth, &used);
	if (used != len)
		CC_CORRUPT("child node length does not match");
	return nd;
}

/* Parses one node at p (8-aligned) of n values; *used is its size */
CcNode *
parse_node(MemoryContext cx, SiftCcKind kind, const uint8 *p, size_t avail, uint32 n, int depth, size_t *used)
{
	CcNode	   *nd;
	size_t		len;
	uint32		f1;

	if (depth > SIFT_CC_MAXDEPTH)
		CC_CORRUPT("nodes nested too deep");
	need(avail, CC_NHDR);
	nd = rd_alloc(cx, sizeof(CcNode));
	nd->tag = p[0];
	nd->kind = (uint8) kind;
	nd->n = n;
	f1 = get32(p + 4);
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			nd->base = get64(p + 8);
			len = CC_NHDR;
			break;
		case SIFT_CCN_FORBIT:
			nd->W = p[1];
			if (nd->W < 1 || nd->W > 32)
				CC_CORRUPT("bit width out of range");
			nd->base = get64(p + 8);
			len = CC_NHDR + CC_RUP8(4 * (size_t) sift_cc_words(n, nd->W));
			need(avail, len);
			nd->words = p + CC_NHDR;
			break;
		case SIFT_CCN_FORBYTE:
			nd->B = p[1];
			if (nd->B < 1 || nd->B > 8)
				CC_CORRUPT("byte width out of range");
			nd->base = get64(p + 8);
			len = CC_NHDR + CC_RUP8((size_t) n * nd->B);
			need(avail, len);
			nd->words = p + CC_NHDR;
			break;
		case SIFT_CCN_DELTA:
			if (n < 2)
				CC_CORRUPT("delta node with fewer than two values");
			nd->W = p[1];
			if (nd->W > 32)
				CC_CORRUPT("bit width above 32");
			need(avail, CC_NHDR + 8);
			nd->base = get64(p + 8);
			nd->mind = get64(p + 16);
			len = CC_NHDR + 8 + 8 * (size_t) ((n - 1) / 1024) + CC_RUP8(4 * (size_t) sift_cc_words(n, nd->W));
			need(avail, len);
			nd->chk = p + CC_NHDR + 8;
			nd->words = nd->chk + 8 * (size_t) ((n - 1) / 1024);
			break;
		case SIFT_CCN_RLE:
			{
				uint32		vl = get32(p + 8),
							ll = get32(p + 12);
				uint32	   *lens;
				uint64		sum = 0;

				nd->nruns = f1;
				if (nd->nruns < 1 || nd->nruns > n)
					CC_CORRUPT("run count out of range");
				len = CC_NHDR + (size_t) vl + ll;
				need(avail, len);
				nd->a = parse_child(cx, SIFT_CC_INT, p + CC_NHDR, vl, nd->nruns, depth + 1);
				nd->b = parse_child(cx, SIFT_CC_INT, p + CC_NHDR + vl, ll, nd->nruns, depth + 1);
				lens = rd_alloc(cx, sizeof(uint32) * CC_CAP32(nd->nruns));
				nd_u32(nd->b, 0, lens);
				nd->rstart = rd_alloc(cx, sizeof(uint32) * (nd->nruns + 1));
				for (uint32 r = 0; r < nd->nruns; r++)
				{
					if (lens[r] == 0)
						CC_CORRUPT("empty run");
					nd->rstart[r] = (uint32) sum;
					sum += lens[r];
					if (sum > n)
						CC_CORRUPT("runs longer than the chunk");
				}
				if (sum != n)
					CC_CORRUPT("runs shorter than the chunk");
				nd->rstart[nd->nruns] = n;
				pfree(lens);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32		dl = get32(p + 8),
							cl = get32(p + 12);

				nd->d = f1;
				if (nd->d < 1 || nd->d > n)
					CC_CORRUPT("dictionary size out of range");
				len = CC_NHDR + (size_t) dl + cl;
				need(avail, len);
				nd->a = parse_child(cx, SIFT_CC_INT, p + CC_NHDR, dl, nd->d, depth + 1);
				nd->b = parse_child(cx, SIFT_CC_INT, p + CC_NHDR + dl, cl, n, depth + 1);
				nd->dict = rd_alloc(cx, sizeof(int64) * (nd->d + 8));
				nd_i64(nd->a, nd->dict);
				for (uint32 i = 1; i < nd->d; i++)
					if (nd->dict[i] <= nd->dict[i - 1])
						CC_CORRUPT("dictionary is not sorted");
				break;
			}
		case SIFT_CCN_ALP:
			{
				uint32		dl = get32(p + 8),
							pl = get32(p + 12);
				int			emax = kind == SIFT_CC_F8 ? SIFT_CC_ALP_EMAX_F8 : SIFT_CC_ALP_EMAX_F4;

				if (kind == SIFT_CC_INT)
					CC_CORRUPT("decimal node in an integer chunk");
				need(avail, 32);
				nd->e = p[1];
				nd->nexc = f1;
				nd->dmin = get64(p + 16);
				nd->dmax = get64(p + 24);
				int64		lim = kind == SIFT_CC_F8 ? ((int64) 1 << 52) : ((int64) 1 << 24);

				if (nd->e > emax || nd->nexc > n || nd->dmax < nd->dmin || nd->dmin < -lim || nd->dmax > lim ||
					(uint64) nd->dmax - (uint64) nd->dmin > 0xFFFFFFFFu)
					CC_CORRUPT("decimal node parameters out of range");
				if ((nd->nexc == 0) != (pl == 0))
					CC_CORRUPT("exception list length does not match");
				len = 32 + (size_t) dl + pl + 8 * (size_t) nd->nexc;
				need(avail, len);
				nd->a = parse_child(cx, SIFT_CC_INT, p + 32, dl, n, depth + 1);
				if (nd->nexc)
				{
					int64	   *tmp = rd_alloc(cx, sizeof(int64) * (nd->nexc + 8));

					nd->b = parse_child(cx, SIFT_CC_INT, p + 32 + dl, pl, nd->nexc, depth + 1);
					nd_i64(nd->b, tmp);
					nd->pos = rd_alloc(cx, sizeof(uint32) * nd->nexc);
					for (uint32 i = 0; i < nd->nexc; i++)
					{
						if (tmp[i] < 0 || tmp[i] >= (int64) n || (i && tmp[i] <= tmp[i - 1]))
							CC_CORRUPT("exception positions out of order");
						nd->pos[i] = (uint32) tmp[i];
					}
					pfree(tmp);
				}
				nd->ekeys = p + 32 + dl + pl;
				break;
			}
		case SIFT_CCN_SCALE:
			need(avail, 24);
			nd->base = get64(p + 8);
			nd->mind = get64(p + 16);	/* the divisor */
			if (nd->mind < 2)
				CC_CORRUPT("scale factor below 2");
			len = 24 + (size_t) f1;
			need(avail, len);
			nd->a = parse_child(cx, SIFT_CC_INT, p + 24, f1, n, depth + 1);
			break;
		case SIFT_CCN_WRAP:
			{
				uint32		cl = get32(p + 8);
				uint8	   *buf;
				size_t		u;

				nd->alg = p[1];
				len = CC_NHDR + CC_RUP8(cl);
				need(avail, len);
				if (!sift_colz_available(nd->alg))
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("column chunk uses compression algorithm %d, which this build does not provide", nd->alg)));
				if (f1 < CC_NHDR || (f1 & 7) || f1 > (uint64) n * 9 + 4096 + 64)
					CC_CORRUPT("wrapped length out of range");
				buf = rd_alloc(cx, (size_t) f1 + SIFT_COL_PAD);
				if (!sift_colz_decompress(nd->alg, p + CC_NHDR, cl, buf, f1))
					CC_CORRUPT("compressed stream is damaged");
				nd->a = parse_node(cx, kind, buf, f1, n, depth + 1, &u);
				if (u != f1)
					CC_CORRUPT("wrapped node length does not match");
				break;
			}
		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("column chunk uses encoding %d, which this build does not know", nd->tag)));
	}
	*used = len;
	return nd;
}

SiftCc *
sift_cc_open(MemoryContext mcxt, const uint8 *blob, Size len, bool verify)
{
	MemoryContext cx;
	SiftCc	   *c;
	size_t		used;

	if (len < SIFT_CC_HDRLEN + CC_NHDR + CC_TRAILER || (len & 7) || ((uintptr_t) blob & 7) || blob[0] != SIFT_CC_MAGIC)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: bad header or misaligned blob")));
	if (blob[1] != SIFT_CC_VERSION)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("column chunk has format version %d, this build reads %d", blob[1], SIFT_CC_VERSION)));
	if (blob[2] > SIFT_CC_F8)
		CC_CORRUPT("unknown value kind");
	if (verify && sift_term_hash((const char *) blob, (int) (len - CC_TRAILER)) != get32(blob + len - CC_TRAILER))
		CC_CORRUPT("check word mismatch");
	cx = AllocSetContextCreate(mcxt, "sift column chunk", ALLOCSET_SMALL_SIZES);
	c = MemoryContextAllocZero(cx, sizeof(SiftCc));
	c->mcxt = cx;
	c->kind = (SiftCcKind) blob[2];
	c->flags = blob[3];
	c->n = get32(blob + 4);
	c->min = get64(blob + 8);
	c->max = get64(blob + 16);
	if (c->n > SIFT_CC_MAXROWS || c->max < c->min)
		CC_CORRUPT("row count or range out of bounds");
	if (((c->flags & SIFT_CCF_OFF32) != 0) != ((uint64) c->max - (uint64) c->min <= 0xFFFFFFFFu))
		CC_CORRUPT("offset flag contradicts the range");
	c->root = parse_node(cx, c->kind, blob + SIFT_CC_HDRLEN, len - SIFT_CC_HDRLEN - CC_TRAILER, c->n, 0, &used);
	if (used != len - SIFT_CC_HDRLEN - CC_TRAILER)
		CC_CORRUPT("trailing bytes after the root node");
	return c;
}

void
sift_cc_close(SiftCc *c)
{
	MemoryContextDelete(c->mcxt);
}

SiftCcKind
sift_cc_kind(const SiftCc *c)
{
	return c->kind;
}

uint32
sift_cc_nrows(const SiftCc *c)
{
	return c->n;
}

int64
sift_cc_min(const SiftCc *c)
{
	return c->min;
}

int64
sift_cc_max(const SiftCc *c)
{
	return c->max;
}

uint8
sift_cc_tag(const SiftCc *c)
{
	return unwrap(c->root)->tag;
}

uint32
sift_cc_off32_bound(const SiftCc *c)
{
	const CcNode *nd = unwrap(c->root);
	uint64		off = (uint32) ((uint64) nd->base - (uint64) c->min),
				span;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			span = 0;
			break;
		case SIFT_CCN_FORBIT:
			span = nd->W < 32 ? (UINT64CONST(1) << nd->W) - 1 : UINT32_MAX;
			break;
		case SIFT_CCN_FORBYTE:
			span = nd->B < 4 ? (UINT64CONST(1) << (8 * nd->B)) - 1 : UINT32_MAX;
			break;
		default:
			return UINT32_MAX;
	}
	return Min(off + span, UINT32_MAX);
}
