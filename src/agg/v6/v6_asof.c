/*
 * agg/v6/v6_asof.c - the ASOF sorted merge (v6_asof.h).
 */
#include "agg/v6/v6_asof.h"

struct SiftAsofStream
{
	SiftAsofSpec spec;
	SiftAsofNext next;
	void	   *ctx;
	SiftAsofBatch b;
	uint32		pos;			/* the next row of b not read */
	bool		eof;
	bool		have_prev_dim;
	int64		prev_dim_key, prev_dim_ts;
	bool		have_cand;		/* BACKWARD: the last dim row that qualified for the current fact key */
	int64		cand_key, cand_ts;
	uint64		cand_tag;
	bool		have_prev_fact;
	int64		prev_fact_key, prev_fact_ts;
	SiftAsofStats st;
};

int64
sift_asof_i64_of_f64(double v)
{
	int64		bits;

	if (v != v)
		bits = INT64_C(0x7FF8000000000000);
	else if (v == 0)
		bits = 0;
	else
		memcpy(&bits, &v, sizeof(bits));
	return bits < 0 ? bits ^ INT64_C(0x7FFFFFFFFFFFFFFF) : bits;
}

SiftAsofStream *
sift_asof_open(MemoryContext cx, SiftAsofSpec spec, SiftAsofNext next, void *ctx)
{
	SiftAsofStream *s = MemoryContextAllocZero(cx, sizeof(SiftAsofStream));

	s->spec = spec;
	s->next = next;
	s->ctx = ctx;
	return s;
}

static inline bool
asof_bit(const uint64 *valid, uint32 i)
{
	return valid == NULL || ((valid[i >> 6] >> (i & 63)) & 1);
}

static inline int
cmp2(int64 k1, int64 t1, int64 k2, int64 t2)
{
	if (k1 != k2)
		return k1 < k2 ? -1 : 1;
	return t1 < t2 ? -1 : t1 > t2;
}

/* the next dim row with a k and a ts, not consumed: false at the end */
static bool
peek_dim(SiftAsofStream *s)
{
	for (;;)
	{
		while (s->pos < s->b.n)
		{
			uint32		i = s->pos;

			if (!asof_bit(s->b.key_valid, i) || !asof_bit(s->b.ts_valid, i))
			{
				s->st.dim_null_rows++;
				s->pos++;
				continue;
			}
			SIFT_INVARIANT(!s->have_prev_dim || cmp2(s->prev_dim_key, s->prev_dim_ts, s->b.key[i], s->b.ts[i]) <= 0,
						   "asof join: the dim rows are not in the order of (k, ts)");
			return true;
		}
		if (s->eof || !s->next(s->ctx, &s->b))
		{
			s->eof = true;
			s->b.n = 0;
			s->pos = 0;
			return false;
		}
		s->pos = 0;
	}
}

static void
take_dim(SiftAsofStream *s)
{
	s->prev_dim_key = s->b.key[s->pos];
	s->prev_dim_ts = s->b.ts[s->pos];
	s->have_prev_dim = true;
	s->pos++;
	s->st.dim_rows++;
}

/* does a dim row of this ts satisfy the condition against the fact ts */
static inline bool
qualifies(const SiftAsofSpec *sp, int64 dim_ts, int64 fact_ts)
{
	if (sp->dir == SIFT_ASOF_BACKWARD)
		return sp->strict ? dim_ts < fact_ts : dim_ts <= fact_ts;
	return sp->strict ? dim_ts > fact_ts : dim_ts >= fact_ts;
}

SiftAsofHit
sift_asof_probe(SiftAsofStream *s, int64 key, bool key_null, int64 ts, bool ts_null)
{
	SiftAsofHit hit = {false, 0, 0};

	s->st.fact_rows++;
	if (key_null || ts_null)
		return hit;
	SIFT_INVARIANT(!s->have_prev_fact || cmp2(s->prev_fact_key, s->prev_fact_ts, key, ts) <= 0,
				   "asof join: the fact rows are not in the order of (k, ts); use sift_asof_match_arrays");
	s->prev_fact_key = key;
	s->prev_fact_ts = ts;
	s->have_prev_fact = true;
	if (s->have_cand && s->cand_key != key)
		s->have_cand = false;

	while (peek_dim(s))
	{
		int64		dk = s->b.key[s->pos], dt = s->b.ts[s->pos];

		if (dk > key)
			break;
		if (s->spec.dir == SIFT_ASOF_BACKWARD)
		{
			/* everything up to (key, ts) is read; the last row of the key that qualifies is the candidate */
			if (dk == key && !qualifies(&s->spec, dt, ts))
				break;
			if (dk == key)
			{
				s->have_cand = true;
				s->cand_key = dk;
				s->cand_ts = dt;
				s->cand_tag = s->b.tag[s->pos];
			}
			take_dim(s);
		}
		else
		{
			/* the dim rows below (key, ts) are never wanted again, the first one at or after it is the match and stays for the next fact row */
			if (dk == key && qualifies(&s->spec, dt, ts))
				break;
			take_dim(s);
		}
	}
	if (s->spec.dir == SIFT_ASOF_BACKWARD)
	{
		if (s->have_cand && s->cand_key == key)
		{
			hit.found = true;
			hit.tag = s->cand_tag;
			hit.ts = s->cand_ts;
		}
	}
	else if (peek_dim(s) && s->b.key[s->pos] == key && qualifies(&s->spec, s->b.ts[s->pos], ts))
	{
		hit.found = true;
		hit.tag = s->b.tag[s->pos];
		hit.ts = s->b.ts[s->pos];
	}
	s->st.matched += hit.found;
	return hit;
}

const SiftAsofStats *
sift_asof_stats(const SiftAsofStream *s)
{
	return &s->st;
}

void
sift_asof_close(SiftAsofStream *s)
{
	pfree(s);
}

/* the first row of dim whose (key, ts) is above (k, t) (inclusive: at or above) */
static uint32
lower_bound(const SiftAsofArrays *d, int64 k, int64 t, bool inclusive_above)
{
	uint32		lo = 0, hi = d->n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		int			c = cmp2(d->key[mid], d->ts[mid], k, t);

		if (inclusive_above ? c < 0 : c <= 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

void
sift_asof_match_arrays(SiftAsofSpec spec, const SiftAsofArrays *dim, uint32 nfact, const int64 *key, const int64 *ts, const uint64 *key_valid, const uint64 *ts_valid, SiftAsofHit *hits)
{
	for (uint32 i = 0; i < nfact; i++)
	{
		uint32		at;
		bool		ok;

		hits[i] = (SiftAsofHit) {false, 0, 0};
		if (!asof_bit(key_valid, i) || !asof_bit(ts_valid, i))
			continue;
		if (spec.dir == SIFT_ASOF_BACKWARD)
		{
			/* the last row at or below (k, t) (below it when strict): one before the first row above */
			at = lower_bound(dim, key[i], ts[i], spec.strict);
			ok = at > 0 && dim->key[at - 1] == key[i];
			at = at - 1;
		}
		else
		{
			/* the first row at or above (k, t) (above it when strict) */
			at = lower_bound(dim, key[i], ts[i], !spec.strict);
			ok = at < dim->n && dim->key[at] == key[i];
		}
		if (ok)
			hits[i] = (SiftAsofHit) {true, dim->tag[at], dim->ts[at]};
	}
}
