/*
 * tests/vk_v6_oracle.c - the scalar oracle of the aggregate kernels: the program of a job run row by row in plain C over the logical data
 * (the semantics of agg/vexpr.h: PostgreSQL's errors on the rows that are live, three-valued masks, the order of floats), with the
 * compiler's overflow builtins, 128-bit integers and the C library's calendar (gmtime, timegm) where the shaders compute by hand.
 */
#include <math.h>
#include <time.h>

#include "vk_v6_common.h"

static const double P10[19] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
static const int64_t P10I[19] = {1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL, 100000000LL, 1000000000LL, 10000000000LL,
	100000000000LL, 1000000000000LL, 10000000000000LL, 100000000000000LL, 1000000000000000LL, 10000000000000000LL, 100000000000000000LL,
	1000000000000000000LL};

typedef struct VM
{
	int64_t		R[16];
	uint32_t	RV,
				MT,
				MK;
	uint32_t	err,
				err_op;
} VM;

static double
dbl(int64_t v)
{
	double		d;

	memcpy(&d, &v, 8);
	return d;
}

static int64_t
bitsd(double d)
{
	int64_t		v;

	memcpy(&v, &d, 8);
	return v;
}

static void
rset(VM *m, uint32_t r, int64_t v, bool ok)
{
	m->R[r] = v;
	m->RV = ok ? (m->RV | 1u << r) : (m->RV & ~(1u << r));
}

static bool
rv(const VM *m, uint32_t r)
{
	return (m->RV >> r) & 1;
}

static void
mset(VM *m, uint32_t s, bool t, bool k)
{
	m->MT = (t && k) ? (m->MT | 1u << s) : (m->MT & ~(1u << s));
	m->MK = k ? (m->MK | 1u << s) : (m->MK & ~(1u << s));
}

static bool
mt(const VM *m, uint32_t s)
{
	return (m->MT >> s) & 1;
}

static bool
mk(const VM *m, uint32_t s)
{
	return (m->MK >> s) & 1;
}

static int
fcmp(double a, double b)
{
	if (isnan(a) || isnan(b))
		return isnan(a) ? (isnan(b) ? 0 : 1) : -1;
	return a < b ? -1 : a > b ? 1 : 0;
}

static bool
cmp_ok(int c, int op)
{
	switch (op)
	{
		case SIFT_VK_CMP_EQ:
			return c == 0;
		case SIFT_VK_CMP_NE:
			return c != 0;
		case SIFT_VK_CMP_LT:
			return c < 0;
		case SIFT_VK_CMP_LE:
			return c <= 0;
		case SIFT_VK_CMP_GT:
			return c > 0;
	}
	return c >= 0;
}

static void
err(VM *m, uint32_t code, uint32_t op)
{
	if (!m->err)
	{
		m->err = code;
		m->err_op = op;
	}
}

/* the calendar by the C library: days since 2000-01-01 or microseconds since 2000-01-01 */
static int64_t
floordiv(int64_t a, int64_t b)
{
	int64_t		q = a / b;

	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static void
op_time(VM *m, uint32_t op, uint32_t ty, uint32_t dst, uint32_t a, int32_t aux, uint32_t i, bool live)
{
	int64_t		x = m->R[a];
	bool		ok = rv(m, a),
				date = ty == SIFT_VK_T_I32;
	int64_t		days = date ? x : floordiv(x, 86400000000LL);
	time_t		t = (time_t) (days * 86400 + 946684800);
	struct tm	tm = {0};

	if (ok && !date && live && (x == INT64_MAX || x == INT64_MIN))
	{
		err(m, SIFT_VK_PE_TIME, i);
		ok = false;
	}
	gmtime_r(&t, &tm);
	if (op == SIFT_VK_OP_DATE_TRUNC)
	{
		int64_t		r;

		if (aux <= 3)
		{
			int64_t		per = aux == 0 ? 1000000LL : aux == 1 ? 60000000LL : aux == 2 ? 3600000000LL : 86400000000LL;

			r = floordiv(x, per) * per;
		}
		else
		{
			struct tm	u = tm;
			time_t		s;

			u.tm_sec = u.tm_min = u.tm_hour = 0;
			if (aux == 4)
				u.tm_mday -= (tm.tm_wday + 6) % 7;
			else if (aux == 5)
				u.tm_mday = 1;
			else if (aux == 6)
			{
				u.tm_mday = 1;
				u.tm_mon = tm.tm_mon / 3 * 3;
			}
			else
			{
				u.tm_mday = 1;
				u.tm_mon = 0;
			}
			s = timegm(&u);
			r = ((int64_t) s - 946684800) * 1000000LL;
		}
		rset(m, dst, r, ok);
	}
	else
	{
		int64_t		tod = date ? 0 : x - days * 86400000000LL,
					r;

		switch (aux)
		{
			case 0: r = tm.tm_year + 1900; break;
			case 1: r = tm.tm_mon / 3 + 1; break;
			case 2: r = tm.tm_mon + 1; break;
			case 3: r = tm.tm_mday; break;
			case 4: r = tm.tm_wday; break;
			case 5: r = tm.tm_wday == 0 ? 7 : tm.tm_wday; break;
			case 6: r = tm.tm_yday + 1; break;
			case 7: r = tod / 3600000000LL; break;
			case 8: r = (tod / 60000000LL) % 60; break;
			default: r = date ? (days + 10957) * 86400 : x + 946684800000000LL; break;
		}
		rset(m, dst, r, ok);
	}
}

static void
vm_op(VM *m, const SiftVkJob *job, const TGran *g, uint32_t row, uint32_t i)
{
	const uint32_t *o = &job->ops[i * SIFT_VK_OP_WORDS];
	uint32_t	op = o[0] & 255,
				ty = (o[0] >> 8) & 255,
				tt = (o[0] >> 16) & 255,
				fl = o[0] >> 24,
				dst = o[1] & 255,
				a = (o[1] >> 8) & 255,
				b = (o[1] >> 16) & 255,
				c = o[1] >> 24,
				act = o[2] & 255;
	int			scale = (int16_t) ((o[2] >> 8) & 0xFFFF);
	int32_t		aux = (int32_t) o[3];
	int64_t		imm = (int64_t) (((uint64_t) o[5] << 32) | o[4]);
	bool		live = mt(m, act);

	switch (op)
	{
		case SIFT_VK_OP_LOAD_COL:
			{
				bool		ok = g->col[a].ok[row];
				int64_t		v = g->col[a].v[row];

				if (ty == SIFT_VK_T_F64)
					v = bitsd(f8_from_key(v));
				rset(m, dst, ok ? v : 0, ok);
				break;
			}
		case SIFT_VK_OP_LOAD_CONST:
			rset(m, dst, imm, true);
			break;
		case SIFT_VK_OP_LOAD_NULL:
		case SIFT_VK_OP_INIT_NULL:
			rset(m, dst, 0, false);
			break;
		case SIFT_VK_OP_CAST:
			{
				int64_t		v = m->R[a];
				bool		ok = rv(m, a);

				if (tt == SIFT_VK_T_F64 && ty != SIFT_VK_T_F64)
					rset(m, dst, bitsd(ty == SIFT_VK_T_NUM ? (double) v / P10[scale] : (double) v), ok);
				else if (tt == SIFT_VK_T_NUM && ty != SIFT_VK_T_NUM && ty != SIFT_VK_T_F64)
				{
					int64_t		r;

					if (__builtin_mul_overflow(v, P10I[scale], &r) && ok && live)
						err(m, SIFT_VK_PE_INT8, i);
					rset(m, dst, r, ok);
				}
				else
					rset(m, dst, v, ok);
				break;
			}
		case SIFT_VK_OP_ADD:
		case SIFT_VK_OP_SUB:
		case SIFT_VK_OP_MUL:
		case SIFT_VK_OP_DIV:
			{
				int64_t		x = m->R[a],
							y = (fl & 1) ? imm : m->R[b],
							r = 0;
				bool		ok = rv(m, a) && ((fl & 1) || rv(m, b)),
							bad = false;

				if (ty == SIFT_VK_T_F64)
				{
					double		p = dbl(x),
								q = dbl(y),
								z = 0;

					if (op == SIFT_VK_OP_DIV && q == 0)
					{
						if (live)
							err(m, SIFT_VK_PE_DIV0, i);
					}
					else
					{
						z = op == SIFT_VK_OP_ADD ? p + q : op == SIFT_VK_OP_SUB ? p - q : op == SIFT_VK_OP_MUL ? p * q : p / q;
						if (live && isinf(z) && !isinf(p) && !isinf(q))
							err(m, SIFT_VK_PE_FLOAT_OVF, i);
						if (live && (op == SIFT_VK_OP_MUL || op == SIFT_VK_OP_DIV) && z == 0 && p != 0 && (op == SIFT_VK_OP_DIV || q != 0))
							err(m, SIFT_VK_PE_FLOAT_UNF, i);
					}
					r = bitsd(z);
				}
				else
				{
					if (op == SIFT_VK_OP_ADD)
						bad = __builtin_add_overflow(x, y, &r);
					else if (op == SIFT_VK_OP_SUB)
						bad = __builtin_sub_overflow(x, y, &r);
					else if (op == SIFT_VK_OP_MUL)
						bad = __builtin_mul_overflow(x, y, &r);
					else if (y == 0)
					{
						if (live)
							err(m, SIFT_VK_PE_DIV0, i);
					}
					else if (x == INT64_MIN && y == -1)
						bad = true;
					else
						r = x / y;
					if (ty == SIFT_VK_T_I32)
						bad = bad || r < INT32_MIN || r > INT32_MAX;
					if (bad && live)
						err(m, ty == SIFT_VK_T_I32 ? SIFT_VK_PE_INT4 : SIFT_VK_PE_INT8, i);
				}
				rset(m, dst, r, ok);
				break;
			}
		case SIFT_VK_OP_NEG:
		case SIFT_VK_OP_ABS:
			{
				int64_t		x = m->R[a],
							r;
				bool		ok = rv(m, a),
							bad;

				if (ty == SIFT_VK_T_F64)
					r = bitsd(op == SIFT_VK_OP_NEG ? -dbl(x) : fabs(dbl(x)));
				else
				{
					bad = ty == SIFT_VK_T_I32 ? x == INT32_MIN : x == INT64_MIN;
					if (op == SIFT_VK_OP_ABS && x >= 0)
						bad = false;
					r = (op == SIFT_VK_OP_NEG || x < 0) ? (int64_t) (0 - (uint64_t) x) : x;
					if (bad && live)
						err(m, ty == SIFT_VK_T_I32 ? SIFT_VK_PE_INT4 : SIFT_VK_PE_INT8, i);
				}
				rset(m, dst, r, ok);
				break;
			}
		case SIFT_VK_OP_CMP:
			{
				int64_t		x = m->R[a],
							y = (fl & 1) ? imm : m->R[b];
				bool		ok = rv(m, a) && ((fl & 1) || rv(m, b));
				int			cr = ty == SIFT_VK_T_F64 ? fcmp(dbl(x), dbl(y)) : (x < y ? -1 : x > y ? 1 : 0);

				mset(m, dst, cmp_ok(cr, aux), ok);
				break;
			}
		case SIFT_VK_OP_IS_NULL:
			mset(m, dst, aux ? rv(m, a) : !rv(m, a), true);
			break;
		case SIFT_VK_OP_MASK_AND:
			{
				bool		ta = mt(m, a), tb = mt(m, b), ka = mk(m, a), kb = mk(m, b);

				mset(m, dst, ta && tb, (ka && kb) || (ka && !ta) || (kb && !tb));
				break;
			}
		case SIFT_VK_OP_MASK_OR:
			{
				bool		ta = mt(m, a), tb = mt(m, b), ka = mk(m, a), kb = mk(m, b);

				mset(m, dst, ta || tb, (ka && kb) || (ka && ta) || (kb && tb));
				break;
			}
		case SIFT_VK_OP_MASK_NOT:
			mset(m, dst, mk(m, a) && !mt(m, a), mk(m, a));
			break;
		case SIFT_VK_OP_MASK_COL:
			{
				bool		ok = g->col[a].ok[row];

				mset(m, dst, ok && g->col[a].v[row] != 0, ok);
				break;
			}
		case SIFT_VK_OP_MASK_CONST:
			mset(m, dst, fl & 1, fl & 2);
			break;
		case SIFT_VK_OP_COPY:
			rset(m, dst, m->R[a], rv(m, a));
			break;
		case SIFT_VK_OP_BLEND:
			if (mt(m, c))
				rset(m, dst, m->R[a], rv(m, a));
			break;
		case SIFT_VK_OP_ACT_COPY:
			mset(m, dst, mt(m, a), true);
			break;
		case SIFT_VK_OP_ACT_AND:
			mset(m, dst, mt(m, a) && mt(m, b), true);
			break;
		case SIFT_VK_OP_ACT_ANDN:
			mset(m, dst, mt(m, a) && !mt(m, b), true);
			break;
		case SIFT_VK_OP_ACT_NOTFALSE:
			mset(m, dst, mt(m, a) && (mt(m, b) || !mk(m, b)), true);
			break;
		case SIFT_VK_OP_ACT_NOTVALID:
			mset(m, dst, mt(m, a) && !rv(m, b), true);
			break;
		case SIFT_VK_OP_ACT_UNKNOWN:
			mset(m, dst, mt(m, a) && !mk(m, b), true);
			break;
		case SIFT_VK_OP_SKIP_IF_EMPTY:
			break;
		case SIFT_VK_OP_RESCALE:
			{
				int64_t		r;

				if (__builtin_mul_overflow(m->R[a], P10I[aux], &r) && live && rv(m, a))
					err(m, SIFT_VK_PE_INT8, i);
				rset(m, dst, r, rv(m, a));
				break;
			}
		case SIFT_VK_OP_DATE_TRUNC:
		case SIFT_VK_OP_EXTRACT:
			op_time(m, op, ty, dst, a, aux, i, live);
			break;
		case SIFT_VK_OP_MASK_BLEND:
			if (mt(m, c))
				mset(m, dst, mt(m, a), mk(m, a));
			break;
		case SIFT_VK_OP_MASK_UNKNOWN:
			mset(m, dst, false, false);
			break;
		case SIFT_VK_OP_MASK_ISNULL:
			mset(m, dst, aux ? mk(m, a) : !mk(m, a), true);
			break;
		case SIFT_VK_OP_IN_SET:
			{
				bool		hit = false;

				if (fl & 1)
				{
					uint64_t	off = (uint64_t) (m->R[a] - imm);

					if (off < (uint64_t) o[7] * 32)
						hit = (job->pool[o[6] + off / 32] >> (off % 32)) & 1;
				}
				else
					for (uint32_t k = 0; k < o[7]; k++)
						hit = hit || (int64_t) (((uint64_t) job->pool[o[6] + 2 * k + 1] << 32) | job->pool[o[6] + 2 * k]) == m->R[a];
				mset(m, dst, hit, rv(m, a));
				break;
			}
		default:
			err(m, SIFT_VK_PE_UNSUP, i);
	}
}

/* the program over a row: false when it raised an error */
static bool
vm_run(VM *m, const SiftVkJob *job, const TGran *g, uint32_t row)
{
	memset(m, 0, sizeof(*m));
	m->MT = m->MK = 1;
	for (uint32_t i = 0; i < job->nops && !m->err; i++)
		vm_op(m, job, g, row, i);
	return m->err == 0;
}

static bool
row_dead(const TGran *g, uint32_t row)
{
	return g->has_dead && ((g->dead[row >> 5] >> (row & 31)) & 1);
}

/* ---- the groups ---- */
static uint64_t
tuple_hash(const RGroup *k, uint32_t nk)
{
	uint64_t	h = 0x9E3779B97F4A7C15ull;

	for (uint32_t j = 0; j < nk; j++)
	{
		h ^= (uint64_t) k->key[j] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
		h *= 0xff51afd7ed558ccdull;
	}
	h ^= k->nullmask;
	return h * 0xc4ceb9fe1a85ec53ull;
}

typedef struct GMap
{
	uint32_t   *slot;
	uint32_t	mask;
} GMap;

static RGroup *
find_group(RefResult *r, GMap *m, const SiftVkJob *job, const RGroup *k)
{
	uint32_t	i = (uint32_t) tuple_hash(k, job->nkeys) & m->mask;

	for (;; i = (i + 1) & m->mask)
	{
		if (m->slot[i] == 0)
		{
			RGroup	   *g;

			if (r->n == r->cap)
			{
				r->cap = r->cap ? r->cap * 2 : 1024;
				r->g = realloc(r->g, (size_t) r->cap * sizeof(RGroup));
			}
			g = &r->g[r->n];
			memset(g, 0, sizeof(*g));
			memcpy(g->key, k->key, sizeof(g->key));
			g->nullmask = k->nullmask;
			for (uint32_t a = 0; a < job->naggs; a++)
				g->a[a].mm = (job->agg[a].kind == SIFT_VK_AK_MIN || job->agg[a].kind == SIFT_VK_AK_MIN_F) ? INT64_MAX : INT64_MIN;
			m->slot[i] = ++r->n;
			return g;
		}
		RGroup	   *g = &r->g[m->slot[i] - 1];

		if (g->nullmask == k->nullmask && memcmp(g->key, k->key, sizeof(int64_t) * job->nkeys) == 0)
			return g;
	}
}

static void
agg_row(const VM *m, const SiftVkJob *job, RGroup *g)
{
	g->rows++;
	for (uint32_t a = 0; a < job->naggs; a++)
	{
		const SiftVkAgg *ag = &job->agg[a];
		RAgg	   *x = &g->a[a];

		if (ag->freg != 255 && !mt(m, ag->freg))
			continue;
		if (ag->kind == SIFT_VK_AK_COUNT_STAR)
		{
			x->cnt++;
			continue;
		}
		if (ag->vreg != 255 && !rv(m, ag->vreg))
			continue;
		if (ag->kind == SIFT_VK_AK_COUNT)
			x->cnt++;
		else if (ag->kind == SIFT_VK_AK_SUM_I)
		{
			x->sum += m->R[ag->vreg];
			x->cnt++;
		}
		else if (ag->kind == SIFT_VK_AK_SUM_F)
		{
			x->fsum += dbl(m->R[ag->vreg]);
			x->cnt++;
		}
		else
		{
			bool		fl = ag->kind == SIFT_VK_AK_MIN_F || ag->kind == SIFT_VK_AK_MAX_F,
						mn = ag->kind == SIFT_VK_AK_MIN || ag->kind == SIFT_VK_AK_MIN_F;
			int64_t		v = fl ? f8_key(dbl(m->R[ag->vreg])) : m->R[ag->vreg];

			x->mm = mn ? (v < x->mm ? v : x->mm) : (v > x->mm ? v : x->mm);
			x->cnt++;
		}
	}
}

bool
ref_run(const TData *d, const SiftVkJob *job, RefResult *out)
{
	GMap		gm = {0};
	VM			m;
	uint64_t	slots = 1 << 13;

	memset(out, 0, sizeof(*out));
	for (uint32_t gi = 0; gi < d->ngran; gi++)
		slots += (uint64_t) d->g[gi].nrows * 2;
	gm.mask = 1;
	while (gm.mask < slots)
		gm.mask = gm.mask * 2 + 1;
	gm.slot = calloc((size_t) gm.mask + 1, 4);
	if (job->mode == SIFT_VK_UNGROUPED || job->nkeys == 0)
	{
		RGroup		k = {0};

		find_group(out, &gm, job, &k);
	}
	for (uint32_t gi = 0; gi < d->ngran; gi++)
	{
		const TGran *g = &d->g[gi];

		for (uint32_t r = 0; r < g->nrows; r++)
		{
			RGroup		k = {0};
			RGroup	   *grp;
			bool		bad = false;

			if (row_dead(g, r))
				continue;
			if (!vm_run(&m, job, g, r))
			{
				out->has_err = true;
				out->err_gran = g->id;
				out->err_row = r;
				out->err_op = m.err_op;
				out->err_code = m.err;
				free(gm.slot);
				return true;
			}
			if (job->where_slot != 255 && !mt(&m, job->where_slot))
				continue;
			out->rows_in++;
			for (uint32_t j = 0; j < job->nkeys; j++)
			{
				bool		ok = rv(&m, job->key_reg[j]);

				k.key[j] = ok ? m.R[job->key_reg[j]] : 0;
				k.nullmask |= ok ? 0 : 1u << j;
				if (job->mode == SIFT_VK_DENSE && ok && (uint64_t) (k.key[j] - job->dense_kmin[j]) >= (uint64_t) (job->dense_span[j] - 1))
					bad = true;
			}
			if (bad)
			{
				out->has_err = true;
				out->err_gran = g->id;
				out->err_row = r;
				out->err_op = 0;
				out->err_code = SIFT_VK_PE_RANGE;
				free(gm.slot);
				return true;
			}
			if (job->mode == SIFT_VK_TOPN)
				continue;
			grp = find_group(out, &gm, job, &k);
			agg_row(&m, job, grp);
		}
	}
	free(gm.slot);
	return true;
}

void
ref_free(RefResult *r)
{
	free(r->g);
	memset(r, 0, sizeof(*r));
}

/* ---- the comparison ---- */
static bool
near(double a, double b, double tol)
{
	if (isnan(a) || isnan(b))
		return isnan(a) && isnan(b);
	if (isinf(a) || isinf(b))
		return a == b;
	return fabs(a - b) <= tol * fmax(1.0, fmax(fabs(a), fabs(b)));
}

static uint64_t
cmp_group(const SiftVkJob *job, const RGroup *g, const int64_t *w, const char *what, double ftol, uint64_t *shown)
{
	uint64_t	bad = 0;

	if (w[0] != g->rows)
	{
		if ((*shown)++ < 5)
			fprintf(stderr, "  %s: group rows: device %lld, oracle %lld\n", what, (long long) w[0], (long long) g->rows);
		bad++;
	}
	for (uint32_t a = 0; a < job->naggs; a++)
	{
		const int64_t *x = w + 1 + 3 * a;
		const RAgg *r = &g->a[a];
		uint8_t		k = job->agg[a].kind;
		bool		ok = true;

		if (k == SIFT_VK_AK_COUNT_STAR || k == SIFT_VK_AK_COUNT)
			ok = x[0] == r->cnt;
		else if (k == SIFT_VK_AK_SUM_I)
		{
			i128		s = (i128) x[1] * ((i128) 1 << 32) + (i128) (uint64_t) x[0];

			ok = s == r->sum && x[2] == r->cnt;
		}
		else if (k == SIFT_VK_AK_SUM_F)
		{
			double		dv;

			memcpy(&dv, &x[0], 8);
			ok = x[2] == r->cnt && near(dv, r->fsum, ftol);
		}
		else
			ok = x[2] == r->cnt && (r->cnt == 0 || x[0] == r->mm);
		if (!ok)
		{
			if ((*shown)++ < 5)
				fprintf(stderr, "  %s: aggregate %u (kind %u): device %lld %lld %lld, oracle cnt %lld mm %lld sum %lld\n", what, a, k, (long long) x[0],
						(long long) x[1], (long long) x[2], (long long) r->cnt, (long long) r->mm, (long long) r->sum);
			bad++;
		}
	}
	return bad;
}

uint64_t
ref_compare(const SiftVkJob *job, const SiftVkResult *res, const RefResult *ref, const char *what, double ftol)
{
	uint64_t	bad = 0,
				shown = 0;
	uint32_t	S = 1 + 3 * job->naggs;

	if (job->mode == SIFT_VK_UNGROUPED)
		return cmp_group(job, &ref->g[0], res->acc, what, ftol, &shown);
	if (job->mode == SIFT_VK_DENSE)
	{
		uint32_t	G = res->ngroups;
		uint8_t    *seen = calloc(G, 1);

		for (uint32_t i = 0; i < ref->n; i++)
		{
			const RGroup *g = &ref->g[i];
			uint64_t	gid = 0;

			for (uint32_t j = 0; j < job->nkeys; j++)
				gid = gid * job->dense_span[j] + ((g->nullmask >> j) & 1 ? job->dense_span[j] - 1 : (uint64_t) (g->key[j] - job->dense_kmin[j]));
			seen[gid] = 1;
			bad += cmp_group(job, g, res->acc + gid * S, what, ftol, &shown);
		}
		for (uint32_t gid = 0; gid < G; gid++)
			if (!seen[gid] && res->acc[(size_t) gid * S] != 0)
			{
				bad++;
				if (shown++ < 5)
					fprintf(stderr, "  %s: group %u has %lld rows on the device and none in the oracle\n", what, gid, (long long) res->acc[(size_t) gid * S]);
			}
		free(seen);
		return bad;
	}
	{
		uint32_t	nk = job->nkeys,
					rec = res->rec_words;
		uint32_t	mask = 1;
		uint32_t   *slot;

		if (res->ngroups != ref->n)
		{
			fprintf(stderr, "  %s: %u groups on the device, %u in the oracle\n", what, res->ngroups, ref->n);
			bad++;
		}
		while (mask < ref->n * 2 + 16)
			mask = mask * 2 + 1;
		slot = calloc((size_t) mask + 1, 4);
		for (uint32_t i = 0; i < ref->n; i++)
		{
			uint32_t	h = (uint32_t) tuple_hash(&ref->g[i], nk) & mask;

			while (slot[h])
				h = (h + 1) & mask;
			slot[h] = i + 1;
		}
		for (uint32_t i = 0; i < res->ngroups; i++)
		{
			const int64_t *w = res->acc + (size_t) i * rec;
			RGroup		k = {0};
			uint32_t	h;
			const RGroup *g = NULL;

			memcpy(k.key, w, nk * 8);
			k.nullmask = (uint32_t) w[nk];
			h = (uint32_t) tuple_hash(&k, nk) & mask;
			for (; slot[h]; h = (h + 1) & mask)
			{
				const RGroup *c = &ref->g[slot[h] - 1];

				if (c->nullmask == k.nullmask && memcmp(c->key, k.key, nk * 8) == 0)
				{
					g = c;
					break;
				}
			}
			if (!g)
			{
				bad++;
				if (shown++ < 5)
					fprintf(stderr, "  %s: a group of the device (key %lld) is not in the oracle\n", what, (long long) k.key[0]);
				continue;
			}
			bad += cmp_group(job, g, w + nk + 1, what, ftol, &shown);
		}
		free(slot);
	}
	return bad;
}

/* ---- the top N ---- */
static bool g_desc, g_nulls_first;

static int
rc_cmp(const void *pa, const void *pb)
{
	const RefCand *a = pa,
			   *b = pb;

	if (a->isnull != b->isnull)
		return (a->isnull == g_nulls_first) ? -1 : 1;
	if (!a->isnull && a->key != b->key)
		return (a->key > b->key) == g_desc ? -1 : 1;
	return 0;
}

uint32_t
ref_topn(const TData *d, const SiftVkJob *job, RefCand *out, uint32_t cap)
{
	uint32_t	n = 0,
				alloc = 1 << 16;
	RefCand    *all = malloc(alloc * sizeof(RefCand));
	VM			m;

	for (uint32_t gi = 0; gi < d->ngran; gi++)
		for (uint32_t r = 0; r < d->g[gi].nrows; r++)
		{
			bool		isnull;

			if (row_dead(&d->g[gi], r) || !vm_run(&m, job, &d->g[gi], r) || (job->where_slot != 255 && !mt(&m, job->where_slot)))
				continue;
			isnull = !rv(&m, job->sort_reg);
			if (isnull && !job->topn_nulls)
				continue;
			if (n == alloc)
				all = realloc(all, (alloc *= 2) * sizeof(RefCand));
			all[n++] = (RefCand) {.key = isnull ? 0 : (job->sort_float ? f8_key(dbl(m.R[job->sort_reg])) : m.R[job->sort_reg]),
				.gran = d->g[gi].id,.row = r,.isnull = isnull};
		}
	g_desc = job->topn_desc;
	g_nulls_first = job->topn_nulls;
	qsort(all, n, sizeof(RefCand), rc_cmp);
	if (n > cap)
		n = cap;
	memcpy(out, all, n * sizeof(RefCand));
	free(all);
	return n;
}

bool
ref_row_key(const TData *d, const SiftVkJob *job, uint32_t gran, uint32_t row, int64_t *key, bool *isnull)
{
	VM			m;

	if (gran >= d->ngran || !vm_run(&m, job, &d->g[gran], row))
		return false;
	*isnull = !rv(&m, job->sort_reg);
	*key = *isnull ? 0 : (job->sort_float ? f8_key(dbl(m.R[job->sort_reg])) : m.R[job->sort_reg]);
	return true;
}
