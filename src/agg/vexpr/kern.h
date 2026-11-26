/*
 * agg/vexpr/kern.h - the row loops of the expression machine's integer and float8 operations (agg/vexpr/kern.c). The destination is the operand of the same row
 * or another register; b is the register or, when bk, the constant bc. chk are the rows whose error counts: a loop notes the first of them that has one in f.
 */
#ifndef SIFT_AGG_VEXPR_KERN_H
#define SIFT_AGG_VEXPR_KERN_H

#include "agg/vexpr/vexpr.h"

#pragma GCC visibility push(hidden)

typedef enum ErrKind
{
	E_INT4,
	E_INT8,
	E_DIV0,
	E_FOVF,
	E_FUNF,
	E_FDIV0,
	E_TSRANGE,
	E_DATERANGE
} ErrKind;

/* the first row of the batch that has an error, and its kind; an operation that finds one in a lower row than the noted takes its place */
typedef struct Fail
{
	int32		row;
	uint8		kind;
} Fail;

static inline void
note(Fail *f, int row, ErrKind kind)
{
	if (row < f->row)
	{
		f->row = row;
		f->kind = (uint8) kind;
	}
}

#define VX_BINARY_PROTO(NAME, T) \
	extern void NAME(T *d, const T *a, const T *b, T bc, bool bk, const uint64 *chk, uint16 n, Fail *f)
#define VX_UNARY_PROTO(NAME, T) \
	extern void NAME(T *d, const T *a, const uint64 *chk, uint16 n, Fail *f)

VX_BINARY_PROTO(vx_add_i32, int32);
VX_BINARY_PROTO(vx_sub_i32, int32);
VX_BINARY_PROTO(vx_mul_i32, int32);
VX_BINARY_PROTO(vx_div_i32, int32);
VX_BINARY_PROTO(vx_add_i64, int64);
VX_BINARY_PROTO(vx_sub_i64, int64);
VX_BINARY_PROTO(vx_mul_i64, int64);
VX_BINARY_PROTO(vx_div_i64, int64);
VX_BINARY_PROTO(vx_add_f64, double);
VX_BINARY_PROTO(vx_sub_f64, double);
VX_BINARY_PROTO(vx_mul_f64, double);
VX_BINARY_PROTO(vx_div_f64, double);
VX_UNARY_PROTO(vx_neg_i32, int32);
VX_UNARY_PROTO(vx_abs_i32, int32);
VX_UNARY_PROTO(vx_neg_i64, int64);
VX_UNARY_PROTO(vx_abs_i64, int64);

#pragma GCC visibility pop

#endif							/* SIFT_AGG_VEXPR_KERN_H */
