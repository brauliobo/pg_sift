/*
 * col/f2/f2_dv_join.c - the deletion vectors of parts joined as they are (col/f2/f2_dv_join.h).
 */
#include "postgres.h"

#include "col/f2/f2_dv_join.h"
#include "core/xid_cache.h"

static void
shifted(const SiftDv *in, uint32 shift, SiftDv *out, FullTransactionId horizon, FullTransactionId *stripped)
{
	FullTransactionId settled = InvalidFullTransactionId;
	SiftDv	   *s = sift_dv_settle(in, horizon, &settled);

	*stripped = sift_fxid_max(*stripped, settled);
	for (int j = 0; j < s->ndirty; j++)
	{
		SiftDvGran	g = s->gran[j];

		if (g.ndead == 0)
			continue;
		g.granule_id += shift;
		out->gran[out->ndirty++] = g;
	}
}

SiftDv *
sift_dv_concat_shifted(const SiftDvJoinIn *in, int n, uint64 out_gen, uint64 out_dv_gen, FullTransactionId strip_horizon, FullTransactionId *stripped_out)
{
	uint32		shift = 0;
	int			ndirty = 0;
	SiftDv	   *out;

	*stripped_out = InvalidFullTransactionId;
	for (int i = 0; i < n; i++)
		ndirty += in[i].dv != NULL ? in[i].dv->ndirty : 0;
	if (ndirty == 0)
		return NULL;
	out = palloc0(sizeof(SiftDv));
	out->part_gen = out_gen;
	out->dv_gen = out_dv_gen;
	out->gran = palloc(ndirty * sizeof(SiftDvGran));
	for (int i = 0; i < n; i++)
	{
		if (in[i].dv != NULL)
			shifted(in[i].dv, shift, out, strip_horizon, stripped_out);
		shift += in[i].ngranules;
	}
	out->ngranules = shift;
	for (int j = 1; j < out->ndirty; j++)
		Assert(out->gran[j].granule_id > out->gran[j - 1].granule_id);
	return out->ndirty > 0 ? out : NULL;
}

bool
sift_dv_join_sparse(const uint64 *nrows, const uint64 *ndead, int n, int dead_pct)
{
	uint64		rows = 0,
				dead = 0;

	for (int i = 0; i < n; i++)
	{
		if (ndead[i] * 100 >= nrows[i] * (uint64) dead_pct)
			return false;
		rows += nrows[i];
		dead += ndead[i];
	}
	return dead * 100 < rows * (uint64) dead_pct;
}

bool
sift_dv_join_keeps_sketch(const uint64 *nrows, const uint64 *ndead, int n)
{
	uint64		rows = 0,
				dead = 0;

	for (int i = 0; i < n; i++)
	{
		rows += nrows[i];
		dead += ndead[i];
	}
	return dead * 100 <= rows;
}
