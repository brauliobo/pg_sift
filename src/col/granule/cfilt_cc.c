/*
 * col/granule/cfilt_cc.c - the kernels of col/granule/cfilt_kern.h on the nodes of a column chunk of the codec (col/granule/cfilt_cc.h).
 */
#include "postgres.h"

#include "col/codec_int.h"
#include "col/granule/cfilt_cc.h"
#include "core/par/arena.h"

_Thread_local SiftCfCcStats sift_cf_cc_stats;

static SiftCfResult cf_node(const CcNode *nd, const SiftCfTest *t, const SiftCfBounds *bnd, uint64 *out);

static bool
is_range(const SiftCfTest *t)
{
	return t->kind == SIFT_CFT_RANGE || t->kind == SIFT_CFT_NRANGE;
}

static SiftCfResult
cf_rle(const CcNode *nd, const SiftCfTest *t, uint64 *out)
{
	int64	   *vals = sift_alloc(sizeof(int64) * (nd->nruns + SIFT_COL_PAD / 8));
	uint32	   *lens = sift_alloc(sizeof(uint32) * nd->nruns);
	SiftCfResult res;

	nd_i64(nd->a, vals);
	for (uint32 r = 0; r < nd->nruns; r++)
		lens[r] = nd->rstart[r + 1] - nd->rstart[r];
	res = sift_cf_rle(&(SiftCfRle) {.nruns = nd->nruns,.n = nd->n,.vals = vals,.lens = lens}, t, out);
	sift_free(vals);
	sift_free(lens);
	return res;
}

/* The codes of a node tested by the bits of match (a bit for each code of 0 .. dict_n - 1): the codes are an array, the vertical frame of reference of the codec, runs or one code */
static SiftCfResult
cf_codes(const CcNode *nd, uint32 dict_n, const uint64 *match, uint64 *out)
{
	const CcNode *codes = unwrap(nd);
	uint32		bits = codes->tag == SIFT_CCN_FORBYTE ? 8 * codes->B : codes->tag == SIFT_CCN_FORBIT ? codes->W : 64;
	uint32		reach = bits >= 32 ? dict_n : (uint32) Min(dict_n, UINT64CONST(1) << bits);
	SiftCfTest	member = {.kind = SIFT_CFT_MEMBER,.member = match,.nmember = dict_n};

	if (dict_n == 0)
		return SIFT_CF_FALSE;
	switch (codes->tag)
	{
		case SIFT_CCN_FORBYTE:
			if (codes->base != 0)
				return sift_cf_for(&(SiftCfFor) {.base = codes->base,.bits = 8 * codes->B,.n = codes->n,.packed = codes->words}, &member, out);
			return sift_cf_dict(&(SiftCfDict) {.dict_n = reach,.bits = 8 * codes->B,.n = codes->n,.packed = codes->words,.match = match}, out);
		case SIFT_CCN_FORBIT:
			if (codes->base != 0)
				return sift_cf_for_vert(&(SiftCfForVert) {.base = codes->base,.bits = codes->W,.n = codes->n,.words = codes->words}, &member, out);
			return sift_cf_dict_vert(&(SiftCfDictVert) {.dict_n = reach,.bits = codes->W,.n = codes->n,.words = codes->words,.match = match}, out);
		case SIFT_CCN_CONST:
			return sift_cf_test_value(&member, codes->base) ? SIFT_CF_TRUE : SIFT_CF_FALSE;
		case SIFT_CCN_RLE:
			return cf_rle(codes, &member, out);
		default:
			return SIFT_CF_UNSUPPORTED;
	}
}

/* The entries of a dictionary node that pass are found by binary search, then the codes are tested for them */
static SiftCfResult
cf_dict(const CcNode *nd, const SiftCfTest *t, uint64 *out)
{
	uint64	   *match = sift_alloc0(sizeof(uint64) * ((nd->d + 63) / 64 + 1));
	SiftCfResult res;

	sift_cf_dict_match_sorted(nd->dict, nd->d, t, match);
	res = cf_codes(nd->b, nd->d, match, out);
	sift_free(match);
	return res;
}

/* base + g * w is in a range exactly when w is in a range of its own: the node below is asked that */
static SiftCfResult
cf_scale_range(const CcNode *nd, const SiftCfTest *t, uint64 *out)
{
	uint64		g = (uint64) nd->mind,
				d = t->lo <= nd->base ? 0 : (uint64) t->lo - (uint64) nd->base,
				wlo = d / g + (d % g != 0),
				whi = t->hi < nd->base ? 0 : ((uint64) t->hi - (uint64) nd->base) / g;
	bool		empty = t->lo > t->hi || t->hi < nd->base || wlo > whi || wlo > (uint64) INT64_MAX;

	if (empty)
		return t->kind == SIFT_CFT_RANGE ? SIFT_CF_FALSE : SIFT_CF_TRUE;
	return cf_node(nd->a, &(SiftCfTest) {.kind = t->kind,.lo = (int64) wlo,.hi = (int64) Min(whi, (uint64) INT64_MAX)}, &(SiftCfBounds) {0}, out);
}

/* ... and is in a set exactly when w is in the set of the keys that are base + g * w, which stay in ascending order */
static SiftCfResult
cf_scale_set(const CcNode *nd, const SiftCfTest *t, uint64 *out)
{
	uint64		g = (uint64) nd->mind;
	int64	   *codes = sift_alloc(sizeof(int64) * Max(t->nset, 1));
	uint32		n = 0;
	SiftCfResult res;

	for (uint32 i = 0; i < t->nset; i++)
		if (t->set[i] >= nd->base && ((uint64) t->set[i] - (uint64) nd->base) % g == 0 && ((uint64) t->set[i] - (uint64) nd->base) / g <= INT64_MAX)
			codes[n++] = (int64) (((uint64) t->set[i] - (uint64) nd->base) / g);
	if (n == 0)
		res = t->kind == SIFT_CFT_SET ? SIFT_CF_FALSE : SIFT_CF_TRUE;
	else
		res = cf_node(nd->a, &(SiftCfTest) {.kind = t->kind,.set = codes,.nset = n}, &(SiftCfBounds) {0}, out);
	sift_free(codes);
	return res;
}

static SiftCfResult
cf_scale(const CcNode *nd, const SiftCfTest *t, uint64 *out)
{
	if (is_range(t))
		return cf_scale_range(nd, t, out);
	if (t->kind == SIFT_CFT_SET || t->kind == SIFT_CFT_NSET)
		return cf_scale_set(nd, t, out);
	return SIFT_CF_UNSUPPORTED;
}

static SiftCfResult
cf_node(const CcNode *nd, const SiftCfTest *t, const SiftCfBounds *bnd, uint64 *out)
{
	switch (nd->tag)
	{
		case SIFT_CCN_WRAP:
			return cf_node(nd->a, t, bnd, out);
		case SIFT_CCN_CONST:
			return sift_cf_for(&(SiftCfFor) {.base = nd->base,.bits = 0,.n = nd->n,.range = *bnd}, t, out);
		case SIFT_CCN_FORBYTE:
			return sift_cf_for(&(SiftCfFor) {.base = nd->base,.bits = 8 * nd->B,.n = nd->n,.packed = nd->words,.range = *bnd}, t, out);
		case SIFT_CCN_FORBIT:
			return sift_cf_for_vert(&(SiftCfForVert) {.base = nd->base,.bits = nd->W,.n = nd->n,.words = nd->words,.range = *bnd}, t, out);
		case SIFT_CCN_DELTA:
			return sift_cf_delta_vert(&(SiftCfDeltaVert) {.first = nd->base,.step = nd->mind,.bits = nd->W,.n = nd->n,.check = nd->chk,.words = nd->words,.range = *bnd}, t, out);
		case SIFT_CCN_RLE:
			return cf_rle(nd, t, out);
		case SIFT_CCN_DICT:
			return cf_dict(nd, t, out);
		case SIFT_CCN_SCALE:
			return cf_scale(nd, t, out);
		default:
			return SIFT_CF_UNSUPPORTED;
	}
}

bool
sift_cf_cc_filter(const SiftCc *cc, const SiftCfTest *t, uint64 *dense)
{
	SiftCfBounds bnd = {.known = true,.vmin = cc->min,.vmax = cc->max};
	uint32		nw = (cc->n + 63) / 64;
	SiftCfResult res = cc->n > 0 ? cf_node(cc->root, t, &bnd, dense) : SIFT_CF_UNSUPPORTED;

	if (res == SIFT_CF_UNSUPPORTED)
	{
		sift_cf_cc_stats.declined++;
		return false;
	}
	if (res == SIFT_CF_FALSE || res == SIFT_CF_TRUE)
	{
		memset(dense, res == SIFT_CF_TRUE ? 0xFF : 0, sizeof(uint64) * nw);
		if (res == SIFT_CF_TRUE && (cc->n & 63))
			dense[nw - 1] &= (UINT64CONST(1) << (cc->n & 63)) - 1;
	}
	memset(dense + nw, 0, sizeof(uint64) * (SIFT_COL_NWORDS(cc->n) - nw));
	sift_cf_cc_stats.node[Min(unwrap(cc->root)->tag, 9)]++;
	return true;
}
