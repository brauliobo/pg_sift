/*
 * agg/v6/v6_proj_in_seek.c - the streams of a multi-value IN on the leading sort column (agg/v6/v6_proj_in_seek.h).
 */
#include "core/host.h"

#include "agg/v6/v6_proj_in_seek.h"

static bool
same_value(bool bytes, const SiftProjConst *a, const SiftProjConst *b)
{
	return bytes ? a->len == b->len && (a->len == 0 || memcmp(a->s, b->s, a->len) == 0) : a->i == b->i;
}

int
sift_v6_in_seek_values(const SiftProjPred *in, SiftProjConst *out)
{
	int			n = 0;

	for (int i = 0; i < in->nvals; i++)
	{
		bool		seen = false;

		for (int k = 0; k < n && !seen; k++)
			seen = same_value(in->bytes, &out[k], &in->vals[i]);
		if (seen)
			continue;
		if (n == SIFT_PROJ_IN_SEEK_MAX)
			return -1;
		out[n++] = in->vals[i];
	}
	return n;
}

void
sift_v6_in_seek_preds(const SiftProjPred *preds, int n, int in_idx, const SiftProjConst *val, SiftProjPred *out, SiftProjConst *one)
{
	memcpy(out, preds, n * sizeof(SiftProjPred));
	*one = *val;
	out[in_idx].op = SPO_EQ;
	out[in_idx].nvals = 1;
	out[in_idx].vals = one;
}

int
sift_v6_in_seek_pred_index(const SiftProjSpec *order, const SiftV6SortUse *use, const SiftProjPred *preds, int n)
{
	if (!use->in_merge)
		return -1;
	for (int i = 0; i < n; i++)
		if (preds[i].op == SPO_IN && preds[i].leaf == order->key[use->in_key_idx].leaf)
			return i;
	return -1;
}

SiftProjSpec
sift_v6_in_seek_order(const SiftV6ProjQuery *q)
{
	SiftProjSpec s = {.kind = SIFT_PROJ_SORT,.nkeys = q->norder,.distinct_leaf = -1};

	memcpy(s.key, q->order, q->norder * sizeof(SiftProjKey));
	return s;
}
