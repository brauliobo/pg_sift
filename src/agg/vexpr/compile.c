/*
 * agg/vexpr/compile.c - an expression tree (agg/vexpr/vexpr.h) to the program of the machine. Registers and boolean slots are a stack: an operation takes its
 * operands from the top and leaves its result in the register of its first operand, so that a tree needs as many registers as it is deep. Slot 0 is
 * the rows of the batch. Every operation is given the slot of the rows that are live for it: the rows of the batch for the top of the tree, and below
 * AND, OR, COALESCE and CASE the rows that PostgreSQL goes on to evaluate there (agg/vexpr/vexpr.h); a group of operations whose live rows are none is
 * jumped over.
 *
 * The type of every node is worked out before its code is made (ti_of): an operator of two types takes them to the type PostgreSQL gives it, and the casts and
 * the rescaling of a numeric are emitted on the operand. A numeric is a scaled integer; the bound of its digits, which every operation grows by its own rule
 * (a sum adds the bounds, a product multiplies them), decides between 64 and 128 bits, so that no operation of the machine can overflow a numeric: where the
 * bound does not fit 128 bits the tree is not compiled.
 */
#include "postgres.h"

#include <math.h>

#include "agg/vexpr/vexpr.h"
#include "agg/vexpr/cal.h"

#define MAX_DEPTH		SIFT_VEXPR_MAX_OPS
#define LIM_NUM64		9.2e18
#define LIM_NUM128		1.6e38
#define LIM_TO_F64		1e15
#define MAX_F64_SCALE	22

typedef struct Comp
{
	SiftVexprProg *p;
	int			nregs;
	int			nmasks;
	int			depth;
	bool		fail;
} Comp;

/* the type of a value: for a numeric its scale and the exclusive bound of its unscaled magnitude; inf: an EXTRACT whose infinity is the least or greatest int64 */
typedef struct TI
{
	SiftVecType type;
	int			scale;
	double		bound;
	bool		inf;
} TI;

static int comp_val(Comp *c, const SiftVexprNode *n, int act);
static int comp_bool(Comp *c, const SiftVexprNode *n, int act);

static bool
is_int(SiftVecType t)
{
	return t == SVEC_I32 || t == SVEC_I64;
}

static bool
is_numeric(SiftVecType t)
{
	return t == SVEC_NUM64 || t == SVEC_NUM128;
}

static bool
is_bool_node(const SiftVexprNode *n)
{
	switch (n->kind)
	{
		case SVX_CMP:
		case SVX_AND:
		case SVX_OR:
		case SVX_NOT:
		case SVX_IS_NULL:
		case SVX_IS_NOT_NULL:
		case SVX_MASK_IN:
		case SVX_BOOL_CONST:
			return true;
		case SVX_COALESCE:
		case SVX_CASE:
			return n->type == SVEC_BOOL;
		default:
			return false;
	}
}

/* whether the node has the arguments its kind reads: the compiler indexes args[] only after this */
static bool
arity_ok(const SiftVexprNode *n)
{
	switch (n->kind)
	{
		case SVX_CAST:
		case SVX_NEG:
		case SVX_ABS:
		case SVX_NOT:
		case SVX_IS_NULL:
		case SVX_IS_NOT_NULL:
		case SVX_DATE_TRUNC:
		case SVX_EXTRACT:
			if (n->nargs != 1)
				return false;
			break;
		case SVX_ADD:
		case SVX_SUB:
		case SVX_MUL:
		case SVX_DIV:
		case SVX_MOD:
		case SVX_CMP:
		case SVX_AND:
		case SVX_OR:
			if (n->nargs != 2)
				return false;
			break;
		case SVX_COALESCE:
			if (n->nargs < 2 || n->nargs > SIFT_VEXPR_MAX_ARGS)
				return false;
			break;
		case SVX_CASE:
			if (n->aux < 1 || n->aux > SIFT_VEXPR_MAX_ARGS || n->nargs != 2 * n->aux + n->flag)
				return false;
			break;
		default:
			return true;
	}
	for (int i = 0; i < n->nargs; i++)
		if (!n->args[i])
			return false;
	return true;
}

static double
pow10_d(int e)
{
	double		p = 1.0;

	for (int i = 0; i < e; i++)
		p *= 10.0;
	return p;
}

static double
bound_at(const TI *t, int scale)
{
	return t->bound * pow10_d(scale - t->scale);
}

static SiftVecType
numeric_type(double bound)
{
	return bound <= LIM_NUM64 ? SVEC_NUM64 : SVEC_NUM128;
}

/* the type both operands are brought to: rescale = the numerics get the larger scale (a sum, a compare), else only the width */
static bool
unify(const TI *a, const TI *b, bool rescale, TI *out)
{
	memset(out, 0, sizeof(*out));
	if (a->type == SVEC_F64 || b->type == SVEC_F64)
	{
		const TI   *o = a->type == SVEC_F64 ? b : a;

		if (is_numeric(o->type) && (o->bound > LIM_TO_F64 || o->scale > MAX_F64_SCALE))
			return false;
		out->type = SVEC_F64;
		return true;
	}
	if (is_numeric(a->type) || is_numeric(b->type))
	{
		const int	sa = is_numeric(a->type) ? a->scale : 0,
					sb = is_numeric(b->type) ? b->scale : 0;

		out->scale = rescale ? Max(sa, sb) : 0;
		out->bound = rescale ? Max(a->bound * pow10_d(out->scale - sa), b->bound * pow10_d(out->scale - sb)) : Max(a->bound, b->bound);
		if (out->bound > LIM_NUM128)
			return false;
		out->type = numeric_type(out->bound);
		return true;
	}
	if (!is_int(a->type) || !is_int(b->type))
		return false;
	out->type = a->type == SVEC_I64 || b->type == SVEC_I64 ? SVEC_I64 : SVEC_I32;
	out->bound = Max(a->bound, b->bound);
	return true;
}

static bool ti_of(const SiftVexprNode *n, int depth, TI *t);

/* the type of an operand that computes on its value: an infinity of EXTRACT has no number to compute with */
static bool
ti_finite(const SiftVexprNode *n, int depth, TI *t)
{
	return ti_of(n, depth, t) && !t->inf;
}

static bool
ti_of(const SiftVexprNode *n, int depth, TI *t)
{
	TI			x,
				y;

	memset(t, 0, sizeof(*t));
	if (depth > MAX_DEPTH || !arity_ok(n))
		return false;
	switch (n->kind)
	{
		case SVX_COL:
		case SVX_CONST:
		case SVX_NULL:
			t->type = n->type;
			if (n->type == SVEC_I32)
				t->bound = 2147483648.0;
			else if (n->type == SVEC_I64)
				t->bound = 9.3e18;
			else if (is_numeric(n->type))
			{
				t->scale = n->scale;
				t->bound = n->kind == SVX_COL ? pow10_d(n->prec ? n->prec : n->type == SVEC_NUM64 ? 18 : 38) : fabs((double) n->i64) + 1.0;
				if (n->kind == SVX_COL && n->type == SVEC_NUM64 && n->prec > 18)
					return false;
				if (n->scale < 0 || n->scale > 38 || t->bound > LIM_NUM128)
					return false;
			}
			else if (n->type != SVEC_F64)
				return false;
			return !(n->kind == SVX_CONST && n->type == SVEC_I32 && (n->i64 < INT32_MIN || n->i64 > INT32_MAX));
		case SVX_CAST:
			if (!ti_finite(n->args[0], depth + 1, &x))
				return false;
			t->type = n->type;
			if (n->type == SVEC_I64)
				return x.type == SVEC_I32 && (t->bound = x.bound, true);
			if (n->type == SVEC_F64)
			{
				if (x.type == SVEC_F64)
					return false;
				return (!is_numeric(x.type) || (x.bound <= LIM_TO_F64 && x.scale <= MAX_F64_SCALE));
			}
			if (is_numeric(n->type) && (is_int(x.type) || is_numeric(x.type)))
			{
				const int	from = is_numeric(x.type) ? x.scale : 0;

				t->scale = n->scale;
				t->bound = x.bound * pow10_d(n->scale - from);
				if (n->scale < from || n->scale > 38 || t->bound > LIM_NUM128 || (n->type == SVEC_NUM64 && t->bound > LIM_NUM64))
					return false;
				return true;
			}
			return false;
		case SVX_ADD:
		case SVX_SUB:
		case SVX_MUL:
		case SVX_DIV:
		case SVX_MOD:
			if (!ti_finite(n->args[0], depth + 1, &x) || !ti_finite(n->args[1], depth + 1, &y) || !unify(&x, &y, n->kind != SVX_MUL, t))
				return false;
			if (is_numeric(t->type))
			{
				const int	sx = is_numeric(x.type) ? x.scale : 0,
							sy = is_numeric(y.type) ? y.scale : 0;

				if (n->kind == SVX_DIV || n->kind == SVX_MOD)
					return false;
				if (n->kind == SVX_MUL)
				{
					t->scale = sx + sy;
					t->bound = x.bound * y.bound;
				}
				else
					t->bound = bound_at(&x, t->scale) + bound_at(&y, t->scale);
				if (t->scale > 38 || t->bound > LIM_NUM128)
					return false;
				t->type = numeric_type(t->bound);
			}
			return true;
		case SVX_NEG:
		case SVX_ABS:
			if (!ti_finite(n->args[0], depth + 1, t))
				return false;
			if (is_numeric(t->type))
				t->type = numeric_type(t->bound);
			return t->type != SVEC_BOOL;
		case SVX_COALESCE:
		case SVX_CASE:
			{
				const int	nval = n->kind == SVX_COALESCE ? n->nargs : n->aux + n->flag;
				int			scale_seen = -1;
				bool		same_scale = true;

				for (int i = 0; i < nval; i++)
				{
					const SiftVexprNode *arg = n->args[n->kind == SVX_COALESCE ? i : (i < n->aux ? 2 * i + 1 : 2 * n->aux)];

					if (!ti_finite(arg, depth + 1, &x))
						return false;
					if (i == 0)
						*t = x;
					else if (!unify(t, &x, true, &y))
						return false;
					else
						*t = y;
					scale_seen = scale_seen < 0 ? (is_numeric(x.type) ? x.scale : 0) : scale_seen;
					same_scale &= (is_numeric(x.type) ? x.scale : 0) == scale_seen;
				}
				/* a numeric of PostgreSQL shows the scale of its own value: branches of two scales would print differently where the machine has one scale */
				return t->type != SVEC_BOOL && (!is_numeric(t->type) || same_scale);
			}
		case SVX_DATE_TRUNC:
			if (!ti_of(n->args[0], depth + 1, &x) || (x.type != SVEC_I64 && x.type != SVEC_I32) || n->aux < SVU_SECOND || n->aux > SVU_YEAR)
				return false;
			t->type = SVEC_I64;
			t->bound = 9.3e18;
			return true;
		case SVX_EXTRACT:
			if (!ti_of(n->args[0], depth + 1, &x) || (x.type != SVEC_I64 && x.type != SVEC_I32) || n->aux < SVF_YEAR || n->aux > SVF_EPOCH ||
					(x.type == SVEC_I32 && (n->aux == SVF_HOUR || n->aux == SVF_MINUTE)))
				return false;
			t->type = SVEC_NUM64;
			t->scale = vcal_extract_scale((SiftVxField) n->aux, x.type == SVEC_I32);
			t->bound = 1e18;
			t->inf = n->aux == SVF_YEAR || n->aux == SVF_EPOCH;
			return true;
		default:
			return false;
	}
}

static int
emit(Comp *c, SiftVecOp op)
{
	if (c->p->n_ops >= SIFT_VEXPR_MAX_OPS)
	{
		c->fail = true;
		return 0;
	}
	c->p->wide |= op.type == SVEC_NUM128 || op.type_to == SVEC_NUM128;
	c->p->ops[c->p->n_ops] = op;
	return c->p->n_ops++;
}

static int
reg_alloc(Comp *c)
{
	if (c->nregs >= SIFT_VEXPR_MAX_REGS)
	{
		c->fail = true;
		return SIFT_VEXPR_MAX_REGS - 1;
	}
	c->p->n_regs = Max(c->p->n_regs, c->nregs + 1);
	return c->nregs++;
}

static int
mask_alloc(Comp *c)
{
	if (c->nmasks >= SIFT_VEXPR_MAX_MASKS)
	{
		c->fail = true;
		return SIFT_VEXPR_MAX_MASKS - 1;
	}
	c->p->n_masks = Max(c->p->n_masks, c->nmasks + 1);
	return c->nmasks++;
}

static bool
is_const(const SiftVexprNode *n)
{
	return n->kind == SVX_CONST;
}

static SiftVecOp
op_of(SiftVecOpCode code, SiftVecType type, int act)
{
	SiftVecOp	o = {.op = code, .type = type, .act = (uint8) act};

	return o;
}

static SiftCmpOp
flip(SiftCmpOp op)
{
	return op == SCMP_LT ? SCMP_GT : op == SCMP_LE ? SCMP_GE : op == SCMP_GT ? SCMP_LT : op == SCMP_GE ? SCMP_LE : op;
}

static void
patch_skip(Comp *c, int at)
{
	c->p->ops[at].aux = c->p->n_ops - at - 1;
}

static int
skip_on(Comp *c, int slot)
{
	SiftVecOp	o = op_of(VOP_SKIP_IF_EMPTY, SVEC_I64, 0);

	o.a = (uint8) slot;
	return emit(c, o);
}

static void
emit_cast(Comp *c, int reg, SiftVecType from, SiftVecType to, int scale_from, int scale_to, int act)
{
	SiftVecOp	o = op_of(VOP_CAST, from, act);

	o.type_to = to;
	o.dst = o.a = (uint8) reg;
	o.scale = (int16) scale_to;
	o.aux = scale_from;
	emit(c, o);
}

/* the register, in place, as the type `to`: the casts of a number to a wider one, to a numeric and to float8, and the rescaling of a numeric */
static void
coerce(Comp *c, int reg, const TI *from, const TI *to, int act)
{
	int			scale = is_numeric(from->type) ? from->scale : 0;
	SiftVecType type = from->type;

	if (type == to->type && (!is_numeric(type) || from->scale == to->scale))
		return;
	if (to->type == SVEC_F64)
	{
		emit_cast(c, reg, from->type, SVEC_F64, scale, 0, act);
		return;
	}
	if (!is_numeric(to->type))
	{
		emit_cast(c, reg, from->type, to->type, 0, 0, act);
		return;
	}
	if (type != to->type)
	{
		if (type == SVEC_I64 && to->type == SVEC_NUM64)
			c->fail = true;
		emit_cast(c, reg, type, to->type, scale, scale, act);
		type = to->type;
	}
	if (to->scale > scale)
	{
		SiftVecOp	o = op_of(VOP_RESCALE, type, act);

		o.dst = o.a = (uint8) reg;
		o.aux = to->scale - scale;
		o.scale = (int16) to->scale;
		emit(c, o);
	}
}

static bool
fits_imm(const SiftVexprNode *n, const TI *tn, const TI *target)
{
	return is_const(n) && tn->type == target->type && (!is_numeric(tn->type) || tn->scale == target->scale) && target->type != SVEC_NUM128 &&
		(target->type != SVEC_I32 || (n->i64 >= INT32_MIN && n->i64 <= INT32_MAX));
}

/* what an operand is brought to: the common type; a product keeps the scale of each operand */
static TI
operand_target(const TI *tc, const TI *own, bool keep_scale)
{
	TI			t = *tc;

	if (keep_scale && is_numeric(tc->type))
		t.scale = is_numeric(own->type) ? own->scale : 0;
	return t;
}

static int
comp_arith(Comp *c, const SiftVexprNode *n, int act)
{
	const SiftVecOpCode code = n->kind == SVX_ADD ? VOP_ADD : n->kind == SVX_SUB ? VOP_SUB : n->kind == SVX_MUL ? VOP_MUL :
		n->kind == SVX_DIV ? VOP_DIV : n->kind == SVX_MOD ? VOP_MOD : n->kind == SVX_NEG ? VOP_NEG : VOP_ABS;
	const bool	unary = n->nargs == 1;
	const SiftVexprNode *x = n->args[0],
			   *y = unary ? NULL : n->args[1];
	TI			tx,
				ty,
				tc,
				res,
				ox,
				oy;
	SiftVecOp	o;
	int			d;

	if (!unary && (n->kind == SVX_ADD || n->kind == SVX_MUL) && is_const(x) && !is_const(y))
	{
		const SiftVexprNode *t = x;

		x = y;
		y = t;
	}
	if (!ti_of(n, 0, &res) || !ti_of(x, 0, &tx) || (y && !ti_of(y, 0, &ty)))
	{
		c->fail = true;
		return reg_alloc(c);
	}
	if (unary)
		tc = tx;
	else if (!unify(&tx, &ty, n->kind != SVX_MUL, &tc))
	{
		c->fail = true;
		return reg_alloc(c);
	}
	if (is_numeric(tc.type))
		tc.type = res.type;
	ox = operand_target(&tc, &tx, n->kind == SVX_MUL);
	oy = unary ? ox : operand_target(&tc, &ty, n->kind == SVX_MUL);
	o = op_of(code, tc.type, act);
	o.scale = (int16) res.scale;
	d = comp_val(c, x, act);
	coerce(c, d, &tx, &ox, act);
	o.dst = o.a = (uint8) d;
	if (y && fits_imm(y, &ty, &oy))
	{
		o.flags = VOP_FLAG_CONST_B;
		o.imm_i64 = y->i64;
		o.imm_f64 = y->f64;
	}
	else if (y)
	{
		o.b = (uint8) comp_val(c, y, act);
		coerce(c, o.b, &ty, &oy, act);
		c->nregs = d + 1;
	}
	emit(c, o);
	return d;
}

/* COALESCE(args[i], ...): the next argument runs where this one is NULL */
static int
comp_coalesce(Comp *c, const SiftVexprNode *n, int i, int act, const TI *tc)
{
	TI			ta;
	const int	d = comp_val(c, n->args[i], act);
	int			m2,
				sk;
	SiftVecOp	o;

	if (!ti_of(n->args[i], 0, &ta))
		c->fail = true;
	coerce(c, d, &ta, tc, act);
	if (i == n->nargs - 1)
		return d;
	m2 = mask_alloc(c);
	o = op_of(VOP_ACT_NOTVALID, tc->type, act);
	o.dst = (uint8) m2;
	o.a = (uint8) act;
	o.b = (uint8) d;
	emit(c, o);
	sk = skip_on(c, m2);
	o = op_of(VOP_COPY, tc->type, act);
	o.dst = o.a = (uint8) d;
	o.scale = (int16) tc->scale;
	emit(c, o);
	comp_coalesce(c, n, i + 1, m2, tc);
	o = op_of(VOP_BLEND, tc->type, act);
	o.dst = (uint8) d;
	o.a = (uint8) (d + 1);
	o.c = (uint8) m2;
	emit(c, o);
	patch_skip(c, sk);
	c->nregs = d + 1;
	c->nmasks = m2;
	return d;
}

static void
blend_under(Comp *c, SiftVecType type, int d, int src, int mask)
{
	SiftVecOp	o = op_of(VOP_BLEND, type, mask);

	o.dst = (uint8) d;
	o.a = (uint8) src;
	o.c = (uint8) mask;
	emit(c, o);
}

/* the branch value of a CASE as the type tc, blended into d under the rows of mask */
static void
blend_branch(Comp *c, const SiftVexprNode *arg, const TI *tc, int d, int mask)
{
	TI			ta;
	int			src;

	if (!ti_of(arg, 0, &ta))
		c->fail = true;
	src = comp_val(c, arg, mask);
	coerce(c, src, &ta, tc, mask);
	blend_under(c, tc->type, d, src, mask);
}

static int
comp_case(Comp *c, const SiftVexprNode *n, int act, const TI *tc)
{
	const int	d = reg_alloc(c),
				m0 = c->nmasks,
				rem = mask_alloc(c);
	const int	nwhen = n->aux;
	SiftVecOp	o = op_of(VOP_INIT_NULL, tc->type, act);

	o.dst = (uint8) d;
	o.scale = (int16) tc->scale;
	emit(c, o);
	o = op_of(VOP_ACT_COPY, SVEC_I64, act);
	o.dst = (uint8) rem;
	o.a = (uint8) act;
	emit(c, o);
	for (int k = 0; k < nwhen; k++)
	{
		const int	sk0 = k ? skip_on(c, rem) : -1;
		const int	cond = comp_bool(c, n->args[2 * k], rem),
					br = mask_alloc(c);
		int			sk;

		o = op_of(VOP_ACT_AND, SVEC_I64, act);
		o.dst = (uint8) br;
		o.a = (uint8) rem;
		o.b = (uint8) cond;
		emit(c, o);
		sk = skip_on(c, br);
		blend_branch(c, n->args[2 * k + 1], tc, d, br);
		o = op_of(VOP_ACT_ANDN, SVEC_I64, act);
		o.dst = o.a = (uint8) rem;
		o.b = (uint8) br;
		emit(c, o);
		patch_skip(c, sk);
		if (sk0 >= 0)
			patch_skip(c, sk0);
		c->nregs = d + 1;
		c->nmasks = rem + 1;
	}
	if (n->flag)
	{
		const int	sk = skip_on(c, rem);

		blend_branch(c, n->args[2 * nwhen], tc, d, rem);
		patch_skip(c, sk);
	}
	c->nregs = d + 1;
	c->nmasks = m0;
	return d;
}

static int
comp_calendar(Comp *c, const SiftVexprNode *n, int act)
{
	TI			ta,
				res;
	const int	d = comp_val(c, n->args[0], act);
	SiftVecOp	o;

	if (!ti_of(n, 0, &res) || !ti_of(n->args[0], 0, &ta))
		c->fail = true;
	o = op_of(n->kind == SVX_DATE_TRUNC ? VOP_DATE_TRUNC : VOP_EXTRACT, ta.type, act);
	o.type_to = res.type;
	o.scale = (int16) res.scale;
	o.dst = o.a = (uint8) d;
	o.aux = n->aux;
	o.zone = n->zone;
	emit(c, o);
	return d;
}

static int
comp_val_node(Comp *c, const SiftVexprNode *n, int act)
{
	SiftVecOp	o;
	TI			t;
	int			d;

	if (!arity_ok(n) || !ti_of(n, 0, &t))
	{
		c->fail = true;
		return reg_alloc(c);
	}
	switch (n->kind)
	{
		case SVX_COL:
		case SVX_CONST:
		case SVX_NULL:
			d = reg_alloc(c);
			o = op_of(n->kind == SVX_COL ? VOP_LOAD_COL : n->kind == SVX_CONST ? VOP_LOAD_CONST : VOP_LOAD_NULL, n->type, act);
			o.dst = (uint8) d;
			o.aux = n->aux;
			o.imm_i64 = n->i64;
			o.imm_f64 = n->f64;
			o.scale = (int16) t.scale;
			emit(c, o);
			return d;
		case SVX_CAST:
			{
				TI			from;

				d = comp_val(c, n->args[0], act);
				if (!ti_of(n->args[0], 0, &from))
					c->fail = true;
				coerce(c, d, &from, &t, act);
				return d;
			}
		case SVX_ADD:
		case SVX_SUB:
		case SVX_MUL:
		case SVX_DIV:
		case SVX_MOD:
		case SVX_NEG:
		case SVX_ABS:
			return comp_arith(c, n, act);
		case SVX_COALESCE:
			return comp_coalesce(c, n, 0, act, &t);
		case SVX_CASE:
			return comp_case(c, n, act, &t);
		case SVX_DATE_TRUNC:
		case SVX_EXTRACT:
			return comp_calendar(c, n, act);
		default:
			c->fail = true;
			return reg_alloc(c);
	}
}

static int
comp_val(Comp *c, const SiftVexprNode *n, int act)
{
	int			d;

	if (c->fail || c->depth >= MAX_DEPTH)
	{
		c->fail = true;
		return reg_alloc(c);
	}
	c->depth++;
	d = comp_val_node(c, n, act);
	c->depth--;
	return d;
}

/* the least and greatest int64 that stand for the infinities of EXTRACT keep their order only unscaled, against a finite numeric of 64 bits (never either of them) */
static bool
orders_infinity(const TI *t, const TI *tc)
{
	return !t->inf || (t->type == SVEC_NUM64 && tc->type == SVEC_NUM64 && t->scale == tc->scale);
}

static int
comp_cmp(Comp *c, const SiftVexprNode *n, int act)
{
	const SiftVexprNode *x = n->args[0],
			   *y = n->args[1];
	SiftCmpOp	cmp = (SiftCmpOp) n->aux;
	SiftVecOp	o;
	TI			tx,
				ty,
				tc;
	const int	d0 = c->nregs;
	int			ms,
				dx;

	if (is_const(x) && !is_const(y))
	{
		const SiftVexprNode *t = x;

		x = y;
		y = t;
		cmp = flip(cmp);
	}
	if (!ti_of(x, 0, &tx) || !ti_of(y, 0, &ty) || !unify(&tx, &ty, true, &tc) ||
		!orders_infinity(&tx, &tc) || !orders_infinity(&ty, &tc))
	{
		c->fail = true;
		return mask_alloc(c);
	}
	o = op_of(VOP_CMP, tc.type, act);
	dx = comp_val(c, x, act);
	coerce(c, dx, &tx, &tc, act);
	o.a = (uint8) dx;
	if (fits_imm(y, &ty, &tc))
	{
		o.flags = VOP_FLAG_CONST_B;
		o.imm_i64 = y->i64;
		o.imm_f64 = y->f64;
	}
	else
	{
		o.b = (uint8) comp_val(c, y, act);
		coerce(c, o.b, &ty, &tc, act);
	}
	c->nregs = d0;
	ms = mask_alloc(c);
	o.dst = (uint8) ms;
	o.aux = cmp;
	emit(c, o);
	return ms;
}

/* a boolean CASE: the rows of each WHEN that is the first true one take the branch's value and whether it is known; a row that none takes is unknown */
static int
comp_bool_case(Comp *c, const SiftVexprNode *n, int act)
{
	const int	res = mask_alloc(c),
				rem = mask_alloc(c);
	SiftVecOp	o = op_of(VOP_MASK_UNKNOWN, SVEC_I64, act);

	o.dst = (uint8) res;
	emit(c, o);
	o = op_of(VOP_ACT_COPY, SVEC_I64, act);
	o.dst = (uint8) rem;
	o.a = (uint8) act;
	emit(c, o);
	for (int k = 0; k < n->aux; k++)
	{
		const int	sk0 = k ? skip_on(c, rem) : -1;
		const int	cond = comp_bool(c, n->args[2 * k], rem),
					br = mask_alloc(c);
		int			sk,
					val;

		o = op_of(VOP_ACT_AND, SVEC_I64, act);
		o.dst = (uint8) br;
		o.a = (uint8) rem;
		o.b = (uint8) cond;
		emit(c, o);
		sk = skip_on(c, br);
		val = comp_bool(c, n->args[2 * k + 1], br);
		o = op_of(VOP_MASK_BLEND, SVEC_I64, act);
		o.dst = (uint8) res;
		o.a = (uint8) val;
		o.c = (uint8) br;
		emit(c, o);
		o = op_of(VOP_ACT_ANDN, SVEC_I64, act);
		o.dst = o.a = (uint8) rem;
		o.b = (uint8) br;
		emit(c, o);
		patch_skip(c, sk);
		if (sk0 >= 0)
			patch_skip(c, sk0);
		c->nmasks = rem + 1;
	}
	if (n->flag)
	{
		const int	sk = skip_on(c, rem);
		const int	val = comp_bool(c, n->args[2 * n->aux], rem);

		o = op_of(VOP_MASK_BLEND, SVEC_I64, act);
		o.dst = (uint8) res;
		o.a = (uint8) val;
		o.c = (uint8) rem;
		emit(c, o);
		patch_skip(c, sk);
	}
	c->nmasks = res + 1;
	return res;
}

static int
comp_bool_coalesce(Comp *c, const SiftVexprNode *n, int act)
{
	const int	res = comp_bool(c, n->args[0], act);

	c->nmasks = res + 1;
	for (int i = 1; i < n->nargs; i++)
	{
		const int	mact = mask_alloc(c);
		SiftVecOp	o = op_of(VOP_ACT_UNKNOWN, SVEC_I64, act);
		int			sk,
					val;

		o.dst = (uint8) mact;
		o.a = (uint8) act;
		o.b = (uint8) res;
		emit(c, o);
		sk = skip_on(c, mact);
		val = comp_bool(c, n->args[i], mact);
		o = op_of(VOP_MASK_BLEND, SVEC_I64, act);
		o.dst = (uint8) res;
		o.a = (uint8) val;
		o.c = (uint8) mact;
		emit(c, o);
		patch_skip(c, sk);
		c->nmasks = res + 1;
	}
	return res;
}

static int
comp_bool_node(Comp *c, const SiftVexprNode *n, int act)
{
	SiftVecOp	o;
	int			ms;

	if (!arity_ok(n))
	{
		c->fail = true;
		return mask_alloc(c);
	}
	switch (n->kind)
	{
		case SVX_CMP:
			return comp_cmp(c, n, act);
		case SVX_AND:
		case SVX_OR:
			{
				const bool	is_and = n->kind == SVX_AND;
				int			mact,
							sk;

				ms = comp_bool(c, n->args[0], act);
				mact = mask_alloc(c);
				o = op_of(is_and ? VOP_ACT_NOTFALSE : VOP_ACT_ANDN, SVEC_I64, act);
				o.dst = (uint8) mact;
				o.a = (uint8) act;
				o.b = (uint8) ms;
				emit(c, o);
				sk = skip_on(c, mact);
				comp_bool(c, n->args[1], mact);
				o = op_of(is_and ? VOP_MASK_AND : VOP_MASK_OR, SVEC_I64, act);
				o.dst = o.a = (uint8) ms;
				o.b = (uint8) (mact + 1);
				emit(c, o);
				patch_skip(c, sk);
				c->nmasks = ms + 1;
				return ms;
			}
		case SVX_NOT:
			ms = comp_bool(c, n->args[0], act);
			o = op_of(VOP_MASK_NOT, SVEC_I64, act);
			o.dst = o.a = (uint8) ms;
			emit(c, o);
			return ms;
		case SVX_IS_NULL:
		case SVX_IS_NOT_NULL:
			if (is_bool_node(n->args[0]))
			{
				ms = comp_bool(c, n->args[0], act);
				o = op_of(VOP_MASK_ISNULL, SVEC_I64, act);
				o.dst = o.a = (uint8) ms;
				o.aux = n->kind == SVX_IS_NOT_NULL;
				emit(c, o);
				return ms;
			}
			{
				const int	d = comp_val(c, n->args[0], act);

				c->nregs = d;
				ms = mask_alloc(c);
				o = op_of(VOP_IS_NULL, SVEC_I64, act);
				o.dst = (uint8) ms;
				o.a = (uint8) d;
				o.aux = n->kind == SVX_IS_NOT_NULL;
				emit(c, o);
				return ms;
			}
		case SVX_MASK_IN:
		case SVX_BOOL_CONST:
			ms = mask_alloc(c);
			o = op_of(n->kind == SVX_MASK_IN ? VOP_MASK_IN : VOP_MASK_CONST, SVEC_I64, act);
			o.dst = (uint8) ms;
			o.aux = n->kind == SVX_MASK_IN ? n->aux : n->flag;
			emit(c, o);
			return ms;
		case SVX_CASE:
		case SVX_COALESCE:
			if (n->type != SVEC_BOOL)
			{
				c->fail = true;
				return mask_alloc(c);
			}
			return n->kind == SVX_CASE ? comp_bool_case(c, n, act) : comp_bool_coalesce(c, n, act);
		default:
			c->fail = true;
			return mask_alloc(c);
	}
}

static int
comp_bool(Comp *c, const SiftVexprNode *n, int act)
{
	int			ms;

	if (c->fail || c->depth >= MAX_DEPTH)
	{
		c->fail = true;
		return mask_alloc(c);
	}
	c->depth++;
	ms = comp_bool_node(c, n, act);
	c->depth--;
	return ms;
}

bool
sift_vexpr_compile(const SiftVexprNode *root, SiftVexprProg *out)
{
	Comp		c = {out, 0, 1, 0, false};
	TI			t = {SVEC_BOOL, 0, 0, false};

	memset(out, 0, sizeof(*out));
	out->n_masks = 1;
	out->out_is_bool = is_bool_node(root);
	if (!out->out_is_bool && !ti_finite(root, 0, &t))
		return false;
	out->out = (uint8) (out->out_is_bool ? comp_bool(&c, root, 0) : comp_val(&c, root, 0));
	out->out_type = t.type;
	out->out_scale = (int16) t.scale;
	return !c.fail;
}
