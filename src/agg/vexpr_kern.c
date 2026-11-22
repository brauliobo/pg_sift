/*
 * agg/vexpr_kern.c - the row loops of the integer and float8 operations of the machine (agg/vexpr.c): PostgreSQL's overflow and range checks over a batch, each
 * loop the same C for every instruction set (core/fmv.h). A loop does not raise: it notes the first row that has an error (agg/vexpr_kern.h).
 */
#include "postgres.h"

#include <float.h>

#include "utils/float.h"

#include "agg/kern6_int.h"
#include "agg/vexpr_kern.h"
#include "core/fmv.h"

/* the first row of the word with an error of the kind, among the rows that count */
#define CHECK_BAD(f, kind) \
	do { \
		if (bad_ & chk[w_]) \
			note(f, w_ * 64 + __builtin_ctzll(bad_ & chk[w_]), kind); \
	} while (0)

#define LOOP_ROWS(n, check, ...) \
	for (int w_ = 0; w_ < AK6_NW(n); w_++) \
	{ \
		uint64		bad_ = 0; \
		const int	hi_ = Min(64, (n) - w_ * 64); \
		for (int j_ = 0; j_ < hi_; j_++) \
		{ \
			const int i = w_ * 64 + j_; \
			__VA_ARGS__; \
		} \
		check; \
	}

/*
 * The operations of the vector kernels run in two passes, each a loop the compiler vectorizes (core/fmv.h): the first finds out whether any row overflows, the
 * second writes the rows. The _vec function writes nothing and gives false when a row overflows; then the exact function runs: it finds the rows that overflow
 * and raises for a live valid one, and writes the wrapped result of the others. The registers can be the operand of the same row (dst = a), so the loops carry
 * no dependence between rows (SIFT_IVDEP), which restrict could not say.
 */
#define DEF_INT_EXACT(LINKAGE, NAME, T, BUILTIN, ERR) \
LINKAGE void \
NAME(T *d, const T *a, const T *b, T bc, bool bk, const uint64 *chk, uint16 n, Fail *f) \
{ \
	LOOP_ROWS(n, CHECK_BAD(f, ERR), T r; bad_ |= (uint64) BUILTIN(a[i], bk ? bc : b[i], &r) << j_; d[i] = r); \
}

#define DEF_INT(NAME, T, BUILTIN, ERR, RES, OVF) \
DEF_INT_EXACT(static, NAME##_exact, T, BUILTIN, ERR) \
SIFT_FMV_DEFINE(static, bool, NAME##_vec, (T *d, const T *a, const T *b, T bc, bool bk, uint16 n), (d, a, b, bc, bk, n), \
{ \
	unsigned	bad = 0; \
	SIFT_IVDEP \
	for (int i = 0; i < n; i++) \
	{ \
		const T x = a[i], y = bk ? bc : b[i], r = RES; \
		bad |= OVF; \
	} \
	if (bad) \
		return false; \
	SIFT_IVDEP \
	for (int i = 0; i < n; i++) \
	{ \
		const T x = a[i], y = bk ? bc : b[i]; \
		d[i] = RES; \
	} \
	return true; \
}) \
void \
NAME(T *d, const T *a, const T *b, T bc, bool bk, const uint64 *chk, uint16 n, Fail *f) \
{ \
	if (!NAME##_vec(d, a, b, bc, bk, n)) \
		NAME##_exact(d, a, b, bc, bk, chk, n, f); \
}

#define ADD_RES(T, U)	((T) ((U) x + (U) y))
#define SUB_RES(T, U)	((T) ((U) x - (U) y))
#define ADD_OVF			((((x) ^ r) & ((y) ^ r)) < 0)
#define SUB_OVF			((((x) ^ (y)) & ((x) ^ r)) < 0)

DEF_INT(vx_add_i32, int32, __builtin_add_overflow, E_INT4, ADD_RES(int32, uint32), ADD_OVF)
DEF_INT(vx_sub_i32, int32, __builtin_sub_overflow, E_INT4, SUB_RES(int32, uint32), SUB_OVF)
DEF_INT(vx_mul_i32, int32, __builtin_mul_overflow, E_INT4, (int32) ((int64) x * y), (int64) x * y != r)
DEF_INT(vx_add_i64, int64, __builtin_add_overflow, E_INT8, ADD_RES(int64, uint64), ADD_OVF)
DEF_INT(vx_sub_i64, int64, __builtin_sub_overflow, E_INT8, SUB_RES(int64, uint64), SUB_OVF)
DEF_INT_EXACT(, vx_mul_i64, int64, __builtin_mul_overflow, E_INT8)

#define DEF_DIV(NAME, T, MINV, ERR) \
void \
NAME(T *d, const T *a, const T *b, T bc, bool bk, const uint64 *chk, uint16 n, Fail *f) \
{ \
	for (int w = 0; w < AK6_NW(n); w++) \
		for (uint64 m = chk[w]; m; m &= m - 1) \
		{ \
			const int i = w * 64 + __builtin_ctzll(m); \
			const T y = bk ? bc : b[i]; \
			if (y == 0) \
			{ \
				note(f, i, E_DIV0); \
				d[i] = 0; \
			} \
			else if (y == -1 && a[i] == MINV) \
			{ \
				note(f, i, ERR); \
				d[i] = 0; \
			} \
			else \
				d[i] = a[i] / y; \
		} \
}

DEF_DIV(vx_div_i32, int32, INT32_MIN, E_INT4)
DEF_DIV(vx_div_i64, int64, INT64_MIN, E_INT8)

#define DEF_UN_EXACT(NAME, T, MINV, ERR, EXPR) \
static void \
NAME(T *d, const T *a, const uint64 *chk, uint16 n, Fail *f) \
{ \
	LOOP_ROWS(n, CHECK_BAD(f, ERR), bad_ |= (uint64) (a[i] == MINV) << j_; d[i] = (T) (EXPR)); \
}

#define DEF_UN(NAME, T, MINV, ERR, EXPR) \
DEF_UN_EXACT(NAME##_exact, T, MINV, ERR, EXPR) \
SIFT_FMV_DEFINE(static, bool, NAME##_vec, (T *d, const T *a, uint16 n), (d, a, n), \
{ \
	unsigned	bad = 0; \
	SIFT_IVDEP \
	for (int i = 0; i < n; i++) \
		bad |= a[i] == MINV; \
	if (bad) \
		return false; \
	SIFT_IVDEP \
	for (int i = 0; i < n; i++) \
		d[i] = (T) (EXPR); \
	return true; \
}) \
void \
NAME(T *d, const T *a, const uint64 *chk, uint16 n, Fail *f) \
{ \
	if (!NAME##_vec(d, a, n)) \
		NAME##_exact(d, a, chk, n, f); \
}

DEF_UN(vx_neg_i32, int32, INT32_MIN, E_INT4, (uint32) 0 - (uint32) a[i])
DEF_UN(vx_neg_i64, int64, INT64_MIN, E_INT8, (uint64) 0 - (uint64) a[i])
DEF_UN(vx_abs_i32, int32, INT32_MIN, E_INT4, a[i] < 0 ? (uint32) 0 - (uint32) a[i] : (uint32) a[i])
DEF_UN(vx_abs_i64, int64, INT64_MIN, E_INT8, a[i] < 0 ? (uint64) 0 - (uint64) a[i] : (uint64) a[i])

/*
 * The float checks of PostgreSQL's float8 operators: a result that is infinite or zero while the operands are not, with & and | that the compiler runs on every lane. An
 * infinite result has no NaN operand, so the operands are not infinite when their magnitude is at most DBL_MAX.
 */
#define F64_OVF			((fabs(r) > DBL_MAX) & (fabs(x) <= DBL_MAX) & (fabs(y) <= DBL_MAX))
#define F64_UNF			((r == 0.0) & (x != 0.0) & (y != 0.0))

#define DEF_F64(NAME, OP, BAD, EXACT) \
SIFT_FMV_DEFINE(static, bool, NAME##_vec, (double *d, const double *a, const double *b, double bc, bool bk, uint16 n), (d, a, b, bc, bk, n), \
{ \
	unsigned	bad = 0; \
	SIFT_IVDEP \
	for (int i = 0; i < n; i++) \
	{ \
		const double x = a[i], y = bk ? bc : b[i], r = x OP y; \
		bad |= BAD; \
	} \
	if (bad) \
		return false; \
	SIFT_IVDEP \
	for (int i = 0; i < n; i++) \
		d[i] = a[i] OP (bk ? bc : b[i]); \
	return true; \
}) \
void \
NAME(double *d, const double *a, const double *b, double bc, bool bk, const uint64 *chk, uint16 n, Fail *f) \
{ \
	if (!NAME##_vec(d, a, b, bc, bk, n)) \
		EXACT(d, a, b, bc, bk, chk, n, f); \
}

static void
add_f64_exact(double *d, const double *a, const double *b, double bc, bool bk, const uint64 *chk, uint16 n, Fail *f)
{
	LOOP_ROWS(n, CHECK_BAD(f, E_FOVF),
			  const double x = a[i], y = bk ? bc : b[i], r = x + y;
			  bad_ |= (uint64) F64_OVF << j_;
			  d[i] = r);
}

static void
sub_f64_exact(double *d, const double *a, const double *b, double bc, bool bk, const uint64 *chk, uint16 n, Fail *f)
{
	LOOP_ROWS(n, CHECK_BAD(f, E_FOVF),
			  const double x = a[i], y = bk ? bc : b[i], r = x - y;
			  bad_ |= (uint64) F64_OVF << j_;
			  d[i] = r);
}

static void
mul_f64_exact(double *d, const double *a, const double *b, double bc, bool bk, const uint64 *chk, uint16 n, Fail *f)
{
	for (int w = 0; w < AK6_NW(n); w++)
	{
		uint64		over = 0,
					under = 0;
		const int	hi = Min(64, n - w * 64);

		for (int j = 0; j < hi; j++)
		{
			const int	i = w * 64 + j;
			const double x = a[i],
						y = bk ? bc : b[i],
						r = x * y;

			over |= (uint64) F64_OVF << j;
			under |= (uint64) F64_UNF << j;
			d[i] = r;
		}
		over &= chk[w];
		under &= chk[w];
		if (over | under)
		{
			const int	first = __builtin_ctzll(over | under);

			note(f, w * 64 + first, (over >> first & 1) ? E_FOVF : E_FUNF);
		}
	}
}

DEF_F64(vx_add_f64, +, F64_OVF, add_f64_exact)
DEF_F64(vx_sub_f64, -, F64_OVF, sub_f64_exact)
DEF_F64(vx_mul_f64, *, F64_OVF | F64_UNF, mul_f64_exact)

void
vx_div_f64(double *d, const double *a, const double *b, double bc, bool bk, const uint64 *chk, uint16 n, Fail *f)
{
	for (int w = 0; w < AK6_NW(n); w++)
		for (uint64 m = chk[w]; m; m &= m - 1)
		{
			const int	i = w * 64 + __builtin_ctzll(m);
			const double x = a[i],
						y = bk ? bc : b[i];
			double		r = 0;

			if (y == 0.0 && !isnan(x))
				note(f, i, E_FDIV0);
			else
			{
				r = x / y;
				if (isinf(r) && !isinf(x))
					note(f, i, E_FOVF);
				else if (r == 0.0 && x != 0.0 && !isinf(y))
					note(f, i, E_FUNF);
			}
			d[i] = r;
		}
}
