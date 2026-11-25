/*
 * common_vexpr.glsl - the expression machine of the aggregate kernels: the program of agg/vexpr/vexpr.h (SiftVexprProg) as 8-word operations run
 * for one row by one invocation. Registers are 16 values (an int64, or the bits of a double) with a valid bit each, and 16 three-valued
 * boolean slots (a true bit and a known bit); an operation that raises an error in PostgreSQL does so here only on a row that is live for
 * it (its act slot is true), and then the row's evaluation stops with the error code. Operation 0..32 are those of SiftVecOpCode, 40 is the
 * membership of a sorted list or a bitset of the pool.
 */
#define VOP_LOAD_COL		0u
#define VOP_LOAD_CONST		1u
#define VOP_LOAD_NULL		2u
#define VOP_CAST			3u
#define VOP_ADD				4u
#define VOP_SUB				5u
#define VOP_MUL				6u
#define VOP_DIV				7u
#define VOP_NEG				8u
#define VOP_ABS				9u
#define VOP_CMP				10u
#define VOP_IS_NULL			11u
#define VOP_MASK_AND		12u
#define VOP_MASK_OR			13u
#define VOP_MASK_NOT		14u
#define VOP_MASK_COL		15u
#define VOP_MASK_CONST		16u
#define VOP_INIT_NULL		17u
#define VOP_COPY			18u
#define VOP_BLEND			19u
#define VOP_ACT_COPY		20u
#define VOP_ACT_AND			21u
#define VOP_ACT_ANDN		22u
#define VOP_ACT_NOTFALSE	23u
#define VOP_ACT_NOTVALID	24u
#define VOP_SKIP_IF_EMPTY	25u
#define VOP_RESCALE			26u
#define VOP_DATE_TRUNC		27u
#define VOP_EXTRACT			28u
#define VOP_MASK_BLEND		29u
#define VOP_MASK_UNKNOWN	30u
#define VOP_MASK_ISNULL		31u
#define VOP_ACT_UNKNOWN		32u
#define VOP_IN_SET			40u

#define GT_I32		0u
#define GT_I64		1u
#define GT_NUM		2u
#define GT_F64		3u

#define VE_INT4		1u		/* integer out of range */
#define VE_INT8		2u		/* bigint out of range */
#define VE_DIV0		3u		/* division by zero */
#define VE_FLOAT_OVF	4u	/* value out of range: overflow */
#define VE_FLOAT_UNF	5u	/* value out of range: underflow */
#define VE_TIME		6u		/* a timestamp the calendar does not take (an infinity) */
#define VE_UNSUP	7u		/* an operation the machine does not run */

int64_t R[16];
uint RV;
uint MT, MK;
uint vm_err;
uint vm_err_op;

const int64_t P10I[19] = int64_t[19](1l, 10l, 100l, 1000l, 10000l, 100000l, 1000000l, 10000000l, 100000000l, 1000000000l, 10000000000l,
									 100000000000l, 1000000000000l, 10000000000000l, 100000000000000l, 1000000000000000l,
									 10000000000000000l, 100000000000000000l, 1000000000000000000l);

bool rvalid(uint r)
{
	return ((RV >> r) & 1u) != 0u;
}

void rset(uint r, int64_t v, bool valid)
{
	R[r] = v;
	RV = valid ? (RV | (1u << r)) : (RV & ~(1u << r));
}

void mset(uint s, bool t, bool k)
{
	uint bit = 1u << s;

	MT = (t && k) ? (MT | bit) : (MT & ~bit);
	MK = k ? (MK | bit) : (MK & ~bit);
}

bool mtrue(uint s)
{
	return ((MT >> s) & 1u) != 0u;
}

bool mknown(uint s)
{
	return ((MK >> s) & 1u) != 0u;
}

bool live(uint act)
{
	return ((MT >> act) & 1u) != 0u;
}

void verr(uint code, uint op)
{
	if (vm_err == 0u)
	{
		vm_err = code;
		vm_err_op = op;
	}
}

int64_t fdiv64(int64_t a, int64_t b)
{
	int64_t q = a / b, r = a - q * b;

	return (r != 0l && ((r < 0l) != (b < 0l))) ? q - 1l : q;
}

int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
	y -= (m <= 2l) ? 1l : 0l;
	int64_t era = fdiv64(y, 400l), yoe = y - era * 400l;
	int64_t doy = (153l * (m > 2l ? m - 3l : m + 9l) + 2l) / 5l + d - 1l;
	int64_t doe = yoe * 365l + yoe / 4l - yoe / 100l + doy;

	return era * 146097l + doe - 719468l;
}

void civil_from_days(int64_t z, out int64_t y, out int64_t m, out int64_t d)
{
	z += 719468l;
	int64_t era = fdiv64(z, 146097l), doe = z - era * 146097l;
	int64_t yoe = (doe - doe / 1460l + doe / 36524l - doe / 146096l) / 365l;
	int64_t doy = doe - (365l * yoe + yoe / 4l - yoe / 100l);
	int64_t mp = (5l * doy + 2l) / 153l;

	d = doy - (153l * mp + 2l) / 5l + 1l;
	m = mp < 10l ? mp + 3l : mp - 9l;
	y = yoe + era * 400l + ((m <= 2l) ? 1l : 0l);
}

/* the days since 2000-01-01 of the first instant of the unit that holds the days z (month, quarter, year) */
int64_t trunc_days(int64_t z, uint unit)
{
	int64_t y, m, d;

	civil_from_days(z + 10957l, y, m, d);
	if (unit == 6u)
		m = ((m - 1l) / 3l) * 3l + 1l;
	else if (unit == 7u)
		m = 1l;
	return days_from_civil(y, m, 1l) - 10957l;
}

#define US_DAY 86400000000l

/* the weekday of the days since 2000-01-01, 0 for Sunday; the remainder of a negative dividend is not defined in GLSL, so the floor division gives it */
int64_t dow_of(int64_t days)
{
	int64_t x = days + 10957l + 4l;

	return x - fdiv64(x, 7l) * 7l;
}

/* the operation i of the program at byte-free word offset ob */
void vm_op(uint ob, uint i, uint dtrow, uint row)
{
	uint w0 = ctl[ob], w1 = ctl[ob + 1u], w2 = ctl[ob + 2u];
	uint op = w0 & 255u, ty = (w0 >> 8) & 255u, tt = (w0 >> 16) & 255u, fl = w0 >> 24;
	uint dst = w1 & 255u, a = (w1 >> 8) & 255u, b = (w1 >> 16) & 255u, c = w1 >> 24;
	uint act = w2 & 255u;
	int scale = int(w2 << 8) >> 16;
	int aux = int(ctl[ob + 3u]);
	int64_t imm = int64_t(u64of(ctl[ob + 4u], ctl[ob + 5u]));
	bool lv = live(act);

	if (op == VOP_LOAD_COL)
	{
		uint d = dtrow + a * 8u;
		int64_t v = dc_value(d, row);
		bool ok = dc_valid(d, row);

		if (ty == GT_F64)
			v = bitsd(f8_from_key(v));
		rset(dst, ok ? v : 0l, ok);
	}
	else if (op == VOP_LOAD_CONST)
		rset(dst, imm, true);
	else if (op == VOP_LOAD_NULL)
		rset(dst, 0l, false);
	else if (op == VOP_CAST)
	{
		int64_t v = R[a];
		bool ok = rvalid(a);

		if (tt == GT_F64 && ty != GT_F64)
			rset(dst, bitsd(ty == GT_NUM ? double(v) / double(P10I[scale]) : double(v)), ok);
		else if (tt == GT_NUM && ty != GT_NUM && ty != GT_F64)
		{
			int64_t m = P10I[scale], r = v * m;
			if (ok && lv && v != 0l && (r / m != v))
				verr(VE_INT8, i);
			rset(dst, r, ok);
		}
		else
			rset(dst, v, ok);
	}
	else if (op == VOP_ADD || op == VOP_SUB || op == VOP_MUL || op == VOP_DIV)
	{
		int64_t x = R[a], y = (fl & 1u) != 0u ? imm : R[b];
		bool ok = rvalid(a) && ((fl & 1u) != 0u || rvalid(b));
		int64_t r = 0l;

		if (ty == GT_F64)
		{
			double p = dbl(x), q = dbl(y), z = 0.0lf;
			if (op == VOP_DIV && q == 0.0lf)
			{
				if (lv)
					verr(VE_DIV0, i);
			}
			else
			{
				z = op == VOP_ADD ? p + q : op == VOP_SUB ? p - q : op == VOP_MUL ? p * q : p / q;
				if (lv && isinf(z) && !isinf(p) && !isinf(q))
					verr(VE_FLOAT_OVF, i);
				if (lv && (op == VOP_MUL || op == VOP_DIV) && z == 0.0lf && p != 0.0lf && (op == VOP_DIV || q != 0.0lf))
					verr(VE_FLOAT_UNF, i);
			}
			r = bitsd(z);
		}
		else
		{
			bool bad = false;
			if (op == VOP_ADD)
			{
				r = x + y;
				bad = ((x ^ r) & (y ^ r)) < 0l;
			}
			else if (op == VOP_SUB)
			{
				r = x - y;
				bad = ((x ^ y) & (x ^ r)) < 0l;
			}
			else if (op == VOP_MUL)
			{
				r = x * y;
				bad = (x == -1l && y == I64_MIN) || (y == -1l && x == I64_MIN) || (x != 0l && y != 0l && !(x == -1l || y == -1l) && (r / x != y));
			}
			else if (y == 0l)
			{
				if (lv)
					verr(VE_DIV0, i);
			}
			else if (y == -1l && x == I64_MIN)
				bad = true;
			else
				r = x / y;
			if (ty == GT_I32)
				bad = bad || r < -2147483648l || r > 2147483647l;
			if (bad && lv)
				verr(ty == GT_I32 ? VE_INT4 : VE_INT8, i);
		}
		rset(dst, r, ok);
	}
	else if (op == VOP_NEG || op == VOP_ABS)
	{
		int64_t x = R[a], r;
		bool ok = rvalid(a);

		if (ty == GT_F64)
			r = op == VOP_NEG ? bitsd(-dbl(x)) : bitsd(abs(dbl(x)));
		else
		{
			bool bad = (ty == GT_I32) ? (op == VOP_NEG ? x == -2147483648l : x == -2147483648l) : x == I64_MIN;
			if (op == VOP_ABS && x >= 0l)
				bad = false;
			r = op == VOP_NEG ? -x : (x < 0l ? -x : x);
			if (bad && lv)
				verr(ty == GT_I32 ? VE_INT4 : VE_INT8, i);
		}
		rset(dst, r, ok);
	}
	else if (op == VOP_CMP)
	{
		int64_t x = R[a], y = (fl & 1u) != 0u ? imm : R[b];
		bool ok = rvalid(a) && ((fl & 1u) != 0u || rvalid(b));
		int cr = ty == GT_F64 ? fcmp(dbl(x), dbl(y)) : (x < y ? -1 : (x > y ? 1 : 0));

		mset(dst, cmp_ok(cr, aux), ok);
	}
	else if (op == VOP_IS_NULL)
		mset(dst, aux != 0 ? rvalid(a) : !rvalid(a), true);
	else if (op == VOP_MASK_AND)
	{
		bool ta = mtrue(a), tb = mtrue(b), ka = mknown(a), kb = mknown(b);
		mset(dst, ta && tb, (ka && kb) || (ka && !ta) || (kb && !tb));
	}
	else if (op == VOP_MASK_OR)
	{
		bool ta = mtrue(a), tb = mtrue(b), ka = mknown(a), kb = mknown(b);
		mset(dst, ta || tb, (ka && kb) || (ka && ta) || (kb && tb));
	}
	else if (op == VOP_MASK_NOT)
		mset(dst, mknown(a) && !mtrue(a), mknown(a));
	else if (op == VOP_MASK_COL)
	{
		uint d = dtrow + a * 8u;
		bool ok = dc_valid(d, row);

		mset(dst, ok && dc_value(d, row) != 0l, ok);
	}
	else if (op == VOP_MASK_CONST)
		mset(dst, (fl & 1u) != 0u, (fl & 2u) != 0u);
	else if (op == VOP_INIT_NULL)
		rset(dst, 0l, false);
	else if (op == VOP_COPY)
		rset(dst, R[a], rvalid(a));
	else if (op == VOP_BLEND)
	{
		if (mtrue(c))
			rset(dst, R[a], rvalid(a));
	}
	else if (op == VOP_ACT_COPY)
		mset(dst, mtrue(a), true);
	else if (op == VOP_ACT_AND)
		mset(dst, mtrue(a) && mtrue(b), true);
	else if (op == VOP_ACT_ANDN)
		mset(dst, mtrue(a) && !mtrue(b), true);
	else if (op == VOP_ACT_NOTFALSE)
		mset(dst, mtrue(a) && (mtrue(b) || !mknown(b)), true);
	else if (op == VOP_ACT_NOTVALID)
		mset(dst, mtrue(a) && !rvalid(b), true);
	else if (op == VOP_ACT_UNKNOWN)
		mset(dst, mtrue(a) && !mknown(b), true);
	else if (op == VOP_SKIP_IF_EMPTY)
		;
	else if (op == VOP_RESCALE)
	{
		int64_t x = R[a], m = P10I[min(aux, 18)], r = x * m;
		if (lv && rvalid(a) && x != 0l && (r / m != x || (x == -1l && m == I64_MIN)))
			verr(VE_INT8, i);
		rset(dst, r, rvalid(a));
	}
	else if (op == VOP_DATE_TRUNC || op == VOP_EXTRACT)
	{
		int64_t x = R[a];
		bool ok = rvalid(a);
		bool date = ty == GT_I32;
		int64_t days = date ? x : fdiv64(x, US_DAY);

		if (ok && !date && lv && (x == I64_MAX || x == I64_MIN))
		{
			verr(VE_TIME, i);
			ok = false;
		}
		if (op == VOP_DATE_TRUNC)
		{
			uint u = uint(aux);
			int64_t r;
			if (u <= 3u)
			{
				int64_t per = u == 0u ? 1000000l : u == 1u ? 60000000l : u == 2u ? 3600000000l : US_DAY;
				r = fdiv64(x, per) * per;
			}
			else if (u == 4u)
			{
				int64_t dw = dow_of(days);
				r = (days - ((dw + 6l) % 7l)) * US_DAY;
			}
			else
				r = trunc_days(days, u) * US_DAY;
			rset(dst, r, ok);
		}
		else
		{
			uint f = uint(aux);
			int64_t y, mo, dd, r = 0l;
			civil_from_days(days + 10957l, y, mo, dd);
			int64_t tod = date ? 0l : x - days * US_DAY;
			if (f == 0u) r = y;
			else if (f == 1u) r = (mo - 1l) / 3l + 1l;
			else if (f == 2u) r = mo;
			else if (f == 3u) r = dd;
			else if (f == 4u) r = dow_of(days);
			else if (f == 5u) { r = dow_of(days); if (r == 0l) r = 7l; }
			else if (f == 6u) r = days_from_civil(y, mo, dd) - days_from_civil(y, 1l, 1l) + 1l;
			else if (f == 7u) r = tod / 3600000000l;
			else if (f == 8u) r = (tod / 60000000l) % 60l;
			else r = date ? (days + 10957l) * 86400l : x + 946684800000000l;
			rset(dst, r, ok);
		}
	}
	else if (op == VOP_MASK_BLEND)
	{
		if (mtrue(c))
			mset(dst, mtrue(a), mknown(a));
	}
	else if (op == VOP_MASK_UNKNOWN)
		mset(dst, false, false);
	else if (op == VOP_MASK_ISNULL)
		mset(dst, aux != 0 ? mknown(a) : !mknown(a), true);
	else if (op == VOP_IN_SET)
	{
		int64_t x = R[a];
		uint po = ctl[J_POOL] + ctl[ob + 6u], cnt = ctl[ob + 7u];
		bool hit = false;

		if ((fl & 1u) != 0u)
		{
			uint64_t off = uint64_t(x - imm);
			if (off < uint64_t(cnt) * 32ul)
				hit = ((ctl[po + uint(off >> 5)] >> uint(off & 31ul)) & 1u) != 0u;
		}
		else
		{
			uint l = 0u, h = cnt;
			while (l < h)
			{
				uint m = (l + h) >> 1;
				int64_t v = int64_t(u64of(ctl[po + 2u * m], ctl[po + 2u * m + 1u]));
				if (v < x)
					l = m + 1u;
				else
					h = m;
			}
			if (l < cnt)
				hit = int64_t(u64of(ctl[po + 2u * l], ctl[po + 2u * l + 1u])) == x;
		}
		mset(dst, hit, rvalid(a));
	}
	else
		verr(VE_UNSUP, i);
}

/* the program over one row; false when an error stopped it */
bool vm_run(uint dtrow, uint row)
{
	uint nops = ctl[J_NOPS], ob = ctl[J_OPS];

	RV = 0u;
	MT = 1u;
	MK = 1u;
	vm_err = 0u;
	for (uint i = 0u; i < nops && vm_err == 0u; i++)
		vm_op(ob + i * 8u, i, dtrow, row);
	return vm_err == 0u;
}
