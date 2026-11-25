/*
 * agg/vexpr/vexpr.c - the machine of agg/vexpr/vexpr.h: the register file, and one function per operation over the rows of a batch. The operations of arithmetic run
 * on every row (the wrapping builtins give the answer and the overflow bit of each row) and fail only for a live valid row, so a dead row never fails
 * the batch; the comparisons are the slots of agg/kern6/kern6.h of the active level, the bitmaps of the boolean operations are whole words. The errors are
 * PostgreSQL's: the same messages, and for float8 the checks of utils/float.h. An operation does not raise: it notes the first row of the batch that has an
 * error (and of what kind), the later operations go on, and the batch raises the error of the lowest row, of the earliest operation for that row, once the
 * program is done: PostgreSQL meets the rows one after the other, so the error of a row that precedes another's is the one it raises, whichever operation of
 * this program found its own first.
 */
#include "postgres.h"

#include "utils/float.h"

#include "agg/kern6/int.h"
#include "agg/vexpr/vexpr.h"
#include "agg/vexpr/cal.h"
#include "agg/vexpr/kern.h"
#include "core/fmv.h"

typedef struct VMask
{
	uint64		v[SIFT_VEC_WORDS];	/* the rows that are true (a subset of k) */
	uint64		k[SIFT_VEC_WORDS];	/* the rows that are known */
} VMask;

struct SiftVexprCtx
{
	SiftVec		regs[SIFT_VEXPR_MAX_REGS];
	bool		own[SIFT_VEXPR_MAX_REGS];
	uint64		valid[SIFT_VEXPR_MAX_REGS][SIFT_VEC_WORDS] __attribute__((aligned(64)));
	VMask		m[SIFT_VEXPR_MAX_MASKS] __attribute__((aligned(64)));
	int32		civ[VCAL_SCRATCH_INTS] __attribute__((aligned(64)));
	uint8	   *bufs;
	uint8	   *scratch;		/* one register's bytes: a register widened in place is read from here */
	size_t		reg_bytes;
	MemoryContext cxt;
};

#define REG_BYTES_NARROW	(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD)
#define REG_BYTES_WIDE		(SIFT_VEC_LEN * 16 + SIFT_VEC_PAD)

typedef __int128 i128;

static void
ctx_alloc_bufs(SiftVexprCtx *c, size_t reg_bytes)
{
	c->reg_bytes = reg_bytes;
	c->bufs = MemoryContextAllocAligned(c->cxt, reg_bytes * SIFT_VEXPR_MAX_REGS, 64, MCXT_ALLOC_ZERO);
	c->scratch = MemoryContextAllocAligned(c->cxt, reg_bytes, 64, MCXT_ALLOC_ZERO);
}

SiftVexprCtx *
sift_vexpr_ctx_new(MemoryContext cxt)
{
	SiftVexprCtx *c = MemoryContextAllocAligned(cxt, sizeof(SiftVexprCtx), 64, MCXT_ALLOC_ZERO);

	c->cxt = cxt;
	ctx_alloc_bufs(c, REG_BYTES_NARROW);
	return c;
}

void
sift_vexpr_ctx_free(SiftVexprCtx *c)
{
	pfree(c->bufs);
	pfree(c->scratch);
	pfree(c);
}


static void
raise_fail(const Fail *f)
{
	switch ((ErrKind) f->kind)
	{
		case E_INT4:
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("integer out of range")));
			break;
		case E_INT8:
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			break;
		case E_DIV0:
			ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
			break;
		case E_FOVF:
			float_overflow_error();
		case E_FUNF:
			float_underflow_error();
		case E_FDIV0:
			float_zero_divide_error();
		case E_TSRANGE:
			ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE), errmsg("timestamp out of range")));
			break;
		case E_DATERANGE:
			ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE), errmsg("date out of range for timestamp")));
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("EXTRACT of an infinite value is not run by the vectorized expressions"),
							errhint("The scan keeps a chunk whose zone holds an infinity for the executor.")));
	}
}

typedef struct Run
{
	SiftVexprCtx *c;
	const SiftVexprIn *in;
	uint16		n;
	int			nw;
	uint64		tail;
	Fail		fail;
} Run;

static inline uint64
ones(const Run *r, int w)
{
	return w == r->nw - 1 ? r->tail : ~UINT64CONST(0);
}

static inline uint64
vword(const Run *r, const SiftVec *v, int w)
{
	return v->all_valid ? ones(r, w) : v->valid[w] & ones(r, w);
}

static inline size_t
esize(SiftVecType t)
{
	return t == SVEC_I32 ? 4 : t == SVEC_NUM128 ? 16 : 8;
}

static inline void *
buf_of(const Run *r, int reg)
{
	return r->c->bufs + (size_t) reg * r->c->reg_bytes;
}

/* The register as one this run writes: its own buffer and validity, the other fields of a dense vector of the batch */
static SiftVec *
own_reg(Run *r, int reg, SiftVecType type, int scale)
{
	SiftVec    *d = &r->c->regs[reg];

	d->data = buf_of(r, reg);
	d->valid = r->c->valid[reg];
	d->sel = NULL;
	d->len = r->n;
	d->all_valid = false;
	d->dense = true;
	d->type = type;
	d->scale = (int16) scale;
	d->dict = NULL;
	r->c->own[reg] = true;
	return d;
}

SIFT_FMV_DEFINE_VOID(static, vx_neg_f64, (double *d, const double *a, uint16 n), (d, a, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = -a[i];
})

SIFT_FMV_DEFINE_VOID(static, vx_abs_f64, (double *d, const double *a, uint16 n), (d, a, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = fabs(a[i]);
})

static inline VMask *
slot(Run *r, int i)
{
	return &r->c->m[i];
}

/* NUM128: the loops are of 128-bit integers, which the compiler leaves scalar; the bound the compiler proved keeps every result in 128 bits */
static void
arith128(i128 *d, const i128 *a, const i128 *b, i128 bc, bool bk, SiftVecOpCode op, uint16 n)
{
	for (int i = 0; i < n; i++)
	{
		const i128	x = a[i],
					y = bk ? bc : b[i];

		switch (op)
		{
			case VOP_ADD: d[i] = (i128) ((unsigned __int128) x + (unsigned __int128) y); break;
			case VOP_SUB: d[i] = (i128) ((unsigned __int128) x - (unsigned __int128) y); break;
			case VOP_MUL: d[i] = (i128) ((unsigned __int128) x * (unsigned __int128) y); break;
			case VOP_NEG: d[i] = (i128) ((unsigned __int128) 0 - (unsigned __int128) x); break;
			default: d[i] = x < 0 ? (i128) ((unsigned __int128) 0 - (unsigned __int128) x) : x;
		}
	}
}

static void
op_arith(Run *r, const SiftVecOp *op)
{
	const SiftVec a = r->c->regs[op->a];
	const bool	bk = (op->flags & VOP_FLAG_CONST_B) != 0;
	const SiftVec b = bk ? a : r->c->regs[op->b];
	const bool	unary = op->op == VOP_NEG || op->op == VOP_ABS;
	const bool	all_valid = a.all_valid && (bk || unary || b.all_valid);
	const uint64 *act = slot(r, op->act)->v;
	uint64		chk[SIFT_VEC_WORDS];
	Fail	   *f = &r->fail;
	SiftVec    *d;

	d = own_reg(r, op->dst, op->type, op->scale);
	for (int w = 0; w < r->nw; w++)
	{
		const uint64 res = vword(r, &a, w) & (bk || unary ? ~UINT64CONST(0) : vword(r, &b, w));

		if (!all_valid)
			d->valid[w] = res;
		chk[w] = act[w] & res;
	}
	d->all_valid = all_valid;
	if (all_valid)
		d->valid = NULL;
	switch (op->type)
	{
		case SVEC_I32:
			{
				int32	   *o = d->data;
				const int32 *x = a.data,
						   *y = b.data;
				const int32 c = (int32) op->imm_i64;

				switch (op->op)
				{
					case VOP_ADD: vx_add_i32(o, x, y, c, bk, chk, r->n, f); break;
					case VOP_SUB: vx_sub_i32(o, x, y, c, bk, chk, r->n, f); break;
					case VOP_MUL: vx_mul_i32(o, x, y, c, bk, chk, r->n, f); break;
					case VOP_DIV: vx_div_i32(o, x, y, c, bk, chk, r->n, f); break;
					case VOP_NEG: vx_neg_i32(o, x, chk, r->n, f); break;
					default: vx_abs_i32(o, x, chk, r->n, f); break;
				}
				break;
			}
		case SVEC_I64:
		case SVEC_NUM64:
			{
				int64	   *o = d->data;
				const int64 *x = a.data,
						   *y = b.data;

				switch (op->op)
				{
					case VOP_ADD: vx_add_i64(o, x, y, op->imm_i64, bk, chk, r->n, f); break;
					case VOP_SUB: vx_sub_i64(o, x, y, op->imm_i64, bk, chk, r->n, f); break;
					case VOP_MUL: vx_mul_i64(o, x, y, op->imm_i64, bk, chk, r->n, f); break;
					case VOP_DIV: vx_div_i64(o, x, y, op->imm_i64, bk, chk, r->n, f); break;
					case VOP_NEG: vx_neg_i64(o, x, chk, r->n, f); break;
					default: vx_abs_i64(o, x, chk, r->n, f); break;
				}
				break;
			}
		case SVEC_NUM128:
			arith128(d->data, a.data, b.data, (i128) op->imm_i64, bk, op->op, r->n);
			break;
		default:
			{
				double	   *o = d->data;
				const double *x = a.data,
						   *y = b.data;

				switch (op->op)
				{
					case VOP_ADD: vx_add_f64(o, x, y, op->imm_f64, bk, chk, r->n, f); break;
					case VOP_SUB: vx_sub_f64(o, x, y, op->imm_f64, bk, chk, r->n, f); break;
					case VOP_MUL: vx_mul_f64(o, x, y, op->imm_f64, bk, chk, r->n, f); break;
					case VOP_DIV: vx_div_f64(o, x, y, op->imm_f64, bk, chk, r->n, f); break;
					case VOP_NEG: vx_neg_f64(o, x, r->n); break;
					default: vx_abs_f64(o, x, r->n); break;
				}
			}
	}
}

SIFT_FMV_DEFINE_VOID(static, cast_i32_i64, (int64 *d, const int32 *a, uint16 n), (d, a, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = a[i];
})

SIFT_FMV_DEFINE_VOID(static, cast_i32_f64, (double *d, const int32 *a, uint16 n), (d, a, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = a[i];
})

SIFT_FMV_DEFINE_VOID(static, cast_i64_f64, (double *d, const int64 *a, uint16 n), (d, a, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = (double) a[i];
})

SIFT_FMV_DEFINE_VOID(static, cast_num64_f64, (double *d, const int64 *a, uint16 n, double div), (d, a, n, div),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = (double) a[i] / div;
})

SIFT_FMV_DEFINE_VOID(static, rescale_i64, (int64 *d, const int64 *a, uint16 n, int64 mul), (d, a, n, mul),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = (int64) ((uint64) a[i] * (uint64) mul);
})

static void
cast_i64_num128(i128 *d, const int64 *a, uint16 n)
{
	for (int i = 0; i < n; i++)
		d[i] = a[i];
}

static void
cast_i32_num128(i128 *d, const int32 *a, uint16 n)
{
	for (int i = 0; i < n; i++)
		d[i] = a[i];
}

static void
cast_num128_f64(double *d, const i128 *a, uint16 n, double div)
{
	for (int i = 0; i < n; i++)
		d[i] = (double) a[i] / div;
}

static double
pow10_f64(int scale)
{
	double		p = 1.0;

	for (int i = 0; i < scale; i++)
		p *= 10.0;
	return p;
}

static i128
pow10_i128(int shift)
{
	i128		p = 1;

	for (int i = 0; i < shift; i++)
		p *= 10;
	return p;
}

/* The source of an operation that writes a wider element into the buffer of its source: the source is read from a copy */
static const void *
source_of(Run *r, const SiftVec *a, const SiftVec *d, size_t from_size, size_t to_size)
{
	if (a->data != d->data || to_size <= from_size)
		return a->data;
	memcpy(r->c->scratch, a->data, (size_t) r->n * from_size);
	return r->c->scratch;
}

static void
op_cast(Run *r, const SiftVecOp *op)
{
	const SiftVec a = r->c->regs[op->a];
	SiftVec    *d = own_reg(r, op->dst, op->type_to, op->scale);
	const void *src;

	for (int w = 0; w < r->nw; w++)
		d->valid[w] = vword(r, &a, w);
	d->all_valid = a.all_valid;
	if (a.all_valid)
		d->valid = NULL;
	src = source_of(r, &a, d, esize(op->type), esize(op->type_to));
	if (op->type == SVEC_I32 && op->type_to == SVEC_I64)
		cast_i32_i64(d->data, src, r->n);
	else if (op->type == SVEC_I32 && op->type_to == SVEC_F64)
		cast_i32_f64(d->data, src, r->n);
	else if (op->type == SVEC_I64 && op->type_to == SVEC_F64)
		cast_i64_f64(d->data, src, r->n);
	else if (op->type == SVEC_I32 && op->type_to == SVEC_NUM64)
		cast_i32_i64(d->data, src, r->n);
	else if (op->type == SVEC_I32 && op->type_to == SVEC_NUM128)
		cast_i32_num128(d->data, src, r->n);
	else if (op->type == SVEC_I64 && op->type_to == SVEC_NUM128)
		cast_i64_num128(d->data, src, r->n);
	else if (op->type == SVEC_NUM64 && op->type_to == SVEC_NUM128)
		cast_i64_num128(d->data, src, r->n);
	else if (op->type == SVEC_NUM64 && op->type_to == SVEC_F64)
		cast_num64_f64(d->data, src, r->n, pow10_f64(op->aux));
	else
		cast_num128_f64(d->data, src, r->n, pow10_f64(op->aux));
}

static void
op_rescale(Run *r, const SiftVecOp *op)
{
	const SiftVec a = r->c->regs[op->a];
	SiftVec    *d = own_reg(r, op->dst, op->type, op->scale);

	for (int w = 0; w < r->nw; w++)
		d->valid[w] = vword(r, &a, w);
	d->all_valid = a.all_valid;
	if (a.all_valid)
		d->valid = NULL;
	if (op->type == SVEC_NUM64)
		rescale_i64(d->data, a.data, r->n, (int64) pow10_i128(op->aux));
	else
	{
		const i128	mul = pow10_i128(op->aux);
		const i128 *x = a.data;
		i128	   *o = d->data;

		for (int i = 0; i < r->n; i++)
			o[i] = (i128) ((unsigned __int128) x[i] * (unsigned __int128) mul);
	}
}

static void
cmp_i128(uint64 *m, const i128 *a, const i128 *b, i128 bc, bool bk, uint16 n, SiftCmpOp cmp)
{
	memset(m, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (int i = 0; i < n; i++)
	{
		const i128	x = a[i],
					y = bk ? bc : b[i];
		bool		t;

		switch (cmp)
		{
			case SCMP_EQ: t = x == y; break;
			case SCMP_NE: t = x != y; break;
			case SCMP_LT: t = x < y; break;
			case SCMP_LE: t = x <= y; break;
			case SCMP_GT: t = x > y; break;
			default: t = x >= y;
		}
		m[i >> 6] |= (uint64) t << (i & 63);
	}
}

static void
op_cmp(Run *r, const SiftVecOp *op)
{
	const SiftVec a = r->c->regs[op->a];
	const bool	bk = (op->flags & VOP_FLAG_CONST_B) != 0;
	const SiftVec b = bk ? a : r->c->regs[op->b];
	VMask	   *m = slot(r, op->dst);
	const SiftAggKernelsV6 *k = sift_aggk6;
	const SiftCmpOp cmp = (SiftCmpOp) op->aux;

	switch (op->type)
	{
		case SVEC_I32:
			if (bk)
				k->cmp_i32(m->v, a.data, r->n, cmp, (int32) op->imm_i64);
			else
				k->cmp2_i32(m->v, a.data, b.data, r->n, cmp);
			break;
		case SVEC_I64:
		case SVEC_NUM64:
			if (bk)
				k->cmp_i64(m->v, a.data, r->n, cmp, op->imm_i64);
			else
				k->cmp2_i64(m->v, a.data, b.data, r->n, cmp);
			break;
		case SVEC_NUM128:
			cmp_i128(m->v, a.data, b.data, (i128) op->imm_i64, bk, r->n, cmp);
			break;
		default:
			if (bk)
				k->cmp_f64(m->v, a.data, r->n, cmp, op->imm_f64);
			else
				k->cmp2_f64(m->v, a.data, b.data, r->n, cmp);
	}
	for (int w = 0; w < r->nw; w++)
	{
		const uint64 known = vword(r, &a, w) & (bk ? ~UINT64CONST(0) : vword(r, &b, w));

		m->k[w] = known;
		m->v[w] &= known;
	}
}

static void
op_mask(Run *r, const SiftVecOp *op)
{
	VMask	   *d = slot(r, op->dst);
	const VMask a = *slot(r, op->a);
	const VMask b = op->op == VOP_MASK_NOT ? a : *slot(r, op->b);

	for (int w = 0; w < r->nw; w++)
	{
		if (op->op == VOP_MASK_AND)
		{
			d->v[w] = a.v[w] & b.v[w];
			d->k[w] = d->v[w] | (a.k[w] & ~a.v[w]) | (b.k[w] & ~b.v[w]);
		}
		else if (op->op == VOP_MASK_OR)
		{
			d->v[w] = a.v[w] | b.v[w];
			d->k[w] = d->v[w] | (a.k[w] & b.k[w]);
		}
		else
		{
			d->v[w] = a.k[w] & ~a.v[w];
			d->k[w] = a.k[w];
		}
	}
}

/* the rows of slot c take the true and the known bits of slot a: the branch of a boolean CASE or COALESCE that PostgreSQL ran for them */
static void
op_mask_blend(Run *r, const SiftVecOp *op)
{
	VMask	   *d = slot(r, op->dst);
	const VMask a = *slot(r, op->a);
	const uint64 *m = slot(r, op->c)->v;

	for (int w = 0; w < r->nw; w++)
	{
		d->v[w] = (d->v[w] & ~m[w]) | (a.v[w] & m[w]);
		d->k[w] = (d->k[w] & ~m[w]) | (a.k[w] & m[w]);
	}
}

static void
op_mask_unknown(Run *r, const SiftVecOp *op)
{
	VMask	   *d = slot(r, op->dst);

	for (int w = 0; w < r->nw; w++)
		d->v[w] = d->k[w] = 0;
}

static void
op_mask_isnull(Run *r, const SiftVecOp *op)
{
	VMask	   *d = slot(r, op->dst);
	const VMask a = *slot(r, op->a);

	for (int w = 0; w < r->nw; w++)
	{
		d->v[w] = (op->aux ? a.k[w] : ~a.k[w]) & ones(r, w);
		d->k[w] = ones(r, w);
	}
}

static void
op_load_mask(Run *r, const SiftVecOp *op)
{
	VMask	   *d = slot(r, op->dst);

	if (op->op == VOP_MASK_CONST)
		for (int w = 0; w < r->nw; w++)
		{
			d->v[w] = op->aux ? ones(r, w) : 0;
			d->k[w] = ones(r, w);
		}
	else if (op->op == VOP_MASK_IN)
	{
		const SiftVexprMaskIn *in = &r->in->masks[op->aux];

		for (int w = 0; w < r->nw; w++)
		{
			const uint64 known = (in->valid ? in->valid[w] : ~UINT64CONST(0)) & ones(r, w);

			d->k[w] = known;
			d->v[w] = in->bits[w] & known;
		}
	}
	else
		for (int w = 0; w < r->nw; w++)
		{
			const uint64 valid = vword(r, &r->c->regs[op->a], w);

			d->v[w] = op->aux ? valid : ones(r, w) & ~valid;
			d->k[w] = ones(r, w);
		}
}

static void
op_load(Run *r, const SiftVecOp *op)
{
	SiftVec    *d;

	if (op->op == VOP_LOAD_COL)
	{
		r->c->regs[op->dst] = r->in->cols[op->aux];
		r->c->own[op->dst] = false;
		return;
	}
	d = own_reg(r, op->dst, op->type, op->scale);
	if (op->op == VOP_LOAD_CONST)
	{
		d->all_valid = true;
		d->valid = NULL;
		for (int i = 0; i < r->n; i++)
			if (op->type == SVEC_I32)
				((int32 *) d->data)[i] = (int32) op->imm_i64;
			else if (op->type == SVEC_I64 || op->type == SVEC_NUM64)
				((int64 *) d->data)[i] = op->imm_i64;
			else if (op->type == SVEC_NUM128)
				((i128 *) d->data)[i] = (i128) op->imm_i64;
			else
				((double *) d->data)[i] = op->imm_f64;
	}
	else
		memset(d->valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
}

static void
op_copy(Run *r, const SiftVecOp *op)
{
	const SiftVec a = r->c->regs[op->a];
	SiftVec    *d;

	if (op->dst == op->a && r->c->own[op->dst])
		return;
	d = own_reg(r, op->dst, op->type, op->scale);
	memcpy(d->data, a.data, r->n * esize(op->type));
	d->all_valid = a.all_valid;
	if (a.all_valid)
		d->valid = NULL;
	else
		for (int w = 0; w < r->nw; w++)
			d->valid[w] = a.valid[w];
}

/* The rows of the mask take the value of the source; the mask goes by words of 64 rows, so that the loop of a word has a fixed length, and the rows of the last word one by one */
#define DEF_BLEND(NAME, T) \
SIFT_FMV_DEFINE_VOID(static, NAME, (T *d, const T *s, const uint64 *m, uint16 n), (d, s, m, n), \
{ \
	const int	full = n >> 6; \
	for (int w = 0; w < full; w++) \
	{ \
		const uint64 bits = m[w]; \
		T		   *dw = d + w * 64; \
		const T    *sw = s + w * 64; \
		if (bits) \
		{ \
			SIFT_IVDEP \
			for (int j = 0; j < 64; j++) \
				dw[j] = (bits >> j & 1) ? sw[j] : dw[j]; \
		} \
	} \
	for (int i = full * 64; i < n; i++) \
		if (m[full] >> (i & 63) & 1) \
			d[i] = s[i]; \
})

DEF_BLEND(blend32, int32)
DEF_BLEND(blend64, int64)

static void
blend128(i128 *d, const i128 *s, const uint64 *m, uint16 n)
{
	for (int i = 0; i < n; i++)
		if (m[i >> 6] >> (i & 63) & 1)
			d[i] = s[i];
}

static void
op_blend(Run *r, const SiftVecOp *op)
{
	SiftVec    *d = &r->c->regs[op->dst];
	const SiftVec s = r->c->regs[op->a];
	const uint64 *m = slot(r, op->c)->v;

	if (d->all_valid)
	{
		d->valid = r->c->valid[op->dst];
		for (int w = 0; w < r->nw; w++)
			d->valid[w] = ones(r, w);
		d->all_valid = false;
	}
	for (int w = 0; w < r->nw; w++)
	{
		const uint64 sv = vword(r, &s, w);

		d->valid[w] = (d->valid[w] & ~m[w]) | (sv & m[w]);
	}
	if (op->type == SVEC_I32)
		blend32(d->data, s.data, m, r->n);
	else if (op->type == SVEC_NUM128)
		blend128(d->data, s.data, m, r->n);
	else
		blend64(d->data, s.data, m, r->n);
}

static void
op_act(Run *r, const SiftVecOp *op)
{
	VMask	   *d = slot(r, op->dst);
	const VMask a = *slot(r, op->a);
	const VMask b = op->op == VOP_ACT_NOTVALID || op->op == VOP_ACT_COPY ? a : *slot(r, op->b);

	for (int w = 0; w < r->nw; w++)
	{
		uint64		x;

		switch (op->op)
		{
			case VOP_ACT_COPY: x = a.v[w]; break;
			case VOP_ACT_AND: x = a.v[w] & b.v[w]; break;
			case VOP_ACT_ANDN: x = a.v[w] & ~b.v[w]; break;
			case VOP_ACT_NOTFALSE: x = a.v[w] & (~b.k[w] | b.v[w]); break;
			case VOP_ACT_UNKNOWN: x = a.v[w] & ~b.k[w]; break;
			default: x = a.v[w] & ~vword(r, &r->c->regs[op->b], w); break;
		}
		d->v[w] = x;
		d->k[w] = ones(r, w);
	}
}

static bool
slot_empty(Run *r, int i)
{
	const uint64 *v = slot(r, i)->v;

	for (int w = 0; w < r->nw; w++)
		if (v[w])
			return false;
	return true;
}

/* date_trunc and EXTRACT: the kernels of agg/vexpr/cal.h over the rows that count, and the first row of them that PostgreSQL would fail on */
static void
op_calendar(Run *r, const SiftVecOp *op)
{
	const SiftVec a = r->c->regs[op->a];
	const bool	trunc = op->op == VOP_DATE_TRUNC;
	const bool	is_date = op->type == SVEC_I32;
	SiftVec    *d = own_reg(r, op->dst, op->type_to, op->scale);
	const void *src = source_of(r, &a, d, esize(op->type), esize(op->type_to));
	const uint64 *act = slot(r, op->act)->v;
	uint64		chk[SIFT_VEC_WORDS],
				special[SIFT_VEC_WORDS];
	int			row;

	for (int w = 0; w < r->nw; w++)
	{
		d->valid[w] = vword(r, &a, w);
		chk[w] = act[w] & d->valid[w];
	}
	d->all_valid = false;
	if (trunc)
	{
		int64		ts[SIFT_VEC_LEN];

		if (is_date)
		{
			if ((row = vcal_date_to_ts(ts, src, r->n, op->zone, chk)) >= 0)
				note(&r->fail, row, E_DATERANGE);
			src = ts;
		}
		row = vcal_date_trunc(d->data, src, r->n, (SiftVxUnit) op->aux, op->zone, chk, r->c->civ);
		if (row >= 0)
			note(&r->fail, row, E_TSRANGE);
		return;
	}
	row = vcal_extract(d->data, src, is_date, r->n, (SiftVxField) op->aux, op->zone, chk, special, r->c->civ);
	if (row >= 0)
		note(&r->fail, row, E_SPECIAL);
	for (int w = 0; w < r->nw; w++)
		d->valid[w] &= ~special[w];
}

void
sift_vexpr_eval(SiftVexprCtx *c, const SiftVexprProg *prog, const SiftVexprIn *in, const uint64 *active, uint16 n, SiftVexprOut *out)
{
	Run			run = {c, in, n, AK6_NW(n), AK6_TAIL(n), {INT32_MAX, 0}};
	VMask	   *m0 = &c->m[0];

	Assert(n <= SIFT_VEC_LEN);
	if (prog->wide && c->reg_bytes < REG_BYTES_WIDE)
	{
		pfree(c->bufs);
		pfree(c->scratch);
		ctx_alloc_bufs(c, REG_BYTES_WIDE);
	}
	for (int w = 0; w < run.nw; w++)
	{
		m0->v[w] = active[w] & ones(&run, w);
		m0->k[w] = ones(&run, w);
	}
	for (uint16 pc = 0; pc < prog->n_ops; pc++)
	{
		const SiftVecOp *op = &prog->ops[pc];

		switch (op->op)
		{
			case VOP_LOAD_COL:
			case VOP_LOAD_CONST:
			case VOP_LOAD_NULL:
			case VOP_INIT_NULL:
				op_load(&run, op);
				break;
			case VOP_CAST:
				op_cast(&run, op);
				break;
			case VOP_RESCALE:
				op_rescale(&run, op);
				break;
			case VOP_ADD:
			case VOP_SUB:
			case VOP_MUL:
			case VOP_DIV:
			case VOP_NEG:
			case VOP_ABS:
				op_arith(&run, op);
				break;
			case VOP_DATE_TRUNC:
			case VOP_EXTRACT:
				op_calendar(&run, op);
				break;
			case VOP_CMP:
				op_cmp(&run, op);
				break;
			case VOP_IS_NULL:
			case VOP_MASK_IN:
			case VOP_MASK_CONST:
				op_load_mask(&run, op);
				break;
			case VOP_MASK_AND:
			case VOP_MASK_OR:
			case VOP_MASK_NOT:
				op_mask(&run, op);
				break;
			case VOP_MASK_BLEND:
				op_mask_blend(&run, op);
				break;
			case VOP_MASK_UNKNOWN:
				op_mask_unknown(&run, op);
				break;
			case VOP_MASK_ISNULL:
				op_mask_isnull(&run, op);
				break;
			case VOP_COPY:
				op_copy(&run, op);
				break;
			case VOP_BLEND:
				op_blend(&run, op);
				break;
			case VOP_SKIP_IF_EMPTY:
				if (slot_empty(&run, op->a))
					pc += op->aux;
				break;
			default:
				op_act(&run, op);
		}
	}
	if (run.fail.row != INT32_MAX)
		raise_fail(&run.fail);
	out->is_bool = prog->out_is_bool;
	out->vec = prog->out_is_bool ? NULL : &c->regs[prog->out];
	out->true_bits = prog->out_is_bool ? c->m[prog->out].v : NULL;
	out->known_bits = prog->out_is_bool ? c->m[prog->out].k : NULL;
}

uint16
sift_vexpr_filter(SiftVexprCtx *c, const SiftVexprProg *prog, const SiftVexprIn *in, uint64 *active, uint16 n)
{
	SiftVexprOut out;
	uint32		left = 0;

	Assert(prog->out_is_bool);
	sift_vexpr_eval(c, prog, in, active, n, &out);
	for (int w = 0; w < AK6_NW(n); w++)
	{
		active[w] &= out.true_bits[w];
		left += (uint32) sift_pc64(active[w]);
	}
	return (uint16) left;
}
