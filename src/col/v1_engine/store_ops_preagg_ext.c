/*
 * col/v1_engine/store_ops_preagg_ext.c - the sum of a line of a measure and the distinct values of a histogram dimension of a Format 1 unit (col/v1_engine/store_ops_preagg_ext.h).
 */
#include "postgres.h"

#include "agg/vexpr/str.h"
#include "col/v1_engine/store_ops_preagg_ext.h"

bool
sift_v1_line_acc(const SiftV6Linear *lin, int64 n, SiftSrcI128 sum, int64 lo, int64 hi, V6Acc *out)
{
	SiftI128	s;

	*out = (V6Acc) {.n = n};
	if (n == 0)
		return true;
	if (!sift_v6_linear_fits(lin, lo, hi) || !sift_v6_linear_sum(lin, (SiftI128) {(uint64) sum, (int64) (sum >> 64)}, n, &s))
		return false;
	v6acc_set_i128(out, s);
	return true;
}

static void
flush(SiftV6Group *g, int agg, SiftVec *v, uint16 n)
{
	SiftVec    *keys[1] = {v};

	v->len = n;
	sift_v6_group_merge_dist(g, agg, keys, v, n);
}

void
sift_v1_hist_merge_dist(SiftV6Group *g, int agg, const SiftSrcHist *h, bool text, void *buf, SiftV1HistKeep keep, const void *arg)
{
	SiftVec		v = {.data = buf,.dense = true,.all_valid = true,.type = text ? SVEC_STRVIEW : SVEC_I64};
	uint16		n = 0;

	for (uint32 r = 0; r < h->ngroups; r++)
	{
		if (h->count[r] == 0 || (h->has_null && r == h->ngroups - 1) || (keep != NULL && !keep(arg, h, r)))
			continue;
		if (text)
			((SiftStrView *) buf)[n] = sift_vstr_make(h->dict->bytes + h->dict->off[h->key[r]], h->dict->off[h->key[r] + 1] - h->dict->off[h->key[r]]);
		else
			((int64 *) buf)[n] = h->key[r];
		if (++n == SIFT_VEC_LEN)
		{
			flush(g, agg, &v, n);
			n = 0;
		}
	}
	if (n > 0)
		flush(g, agg, &v, n);
}
