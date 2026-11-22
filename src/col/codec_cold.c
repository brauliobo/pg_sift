/*
 * col/codec_cold.c - the nodes DELTA_BYTE and BSS of the chunk codec, and the choice of a leaf under a general-purpose layer.
 *
 * DELTA_BYTE: head (tag, width B of 1..4), first value, smallest step, a checkpoint (the value) at every 1024th row, then n bytes-wide residues
 * (step - smallest step, 0 for row 0), little-endian, padded to 8. Decoded with the prefix kernel of the vertical delta node.
 * BSS: head (tag, width B of 2..8), base, then B planes of n bytes: plane b holds byte b of (value - base).
 * Alone they are no smaller than the frame of reference; they exist so that the zstd of the cold tier sees byte-aligned, repetitive streams.
 */
#include "core/host.h"

#include "col/codec_int.h"

#define CC_COLD_MIN_ROWS	256
#define CC_COLD_GAIN_PCT	8
#define CC_COLD_GAIN_L2_PCT	3
#define CC_DBYTE_BLOCK		1024

/* ---- reader ---- */

static size_t
dbyte_len(uint32 n, uint32 B)
{
	return CC_NHDR + 8 + 8 * (size_t) ((n - 1) / CC_DBYTE_BLOCK) + CC_RUP8((size_t) n * B);
}

size_t
cc_parse_delta_byte(const uint8 *p, size_t avail, uint32 n, CcNode *nd)
{
	size_t		len;

	if (n < 2)
		CC_CORRUPT("delta node with fewer than two values");
	nd->B = p[1];
	if (nd->B < 1 || nd->B > 4)
		CC_CORRUPT("byte width of a delta node out of range");
	need(avail, CC_NHDR + 8);
	nd->base = get64(p + 8);
	nd->mind = get64(p + 16);
	len = dbyte_len(n, nd->B);
	need(avail, len);
	nd->chk = p + CC_NHDR + 8;
	nd->words = nd->chk + 8 * (size_t) ((n - 1) / CC_DBYTE_BLOCK);
	return len;
}

size_t
cc_parse_bss(const uint8 *p, size_t avail, uint32 n, CcNode *nd)
{
	size_t		len;

	nd->B = p[1];
	if (nd->B < 2 || nd->B > 8)
		CC_CORRUPT("plane count out of range");
	nd->base = get64(p + 8);
	len = CC_NHDR + CC_RUP8((size_t) n * nd->B);
	need(avail, len);
	nd->words = p + CC_NHDR;
	return len;
}

/* The residues of rows [from, from + len) as u32 */
static void
dbyte_residues(const CcNode *nd, uint32 from, uint32 len, uint32 *tmp)
{
	CcNode		s = {0};

	s.tag = SIFT_CCN_FORBYTE;
	s.B = nd->B;
	s.n = len;
	s.words = nd->words + (size_t) from * nd->B;
	forbyte_u32(&s, 0, tmp);
}

void
cc_delta_byte_decode(const CcNode *nd, int64 *dst)
{
	uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(nd->n));

	dbyte_residues(nd, 0, nd->n, tmp);
	sift_cck->prefix64(tmp, nd->n, dst, nd->base, nd->mind);
	pfree(tmp);
}

/* The values of rows [b * 1024, b * 1024 + upto) from the checkpoint of their block */
void
cc_delta_byte_block(const CcNode *nd, uint32 b, uint32 upto, int64 *run)
{
	uint32		tmp[CC_DBYTE_BLOCK + SIFT_COL_PAD / 4];

	dbyte_residues(nd, b * CC_DBYTE_BLOCK, upto, tmp);
	sift_cck->prefix64(tmp, upto, run, b ? get64(nd->chk + 8 * (size_t) (b - 1)) : nd->base, nd->mind);
}

int64
cc_delta_byte_get(const CcNode *nd, uint32 i)
{
	int64		run[CC_DBYTE_BLOCK + SIFT_COL_PAD / 8];

	cc_delta_byte_block(nd, i / CC_DBYTE_BLOCK, i % CC_DBYTE_BLOCK + 1, run);
	return run[i % CC_DBYTE_BLOCK];
}

void
cc_bss_decode(const CcNode *nd, int64 *dst)
{
	uint32		n = nd->n;

	for (uint32 i = 0; i < n; i++)
		dst[i] = 0;
	for (uint32 b = 0; b < nd->B; b++)
	{
		const uint8 *plane = nd->words + (size_t) b * n;

		for (uint32 i = 0; i < n; i++)
			dst[i] |= (int64) ((uint64) plane[i] << (8 * b));
	}
	for (uint32 i = 0; i < n; i++)
		dst[i] = (int64) ((uint64) nd->base + (uint64) dst[i]);
}

int64
cc_bss_get(const CcNode *nd, uint32 i)
{
	uint64		x = 0;

	for (uint32 b = 0; b < nd->B; b++)
		x |= (uint64) nd->words[(size_t) b * nd->n + i] << (8 * b);
	return (int64) ((uint64) nd->base + x);
}

/* ---- writer ---- */

bool
cc_cold_active(const CcW *w, CcRole role)
{
	return w->pol->mode == SIFT_CCM_LAYER || role == CCR_COLD;
}

static bool
new_nodes(const CcW *w, uint32 n)
{
	const SiftCcPolicy *p = w->pol;

	return p->mode == SIFT_CCM_LAYER && p->alg == SIFT_CC_ALG_ZSTD && !p->explicit_encs && n >= CC_COLD_MIN_ROWS;
}

static uint32
width_of(uint64 range)
{
	uint32		b = (sift_cc_bits_of(range) + 7) / 8;

	return b ? b : 1;
}

static void
dbyte_node(StringInfo cand, const int64 *v, uint32 n, int64 dmin, uint32 B)
{
	uint8	   *res = palloc0((size_t) n * B + 8);

	initStringInfo(cand);
	cc_put_head(cand, SIFT_CCN_DELTA_BYTE, (uint8) B, 0);
	cc_put64(cand, v[0]);
	cc_put64(cand, dmin);
	for (uint32 i = CC_DBYTE_BLOCK; i < n; i += CC_DBYTE_BLOCK)
		cc_put64(cand, v[i]);
	for (uint32 i = 1; i < n; i++)
	{
		uint64		x = (uint64) v[i] - (uint64) v[i - 1] - (uint64) dmin;

		memcpy(res + (size_t) i * B, &x, B);
	}
	appendBinaryStringInfo(cand, (const char *) res, (int) ((size_t) n * B));
	pad8(cand);
	pfree(res);
}

static void
bss_node(StringInfo cand, const int64 *v, uint32 n, int64 base, uint32 B)
{
	uint8	   *planes = palloc((size_t) n * B + 8);

	for (uint32 i = 0; i < n; i++)
	{
		uint64		x = (uint64) v[i] - (uint64) base;

		for (uint32 b = 0; b < B; b++)
			planes[(size_t) b * n + i] = (uint8) (x >> (8 * b));
	}
	initStringInfo(cand);
	cc_put_head(cand, SIFT_CCN_BSS, (uint8) B, 0);
	cc_put64(cand, base);
	appendBinaryStringInfo(cand, (const char *) planes, (int) ((size_t) n * B));
	pad8(cand);
	pfree(planes);
}

typedef struct ColdPick
{
	const CcW  *w;
	CcRole		role;
	StringInfoData node;
	CcZTry		z;				/* the wrapper of node when z.buf */
	Size		size;
	int			tag;
} ColdPick;

/* Takes cand when its size, after the best wrapper, saves gain_pct of the best so far (any bytes when 0); frees it otherwise */
static void
pick_offer(ColdPick *pk, StringInfo cand, int tag, int gain_pct, bool need_wrapped)
{
	CcZTry		z;
	Size		sz = cand->len;
	bool		take;

	if (wrap_choose(pk->w, (const uint8 *) cand->data, cand->len, pk->role, &z))
		sz = CC_NHDR + CC_RUP8(z.len);
	else
		z.buf = NULL;
	take = (z.buf || !need_wrapped) && (gain_pct ? (uint64) sz * 100 <= (uint64) pk->size * (100 - gain_pct) : sz < pk->size);
	if (!take)
	{
		if (z.buf)
			pfree(z.buf);
		pfree(cand->data);
		return;
	}
	pfree(pk->node.data);
	if (pk->z.buf)
		pfree(pk->z.buf);
	pk->node = *cand;
	pk->z = z;
	pk->size = sz;
	pk->tag = tag;
}

static void
delta_stats(const int64 *v, uint32 n, int64 *dmin, uint64 *drange)
{
	int64		lo = 0,
				hi = 0;

	for (uint32 i = 1; i < n; i++)
	{
		int64		d = (int64) ((uint64) v[i] - (uint64) v[i - 1]);

		if (i == 1 || d < lo)
			lo = d;
		if (i == 1 || d > hi)
			hi = d;
	}
	*dmin = lo;
	*drange = (uint64) hi - (uint64) lo;
}

/*
 * The leaf node at out->data[start..] (frame of reference of W bits, plain bytes or vertical delta) is final or replaced by the byte-aligned twin, a byte-wise delta or the byte planes,
 * whichever is the smallest after the wrapper of the policy; then wrapped. Returns the tag of the node before any wrapper.
 */
int
cc_cold_leaf(const CcW *w, StringInfo out, int start, const int64 *v, uint32 n, int64 min, uint64 range, int tag, uint32 W, CcRole role)
{
	ColdPick	pk = {.w = w,.role = role};
	StringInfoData cand;
	bool		more = new_nodes(w, n);
	uint32		B = width_of(range);
	/* STOR[STOR-CODEC-06]: the deepest cold level (no fast decode) takes a byte-wise delta or byte planes for a 3% gain, the others for 8% (granule_int/num) */
	int			gain = w->pol->fast_decode ? CC_COLD_GAIN_PCT : CC_COLD_GAIN_L2_PCT;

	initStringInfo(&pk.node);
	appendBinaryStringInfo(&pk.node, out->data + start, out->len - start);
	pk.tag = tag;
	pk.size = pk.node.len;
	{
		CcZTry		z;

		if (wrap_choose(w, (const uint8 *) pk.node.data, pk.node.len, role, &z))
		{
			pk.z = z;
			pk.size = CC_NHDR + CC_RUP8(z.len);
		}
	}
	if (tag == SIFT_CCN_FORBIT && (W <= 16 || (more && range <= 0xFFFFFFFFu)))
	{
		initStringInfo(&cand);
		cc_emit_forbyte(&cand, v, n, min, B);
		pick_offer(&pk, &cand, SIFT_CCN_FORBYTE, W <= 16 ? 0 : gain, false);
	}
	if (more)
	{
		int64		dmin;
		uint64		drange;

		delta_stats(v, n, &dmin, &drange);
		if (drange <= 0xFFFFFFFFu)
		{
			dbyte_node(&cand, v, n, dmin, width_of(drange));
			pick_offer(&pk, &cand, SIFT_CCN_DELTA_BYTE, gain, true);
		}
		if (B >= 2)
		{
			bss_node(&cand, v, n, min, B);
			pick_offer(&pk, &cand, SIFT_CCN_BSS, gain, true);
		}
	}
	out->len = start;
	out->data[start] = 0;
	if (pk.z.buf)
	{
		cc_emit_wrap(out, &pk.z, pk.node.len);
		pfree(pk.z.buf);
	}
	else
		appendBinaryStringInfo(out, pk.node.data, pk.node.len);
	pfree(pk.node.data);
	return pk.tag;
}
