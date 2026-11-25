/*
 * agg/v6/gather.c - the rows of a vector picked by an index list (agg/v6/gather.h).
 */
#include "core/host.h"

#include "agg/v6/gather.h"

size_t
sift_gather_width(SiftVecType t)
{
	switch (t)
	{
		case SVEC_BOOL:
			return sizeof(uint8);
		case SVEC_I32:
		case SVEC_DICT32:
			return sizeof(uint32);
		case SVEC_I64:
		case SVEC_F64:
		case SVEC_NUM64:
		case SVEC_DATUM:
			return sizeof(uint64);
		case SVEC_NUM128:
			return sizeof(SiftI128);
		case SVEC_NUM256:
			return sizeof(SiftI256);
		case SVEC_UUID:
			return sizeof(SiftUuid16);
		case SVEC_INTERVAL:
			return sizeof(SiftInterval16);
		case SVEC_STRVIEW:
			return sizeof(SiftStrView);
	}
	__builtin_unreachable();
}

static inline bool
src_valid(const SiftVec *src, uint32 r)
{
	return src->all_valid || ((src->valid[r >> 6] >> (r & 63)) & 1);
}

void
sift_vec_gather(SiftVec *out, uint64 *out_valid, void *out_data, const SiftVec *src, const uint32 *idx, uint32 n)
{
	const size_t w = sift_gather_width(src->type);
	char	   *dst = out_data;
	const char *from = src->data;

	memset(out_valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
	{
		const bool	ok = idx[i] != SIFT_GATHER_NONE && src_valid(src, idx[i]);

		if (ok)
			memcpy(dst + i * w, from + (size_t) idx[i] * w, w);
		else
			memset(dst + i * w, 0, w);
		out_valid[i >> 6] |= (uint64) ok << (i & 63);
	}
	*out = (SiftVec) {.data = out_data,.valid = out_valid,.len = (uint16) n,.all_valid = false,.dense = true,.type = src->type,.scale = src->scale,.dict = src->dict,.typid = src->typid,.typlen = src->typlen,.typbyval = src->typbyval};
}

void
sift_vec_gather_nan(SiftVec *out, uint64 *out_nan, const SiftVec *src, const uint32 *idx, uint32 n)
{
	out->nan = NULL;
	if (src->nan == NULL)
		return;
	memset(out_nan, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		out_nan[i >> 6] |= (uint64) (idx[i] != SIFT_GATHER_NONE && ((src->nan[idx[i] >> 6] >> (idx[i] & 63)) & 1)) << (i & 63);
	out->nan = out_nan;
}
