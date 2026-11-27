/*
 * agg/filter/eval.c - the clauses of FILTER (WHERE ..) over the vectors of a batch (v6_filter_eval.h).
 */
#include "agg/filter/eval.h"
#include "agg/batch.h"

#include <math.h>

int
sift_f8_cmp(double a, double b)
{
	if (isnan(a))
		return isnan(b) ? 0 : 1;
	return isnan(b) ? -1 : (a < b ? -1 : a > b);
}

static inline bool
op_holds(SiftPredOp op, int c)
{
	switch (op)
	{
		case SPO_EQ:
			return c == 0;
		case SPO_NE:
			return c != 0;
		case SPO_LT:
			return c < 0;
		case SPO_LE:
			return c <= 0;
		case SPO_GT:
			return c > 0;
		default:
			return c >= 0;
	}
}

static inline int
cmp_i64(int64 a, int64 b)
{
	return a < b ? -1 : a > b;
}

static inline int64
int_at(const SiftVec *v, uint32 r)
{
	switch (v->type)
	{
		case SVEC_I32:
			return ((const int32 *) v->data)[r];
		case SVEC_BOOL:
			return ((const uint8 *) v->data)[r];
		default:
			return ((const int64 *) v->data)[r];
	}
}

static inline const uint8 *
str_bytes(const SiftStrView *s)
{
	return s->len <= 12 ? s->prefix : (const uint8 *) s->u.ptr;
}

static inline bool
str_equal(const SiftStrView *s, const uint8 *b, uint32 len)
{
	return s->len == len && (len == 0 || memcmp(str_bytes(s), b, len) == 0);
}

/* a numeric NaN is above every number and equal to none of them */
static inline bool
nan_holds(SiftPredOp op)
{
	return op == SPO_NE || op == SPO_GT || op == SPO_GE || op == SPO_NOT_IN;
}

static void
eval_int(const SiftPred *p, const SiftVec *v, uint32 n, uint64 *out)
{
	for (uint32 r = 0; r < n; r++)
	{
		const int64 x = int_at(v, r);
		bool		hit = false;

		if (sift_vec_nan_at(v, r))
			hit = nan_holds(p->op);
		else if (!sift_vec_valid_at(v, r))
			continue;
		else if (p->op == SPO_IN || p->op == SPO_NOT_IN)
		{
			for (uint32 k = 0; k < p->nin && !hit; k++)
				hit = x == p->in_i[k];
			hit ^= p->op == SPO_NOT_IN;
		}
		else
			hit = op_holds(p->op, cmp_i64(x, p->i));
		out[r >> 6] |= (uint64) hit << (r & 63);
	}
}

static void
eval_f64(const SiftPred *p, const SiftVec *v, uint32 n, uint64 *out)
{
	const double *d = v->data;

	for (uint32 r = 0; r < n; r++)
	{
		bool		hit = false;

		if (!sift_vec_valid_at(v, r))
			continue;
		if (p->op == SPO_IN || p->op == SPO_NOT_IN)
		{
			for (uint32 k = 0; k < p->nin && !hit; k++)
				hit = sift_f8_cmp(d[r], p->in_f[k]) == 0;
			hit ^= p->op == SPO_NOT_IN;
		}
		else
			hit = op_holds(p->op, sift_f8_cmp(d[r], p->f));
		out[r >> 6] |= (uint64) hit << (r & 63);
	}
}

static void
eval_str(const SiftPred *p, const SiftVec *v, uint32 n, uint64 *out)
{
	const SiftStrView *s = v->data;

	for (uint32 r = 0; r < n; r++)
	{
		bool		hit = false;

		if (!sift_vec_valid_at(v, r))
			continue;
		if (p->op == SPO_IN || p->op == SPO_NOT_IN)
		{
			for (uint32 k = 0; k < p->nin && !hit; k++)
				hit = str_equal(&s[r], p->in_s[k].bytes, p->in_s[k].len);
			hit ^= p->op == SPO_NOT_IN;
		}
		else if (p->op == SPO_PREFIX)
			hit = s[r].len >= p->s.len && (p->s.len == 0 || memcmp(str_bytes(&s[r]), p->s.bytes, p->s.len) == 0);
		else
			hit = str_equal(&s[r], p->s.bytes, p->s.len) == (p->op == SPO_EQ);
		out[r >> 6] |= (uint64) hit << (r & 63);
	}
}

void
sift_pred_eval(const SiftPred *p, const SiftVec *v, uint32 n_raw, uint64 *out)
{
	memset(out, 0, sizeof(uint64) * SIFT_VEC_WORDS);
	if (p->op == SPO_IS_NULL || p->op == SPO_NOT_NULL)
	{
		for (uint32 r = 0; r < n_raw; r++)
			out[r >> 6] |= (uint64) ((sift_vec_valid_at(v, r) || sift_vec_nan_at(v, r)) == (p->op == SPO_NOT_NULL)) << (r & 63);
		return;
	}
	switch (p->kind)
	{
		case SPK_INT:
			eval_int(p, v, n_raw, out);
			break;
		case SPK_F64:
			eval_f64(p, v, n_raw, out);
			break;
		default:
			eval_str(p, v, n_raw, out);
			break;
	}
}

void
sift_clause_eval(const SiftClause *c, SiftVec *const *vecs, uint32 n_raw, uint64 *out)
{
	uint64		one[SIFT_VEC_WORDS];
	const uint32 words = (n_raw + 63) / 64;

	memset(out, 0, sizeof(uint64) * SIFT_VEC_WORDS);
	for (uint32 w = 0; w < words; w++)
		out[w] = w + 1 < words || (n_raw & 63) == 0 ? ~UINT64_C(0) : (UINT64_C(1) << (n_raw & 63)) - 1;
	for (int i = 0; i < c->npred;)
	{
		const int	group = c->pred[i].group;
		uint64		any[SIFT_VEC_WORDS] = {0};

		do
		{
			sift_pred_eval(&c->pred[i], vecs[c->pred[i].vec], n_raw, one);
			for (uint32 w = 0; w < words; w++)
				any[w] |= one[w];
			i++;
		} while (group != 0 && i < c->npred && c->pred[i].group == group);
		for (uint32 w = 0; w < words; w++)
			out[w] &= any[w];
	}
}
