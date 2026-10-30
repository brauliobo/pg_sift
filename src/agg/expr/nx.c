/*
 * agg/expr/nx.c - the numeric programs of an expression
 * The integer and numeric steps the scan runs for an expression, and whether they
 * can raise.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"

#include "agg/expr/int.h"

/* digits an integer or numeric(p, s) column may hold: from the type modifier of its reference, else what the storage holds */
int
leaf_digits(Oid typid, int32 typmod)
{
	if (typid == INT2OID)
		return 5;
	if (typid == INT4OID)
		return 10;
	return typmod >= 0 ? ((typmod - VARHDRSZ) >> 16) & 0xffff : 19;
}

static bool
nx_put(SiftAggNum *o, SiftAggNxOp op, int leaf, int a, int b, sift_i128 c)
{
	if (o->n >= SIFT_AGG_MAX_VX)
		return false;
	o->step[o->n++] = (SiftAggNx) {op, leaf, a, b, c};
	return true;
}

static bool
nx_leaf(AggCtx *c, Node *n, int leaf, SiftAggNum *o, int *scale, int *dig)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	if (l->kind != SCL_SCALAR || !((l->vtype == SCV_I64 && (l->typid == NUMERICOID || l->typid == INT8OID)) ||
								   (l->vtype == SCV_I32 && (l->typid == INT2OID || l->typid == INT4OID))))
		return false;
	use_leaf(c, leaf);
	*scale = l->typid == NUMERICOID ? l->scale : 0;
	*dig = leaf_digits(l->typid, exprTypmod(n));
	return nx_put(o, SNX_LEAF, leaf, 0, 0, 0);
}

/* a constant is its text: the digits and the dscale PostgreSQL prints; NaN and the infinities are not numbers of the program */
static bool
nx_const(Const *k, SiftAggNum *o, int *scale, int *dig)
{
	sift_i128	v,
				m;

	if (k->constisnull)
		return false;
	if (k->consttype == INT2OID || k->consttype == INT4OID)
	{
		v = k->consttype == INT2OID ? DatumGetInt16(k->constvalue) : DatumGetInt32(k->constvalue);
		*scale = 0;
	}
	else if (k->consttype != NUMERICOID || !sift_parse_decimal(DatumGetCString(DirectFunctionCall1(numeric_out, k->constvalue)), &v, scale))
		return false;
	for (*dig = 1, m = v < 0 ? -v : v; m >= 10; m /= 10)
		(*dig)++;
	return nx_put(o, SNX_CONST, -1, 0, 0, v);
}

/* left op right, the result scale the larger one (numeric_add, numeric_sub); *scale and *dig are the left operand's on entry */
bool
nx_addsub(SiftAggNum *o, SiftAggNxOp op, int *scale, int *dig, int s2, int d2)
{
	int			s1 = *scale,
				sc = Max(s1, s2);

	*dig = Max(*dig + sc - s1, d2 + sc - s2) + 1;
	*scale = sc;
	return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, op, -1, sc - s1, sc - s2, 0);
}

/* round, trunc, floor, ceil to n digits after the point: the result has scale n (0 for a negative n), as numeric_round and its kin give */
static bool
nx_rescale(SiftAggNum *o, SiftAggNxOp op, int n, int *scale, int *dig)
{
	int			a = Max(*scale - n, 0),
				b = n >= *scale ? n - *scale : Max(-n, 0);

	*dig = (a > 0 ? Max(*dig - a, 0) + 1 : *dig) + b;
	*scale = Max(n, 0);
	return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, op, -1, a, b, 0);
}

/*
 * The program of a numeric expression: leaves, numeric constants, + - * of int2 and int4 whose digits keep the result inside the type, + - * and unary minus, abs, round / trunc / floor / ceil with a constant
 * number of digits, the casts of integers to numeric and of numeric to an integer (the digits it needs fit the integer: no overflow
 * error to reproduce). Anything else (division, other functions, parameters inside the arithmetic, a result of more than
 * SIFT_NUM_MAX_DIGITS digits) is false: its scale or its errors are not known here.
 */
bool
nx_walk(AggCtx *c, Node *n, SiftAggNum *o, int *scale, int *dig)
{
	int			leaf,
				s2,
				d2,
				nd = 0,
				maxd = SIFT_NUM_MAX_DIGITS;
	Oid			fid;
	List	   *args;
	SiftAggNxOp op;

	n = strip_relabel(n);
	if (IsA(n, Const))
		return nx_const((Const *) n, o, scale, dig);
	if ((leaf = match_leaf(c, n)) >= 0)
		return nx_leaf(c, n, leaf, o, scale, dig);
	fid = func_of(n);
	args = func_args(n);
	switch (fid)
	{
		case F_NUMERIC_ADD:
		case F_NUMERIC_SUB:
		case F_NUMERIC_MUL:
			if (!nx_walk(c, linitial(args), o, scale, dig) || !nx_walk(c, lsecond(args), o, &s2, &d2))
				return false;
			if (fid != F_NUMERIC_MUL)
				return nx_addsub(o, fid == F_NUMERIC_ADD ? SNX_ADD : SNX_SUB, scale, dig, s2, d2);
			*scale += s2;
			*dig += d2;
			return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, SNX_MUL, -1, 0, 0, 0);
		/* + - * of int2 and int4: PostgreSQL raises when the result leaves the type, so the digits must prove it cannot (4 for int2, 9 for int4) */
		case F_INT2PL:
		case F_INT2MI:
		case F_INT2MUL:
		case F_INT4PL:
		case F_INT4MI:
		case F_INT4MUL:
		case F_INT24PL:
		case F_INT24MI:
		case F_INT24MUL:
		case F_INT42PL:
		case F_INT42MI:
		case F_INT42MUL:
			maxd = (fid == F_INT2PL || fid == F_INT2MI || fid == F_INT2MUL) ? 4 : 9;
			if (!nx_walk(c, linitial(args), o, scale, dig) || !nx_walk(c, lsecond(args), o, &s2, &d2) || *scale != 0 || s2 != 0)
				return false;
			if (fid == F_INT2MUL || fid == F_INT4MUL || fid == F_INT24MUL || fid == F_INT42MUL)
			{
				*dig += d2;
				return *dig <= maxd && nx_put(o, SNX_MUL, -1, 0, 0, 0);
			}
			return nx_addsub(o, (fid == F_INT2PL || fid == F_INT4PL || fid == F_INT24PL || fid == F_INT42PL) ? SNX_ADD : SNX_SUB, scale, dig, s2, d2) && *dig <= maxd;
		case F_NUMERIC_UMINUS:
		case F_ABS_NUMERIC:
		case F_NUMERIC_ABS:
			return nx_walk(c, linitial(args), o, scale, dig) && nx_put(o, fid == F_NUMERIC_UMINUS ? SNX_NEG : SNX_ABS, -1, 0, 0, 0);
		case F_NUMERIC_INT2:
		case F_NUMERIC_INT4:
		case F_NUMERIC_INT8:
			return nx_walk(c, linitial(args), o, scale, dig);
		case F_INT2_NUMERIC:
		case F_INT4_NUMERIC:
		case F_INT8_NUMERIC:
			maxd = fid == F_INT2_NUMERIC ? 4 : fid == F_INT4_NUMERIC ? 9 : 18;
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, SNX_ROUND, 0, scale, dig) && *dig <= maxd &&
				nx_put(o, SNX_INTCAST, -1, fid == F_INT2_NUMERIC ? 2 : fid == F_INT4_NUMERIC ? 4 : 8, 0, 0);
		case F_ROUND_NUMERIC:
		case F_ROUND_NUMERIC_INT4:
		case F_TRUNC_NUMERIC:
		case F_TRUNC_NUMERIC_INT4:
		case F_FLOOR_NUMERIC:
		case F_CEIL_NUMERIC:
		case F_CEILING_NUMERIC:
			if (list_length(args) == 2)
			{
				Const	   *k = (Const *) strip_relabel(lsecond(args));

				if (!IsA(k, Const) || k->constisnull || abs(nd = DatumGetInt32(k->constvalue)) > SIFT_NUM_MAX_DIGITS)
					return false;
			}
			op = (fid == F_ROUND_NUMERIC || fid == F_ROUND_NUMERIC_INT4) ? SNX_ROUND :
				(fid == F_TRUNC_NUMERIC || fid == F_TRUNC_NUMERIC_INT4) ? SNX_TRUNC : fid == F_FLOOR_NUMERIC ? SNX_FLOOR : SNX_CEIL;
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, op, nd, scale, dig);
		default:
			return false;
	}
}

/* the program of e, at most maxdig digits wide */
bool
compile_nx(AggCtx *c, Node *e, int maxdig, SiftAggNum *o)
{
	int			dig;

	o->n = 0;
	o->step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
	return nx_walk(c, e, o, &o->scale, &dig) && dig <= maxdig && (o->dig = dig, true);
}

/*
 * The product of two numeric expressions of at most SIFT_NUM_MAX_DIGITS digits each whose digits together exceed them (a price times one minus a discount
 * times one plus a tax, 47 digits for three decimal(15,2)): the program leaves the two factors on the stack and no multiplication, the sum multiplies
 * each pair in 256 bits (agg_sum_add_product). The digits proven are those of the product, up to SIFT_NUM_PRODUCT_DIGITS: the carries of the sum, in 64 bits, hold 2^31 rows of the largest such value.
 */
bool
compile_nx_product(AggCtx *c, Node *e, SiftAggNum *o)
{
	Node	   *n = strip_relabel(e);
	int			s1,
				d1,
				s2,
				d2;

	o->n = 0;
	o->step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
	return func_of(n) == F_NUMERIC_MUL && nx_walk(c, linitial(func_args(n)), o, &s1, &d1) && nx_walk(c, lsecond(func_args(n)), o, &s2, &d2) &&
		d1 + d2 > SIFT_NUM_MAX_DIGITS && d1 + d2 <= SIFT_NUM_PRODUCT_DIGITS && (o->scale = s1 + s2, o->dig = d1 + d2, true);
}

/* + - * of int8, int4 and int2 operands (int8pl, int84pl, int48pl ...): the SNX op of the function, -1 for any other */
static int
int8_arith_op(Oid fid)
{
	switch (fid)
	{
		case F_INT8PL:
		case F_INT84PL:
		case F_INT82PL:
		case F_INT48PL:
		case F_INT28PL:
		case F_INT24PL:
		case F_INT42PL:
			return SNX_ADD;
		case F_INT8MI:
		case F_INT84MI:
		case F_INT82MI:
		case F_INT48MI:
		case F_INT28MI:
		case F_INT24MI:
		case F_INT42MI:
			return SNX_SUB;
		case F_INT8MUL:
		case F_INT84MUL:
		case F_INT82MUL:
		case F_INT48MUL:
		case F_INT28MUL:
		case F_INT24MUL:
		case F_INT42MUL:
			return SNX_MUL;
		default:
			return -1;
	}
}

/* does int2 OP k stay in int4 for every smallint (so the integer operators of PostgreSQL never raise)? */
static bool
int2_arith_safe(Oid fid, int64 k)
{
	switch (fid)
	{
		case F_INT24PL:
		case F_INT42PL:
		case F_INT24MI:
		case F_INT42MI:
			return k >= -(PG_INT32_MAX - 32768) && k <= PG_INT32_MAX - 32768;
		case F_INT24MUL:
		case F_INT42MUL:
			return k >= -65535 && k <= 65535;
		default:
			return false;
	}
}

/*
 * `leaf OP constant` or `constant OP leaf` of an int8 column (or a smallint one and an integer constant), OP being + - *: the program ends in a cast to int8 that raises "bigint out of range" for
 * a result outside it, as int8pl and its kin do. The rows the cast looks at are the aggregate's (eval_nx).
 */
bool
compile_int8_arith(AggCtx *c, Node *e, SiftAggNum *o)
{
	Node	   *e1 = strip_relabel(e);
	int			op = int8_arith_op(func_of(e1)),
				leaf = -1,
				k = -1;
	List	   *args = func_args(e1);

	if (op < 0 || list_length(args) != 2)
		return false;
	for (int i = 0; i < 2; i++)
	{
		Node	   *a = strip_relabel(list_nth(args, i));
		Const	   *kc = (Const *) a;

		if (IsA(a, Const))
		{
			if (kc->constisnull || (kc->consttype != INT8OID && kc->consttype != INT4OID && kc->consttype != INT2OID) || k >= 0)
				return false;
			k = i;
		}
		else if ((leaf = match_leaf(c, a)) < 0)
			return false;
	}
	if (k < 0 || leaf < 0 || leaf_of(c, leaf)->kind != SCL_SCALAR || leaf_of(c, leaf)->wide)
		return false;
	{
		Const	   *kc = (Const *) strip_relabel(list_nth(args, k));
		int64		v = kc->consttype == INT8OID ? DatumGetInt64(kc->constvalue) : kc->consttype == INT4OID ? DatumGetInt32(kc->constvalue) : DatumGetInt16(kc->constvalue);

		/*
		 * A smallint column and an integer constant (int24pl, int42mi ..) give an integer, which raises past int4: a result that cannot leave it is
		 * the program of an int8 column without the check. A constant of up to 2^31 - 2^15 - 1 added or subtracted, or 65,535 multiplied, keeps every int2 in range.
		 */
		if (leaf_of(c, leaf)->vtype == SCV_I32 && leaf_of(c, leaf)->typid == INT2OID)
		{
			if (kc->consttype != INT4OID || !int2_arith_safe(func_of(e1), v))
				return false;
		}
		else if (leaf_of(c, leaf)->vtype != SCV_I64 || leaf_of(c, leaf)->typid != INT8OID)
			return false;

		o->n = 0;
		o->scale = 0;
		o->step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
		use_leaf(c, leaf);
		return (k == 0 ? nx_put(o, SNX_CONST, -1, 0, 0, v) && nx_put(o, SNX_LEAF, leaf, 0, 0, 0) : nx_put(o, SNX_LEAF, leaf, 0, 0, 0) && nx_put(o, SNX_CONST, -1, 0, 0, v)) &&
			nx_put(o, op, -1, 0, 0, 0) && nx_put(o, SNX_INTCAST, -1, 8, 1, 0);
	}
}

/* the level (0 row, 1 element) of the leaves of a program, -1 when it mixes them */
int
nx_level(AggCtx *c, const SiftAggNum *o)
{
	int			lvl = -1;

	for (int k = 0; k < o->n; k++)
		if (o->step[k].op == SNX_LEAF)
		{
			int			l = leaf_level(c, o->step[k].leaf);

			if (lvl >= 0 && l != lvl)
				return -1;
			lvl = l;
		}
	return lvl;
}

/* does the program raise for a NaN (a cast to an integer)? */
bool
nx_raises(const SiftAggNum *o)
{
	for (int k = 0; k < o->n; k++)
		if (o->step[k].op == SNX_INTCAST)
			return true;
	return false;
}
