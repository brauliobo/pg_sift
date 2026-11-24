/*
 * agg/v6/par/derive.c - the computed keys and arguments for the compute threads (agg/v6/par/derive.h). Nothing here calls the server.
 */
#include "postgres.h"

#include "agg/v6/par/run_int.h"
#include "agg/v6/v6_expr_str.h"

static inline __attribute__((always_inline)) bool
extract_words(const ColView *cv, const ParLoad *ld, int64 unit_us, uint64 flip, uint64 *out)
{
	bool		all_ok = true;

	PAR_EACH_ROW(ld, r, i)
	{
		int64		x = 0;

		all_ok &= sift_vx_extract_one(cv->i[r], unit_us, &x);
		out[i] = (uint64) x ^ flip;
	}
	return all_ok;
}

static bool
extract_of(const SiftV6Derive *d, const ColView *cv, const ParLoad *ld, uint64 flip, uint64 *out)
{
	if (d->aux == 60000000)
		return extract_words(cv, ld, 60000000, flip, out);
	if (d->aux == INT64CONST(3600000000))
		return extract_words(cv, ld, INT64CONST(3600000000), flip, out);
	return extract_words(cv, ld, d->aux, flip, out);
}

bool
par_src_ints(const ParSrc *s, const ParLoad *ld, uint64 flip, uint64 *out)
{
	const ColView *cv = &ld->cv[s->at];

	if (s->d == NULL)
	{
		PAR_EACH_ROW(ld, r, i)
			out[i] = (uint64) cv->i[r] ^ flip;
	}
	else if (s->d->kind == SVD_STRLEN)
	{
		PAR_EACH_ROW(ld, r, i)
		{
			const SiftStrRef v = par_str_at(cv, r);

			out[i] = (uint64) sift_vx_strlen(s->d, v.p, v.len) ^ flip;
		}
	}
	else
		return extract_of(s->d, cv, ld, flip, out);
	return true;
}

static bool
case_holds(const ParSrc *s, const ParLoad *ld, uint32 r)
{
	for (int i = 0; i < s->d->ncond; i++)
		if (ld->cv[s->cond[i]].i[r] != s->d->cval[i])
			return false;
	return true;
}

void
par_src_strs(const ParSrc *s, const ParLoad *ld, SiftStrRef *out)
{
	const ColView *cv = &ld->cv[s->at];

	if (s->d == NULL)
	{
		PAR_EACH_ROW(ld, r, i)
			out[i] = par_str_at(cv, r);
	}
	else if (s->d->kind == SVD_URL_HOST)
	{
		PAR_EACH_ROW(ld, r, i)
		{
			const SiftStrRef v = par_str_at(cv, r);

			sift_vx_url_host(v.p, v.len, &out[i].p, &out[i].len);
		}
	}
	else
	{
		const SiftStrRef other = {s->d->else_bytes, s->d->else_len};

		PAR_EACH_ROW(ld, r, i)
			out[i] = case_holds(s, ld, r) ? par_str_at(cv, r) : other;
	}
}
