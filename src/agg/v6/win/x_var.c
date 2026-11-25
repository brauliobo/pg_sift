/*
 * agg/v6/win/x_var.c - the variance and standard deviation state of v6_winx_var.h.
 */
#include "core/host.h"

#include <math.h>

#include "agg/v6/win/x_var.h"

typedef unsigned __int128 u128;

bool
sift_winvar_f64_add(SiftWinVarF64 *s, double x)
{
	const uint64 old_n = s->n;
	const double old_sx = s->sx;

	s->n++;
	s->sx += x;
	if (old_n > 0)
	{
		const double n = (double) s->n;
		const double tmp = x * n - s->sx;

		s->sxx += tmp * tmp / (n * (double) old_n);
		if (isinf(s->sx) || isinf(s->sxx))
		{
			if (!isinf(old_sx) && !isinf(x))
				return false;
			s->sxx = NAN;
		}
	}
	else if (isnan(x) || isinf(x))
		s->sxx = NAN;
	return true;
}

bool
sift_winvar_f64_final(const SiftWinVarF64 *s, SiftWinVarKind kind, double *out)
{
	const double n = (double) s->n;
	double		var;

	if (s->n == 0 || (sift_winvar_is_sample(kind) && s->n <= 1))
		return false;
	var = s->sxx / (sift_winvar_is_sample(kind) ? n - 1.0 : n);
	*out = sift_winvar_is_stddev(kind) ? sqrt(var) : var;
	return true;
}

static bool
u256_add_u128(SiftU256 *a, u128 b)
{
	u128		carry = b;

	for (int i = 0; i < 4 && carry != 0; i++)
	{
		const u128	t = (u128) a->w[i] + (uint64) carry;

		a->w[i] = (uint64) t;
		carry = (carry >> 64) + (t >> 64);
	}
	return carry == 0;
}

static SiftU256
u256_mul_u128(u128 a, u128 b)
{
	const uint64 a0 = (uint64) a,
				a1 = (uint64) (a >> 64),
				b0 = (uint64) b,
				b1 = (uint64) (b >> 64);
	const u128	p00 = (u128) a0 * b0,
				p01 = (u128) a0 * b1,
				p10 = (u128) a1 * b0,
				p11 = (u128) a1 * b1;
	const u128	mid = (p00 >> 64) + (uint64) p01 + (uint64) p10;
	const u128	hi = (mid >> 64) + (p01 >> 64) + (p10 >> 64) + p11;

	return (SiftU256) {{(uint64) p00, (uint64) mid, (uint64) hi, (uint64) (hi >> 64)}};
}

static bool
u256_mul_u64(const SiftU256 *a, uint64 m, SiftU256 *out)
{
	u128		carry = 0;

	for (int i = 0; i < 4; i++)
	{
		const u128	t = (u128) a->w[i] * m + carry;

		out->w[i] = (uint64) t;
		carry = t >> 64;
	}
	return carry == 0;
}

static int
u256_cmp(const SiftU256 *a, const SiftU256 *b)
{
	for (int i = 3; i >= 0; i--)
		if (a->w[i] != b->w[i])
			return a->w[i] < b->w[i] ? -1 : 1;
	return 0;
}

static void
u256_sub(SiftU256 *a, const SiftU256 *b)
{
	uint64		borrow = 0;

	for (int i = 0; i < 4; i++)
	{
		const uint64 x = a->w[i],
					y = b->w[i];
		const uint64 d = x - y - borrow;

		borrow = (x < y) || (borrow && x == y);
		a->w[i] = d;
	}
}

bool
sift_winvar_int_add(SiftWinVarInt *s, int64 x)
{
	const uint64 mag = x < 0 ? (uint64) 0 - (uint64) x : (uint64) x;

	s->n++;
	s->sx += x;
	return u256_add_u128(&s->sxx, (u128) mag * mag);
}

bool
sift_winvar_int_final(const SiftWinVarInt *s, SiftWinVarKind kind, SiftWinVarExact *out)
{
	const u128	sx_mag = s->sx < 0 ? (u128) 0 - (u128) s->sx : (u128) s->sx;
	SiftU256	n_sxx,
				sx_sq;

	*out = (SiftWinVarExact) {.null = s->n == 0 || (sift_winvar_is_sample(kind) && s->n <= 1)};
	if (out->null)
		return true;
	if (!u256_mul_u64(&s->sxx, s->n, &n_sxx))
		return false;
	sx_sq = u256_mul_u128(sx_mag, sx_mag);
	if (u256_cmp(&n_sxx, &sx_sq) > 0)
	{
		u256_sub(&n_sxx, &sx_sq);
		out->num = n_sxx;
	}
	out->den = sift_winvar_is_sample(kind) ? (u128) s->n * (s->n - 1) : (u128) s->n * s->n;
	return true;
}

static uint64
u256_divmod_u64(SiftU256 *v, uint64 d)
{
	u128		rem = 0;

	for (int i = 3; i >= 0; i--)
	{
		const u128	cur = (rem << 64) | v->w[i];

		v->w[i] = (uint64) (cur / d);
		rem = cur % d;
	}
	return (uint64) rem;
}

static bool
digits_to_dec(char *digits, int nd, int scale, char *buf, size_t cap)
{
	int			pos = 0;

	while (nd < scale + 1)
		digits[nd++] = '0';
	if ((size_t) (nd + 2) > cap)
		return false;
	for (int i = nd - 1; i >= 0; i--)
	{
		buf[pos++] = digits[i];
		if (i == scale && scale > 0)
			buf[pos++] = '.';
	}
	buf[pos] = '\0';
	return true;
}

bool
sift_u256_to_dec(const SiftU256 *v, int scale, char *buf, size_t cap)
{
	char		digits[96];
	SiftU256	t = *v;
	int			nd = 0;

	if (scale < 0 || scale > 40)
		return false;
	do
	{
		uint64		chunk = u256_divmod_u64(&t, UINT64CONST(10000000000000000000));
		const bool	more = (t.w[0] | t.w[1] | t.w[2] | t.w[3]) != 0;

		for (int i = 0; i < 19 && (more || chunk != 0); i++)
		{
			SIFT_INVARIANT(nd < (int) sizeof(digits), "a 256-bit value has more than %d digits", (int) sizeof(digits));
			digits[nd++] = (char) ('0' + chunk % 10);
			chunk /= 10;
		}
		if (!more)
			break;
	} while (true);
	if (nd == 0)
		digits[nd++] = '0';
	return digits_to_dec(digits, nd, scale, buf, cap);
}

bool
sift_u128_to_dec(unsigned __int128 v, char *buf, size_t cap)
{
	char		digits[48];
	int			nd = 0;

	do
	{
		SIFT_INVARIANT(nd < (int) sizeof(digits), "a 128-bit value has more than %d digits", (int) sizeof(digits));
		digits[nd++] = (char) ('0' + (int) (v % 10));
		v /= 10;
	} while (v != 0);
	return digits_to_dec(digits, nd, 0, buf, cap);
}
