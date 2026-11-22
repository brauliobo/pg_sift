/*
 * common_fast.glsl - the fast kernels: the programs that only read columns (a key or a value of an aggregate is a column as stored) and
 * keep the rows that pass a conjunction of comparisons of a column with a constant (agg/vk/fast.c decides). There is no expression
 * machine: the shape of the job is fixed in the pipeline by specialization constants (the columns, the comparisons, the kind of every
 * aggregate), so that the loops over them unroll and every value lives in a register, and an invocation takes four consecutive rows at a
 * time (one word of a one byte column, one 8 byte load of a two byte column, ...) so that a warp reads whole cache lines.
 * Bit i of a tile mask is the row r + i.
 */
#extension GL_EXT_control_flow_attributes : require
#include "common_gran.glsl"

layout(constant_id = 0) const uint SC_NU = 0u;		/* the distinct columns the job reads (as a value or as a double) */
layout(constant_id = 1) const uint SC_UC0 = 0u;		/* the column slot of each, a nibble each: 0..7 */
layout(constant_id = 2) const uint SC_UC1 = 0u;		/* 8..14 */
layout(constant_id = 3) const uint SC_UF = 0u;		/* bit u: the column is read as a double */
layout(constant_id = 4) const uint SC_NPRED = 0u;
layout(constant_id = 5) const uint SC_PCOL = 0u;	/* the column (index u) of each comparison, a nibble each */
layout(constant_id = 6) const uint SC_POP = 0u;		/* its operator (CMP_*) */
layout(constant_id = 7) const uint SC_NAGG = 0u;
layout(constant_id = 8) const uint SC_AKIND = 0u;	/* the kind of each aggregate, a nibble each */
layout(constant_id = 9) const uint SC_ACOL = 0u;	/* its column (index u), 15 none */
layout(constant_id = 10) const uint SC_NKEYS = 0u;
layout(constant_id = 11) const uint SC_KCOL = 0u;	/* the column (index u) of each key, a nibble each */

#define NU_MAX			15u
#define NO_GROUP		0xFFFFFFFFu
#define FAGG_MAX		8u

uint nib(uint packed, uint i)
{
	return (packed >> (4u * i)) & 15u;
}

#define AGG_KIND(a)		nib(SC_AKIND, (a))

struct FCol
{
	uint off;
	uint vo;
	uint w;
	int64_t base;
};

FCol g_uc[NU_MAX];
int64_t g_v[NU_MAX * 4u];
uint g_vn[NU_MAX];

FCol fcol(uint d)
{
	return FCol(ctl[d], ctl[d + 1u], ctl[d + 4u], int64_t(u64of(ctl[d + 2u], ctl[d + 3u])));
}

/* the columns of the job as the granule gi holds them */
void fast_granule(uint dtrow)
{
	[[unroll]] for (uint u = 0u; u < SC_NU; u++)
		g_uc[u] = fcol(dtrow + (u < 8u ? nib(SC_UC0, u) : nib(SC_UC1, u - 8u)) * 8u);
}

int64_t as_column(int64_t v, bool f64)
{
	return f64 ? bitsd(f8_from_key(v)) : v;
}

/* the four rows r.. of a column whose values are all inside the granule: the values and their validity nibble */
uint load4(FCol c, uint r, bool f64, out int64_t v[4])
{
	uint64_t f[4];
	uint64_t b = uint64_t(c.base);

	if (c.w == 1u)
	{
		uint x = a32[c.off + (r >> 2)];

		f[0] = uint64_t(x & 255u);
		f[1] = uint64_t((x >> 8) & 255u);
		f[2] = uint64_t((x >> 16) & 255u);
		f[3] = uint64_t(x >> 24);
	}
	else if (c.w == 2u)
	{
		uint64_t x = a64[(c.off >> 1) + (r >> 2)];

		f[0] = x & 65535ul;
		f[1] = (x >> 16) & 65535ul;
		f[2] = (x >> 32) & 65535ul;
		f[3] = x >> 48;
	}
	else if (c.w == 4u)
	{
		uint64_t x = a64[(c.off >> 1) + (r >> 1)], y = a64[(c.off >> 1) + (r >> 1) + 1u];

		f[0] = x & 0xFFFFFFFFul;
		f[1] = x >> 32;
		f[2] = y & 0xFFFFFFFFul;
		f[3] = y >> 32;
	}
	else if (c.w == 8u)
	{
		uint o = (c.off >> 1) + r;

		f[0] = a64[o];
		f[1] = a64[o + 1u];
		f[2] = a64[o + 2u];
		f[3] = a64[o + 3u];
	}
	else
		f[0] = f[1] = f[2] = f[3] = 0ul;
	[[unroll]] for (uint i = 0u; i < 4u; i++)
		v[i] = as_column(int64_t(b + f[i]), f64);
	if (c.vo == DC_ALL_VALID)
		return 15u;
	if (c.vo == DC_ALL_NULL)
		return 0u;
	return (a32[c.vo + (r >> 5)] >> (r & 31u)) & 15u;
}

/* the last tile of a granule whose rows are not a multiple of four: row by row, no read past the column */
uint load4_tail(FCol c, uint r, bool f64, out int64_t v[4])
{
	uint nb = 0u;

	for (uint i = 0u; i < 4u; i++)
	{
		uint rr = r + i;
		uint64_t f = 0ul;

		v[i] = 0l;
		if (rr >= g_nrows)
			continue;
		if (c.w == 1u)
			f = uint64_t(a8[c.off * 4u + rr]);
		else if (c.w == 2u)
			f = uint64_t(a16[c.off * 2u + rr]);
		else if (c.w == 4u)
			f = uint64_t(a32[c.off + rr]);
		else if (c.w == 8u)
			f = a64[(c.off >> 1) + rr];
		v[i] = as_column(int64_t(uint64_t(c.base) + f), f64);
		if (c.vo == DC_ALL_VALID || (c.vo != DC_ALL_NULL && ((a32[c.vo + (rr >> 5)] >> (rr & 31u)) & 1u) != 0u))
			nb |= 1u << i;
	}
	return nb;
}

uint tile_load(uint u, uint r, bool full)
{
	int64_t t[4];
	bool f64 = ((SC_UF >> u) & 1u) != 0u;
	uint nb = full ? load4(g_uc[u], r, f64, t) : load4_tail(g_uc[u], r, f64, t);

	[[unroll]] for (uint i = 0u; i < 4u; i++)
		g_v[u * 4u + i] = t[i];
	return nb;
}

/* loads the columns of the tile at row r (r < g_nrows, a multiple of four) and returns its rows that are live and pass the comparisons */
uint fast_tile(uint r)
{
	bool full = r + 4u <= g_nrows;
	uint pass = full ? 15u : (1u << (g_nrows - r)) - 1u;

	if (g_dead != DC_ALL_VALID)
		pass &= ~((ctl[ctl[J_DEAD] + g_dead + (r >> 5)] >> (r & 31u)) & 15u);
	[[unroll]] for (uint u = 0u; u < SC_NU; u++)
		g_vn[u] = tile_load(u, r, full);
	[[unroll]] for (uint p = 0u; p < SC_NPRED; p++)
	{
		uint u = nib(SC_PCOL, p), ok = 0u;
		int op = int(nib(SC_POP, p));
		int64_t imm = int64_t(u64of(ctl[J_FAST_IMM + 2u * p], ctl[J_FAST_IMM + 2u * p + 1u]));
		bool f64 = ((SC_UF >> u) & 1u) != 0u;

		[[unroll]] for (uint i = 0u; i < 4u; i++)
		{
			int64_t x = g_v[u * 4u + i];
			int c = f64 ? fcmp(dbl(x), dbl(imm)) : (x < imm ? -1 : (x > imm ? 1 : 0));

			ok |= cmp_ok(c, op) ? (1u << i) : 0u;
		}
		pass &= ok & g_vn[u];
	}
	return pass;
}

/* the rows of the tile whose value of aggregate a is not NULL */
uint agg_rows(uint a, uint pass)
{
	uint u = nib(SC_ACOL, a);

	return u == 15u ? pass : pass & g_vn[u];
}

bool agg_valid(uint a, uint i)
{
	uint u = nib(SC_ACOL, a);

	return u == 15u || ((g_vn[u] >> i) & 1u) != 0u;
}

int64_t agg_value(uint a, uint i)
{
	return g_v[nib(SC_ACOL, a) * 4u + i];
}

/* the order-preserving key the MIN or MAX of aggregate a compares */
int64_t agg_cmp_key(uint a, uint k, uint i)
{
	int64_t v = agg_value(a, i);

	return (k == AK_MIN_F || k == AK_MAX_F) ? f8_key(dbl(v)) : v;
}

/* the group of row i of the tile (the dense key of the job: the keys of row i, each a slot of its span, the last the slot of NULL), NO_GROUP when a key is out of the range of the job */
uint row_group(uint i)
{
	uint gid = 0u;

	[[unroll]] for (uint j = 0u; j < SC_NKEYS; j++)
	{
		uint u = nib(SC_KCOL, j), span = ctl[J_DENSE_SPAN + j], slot = span - 1u;

		if (((g_vn[u] >> i) & 1u) != 0u)
		{
			int64_t kmin = int64_t(u64of(ctl[J_DENSE_KMIN + 2u * j], ctl[J_DENSE_KMIN + 2u * j + 1u]));
			uint64_t d = uint64_t(g_v[u * 4u + i] - kmin);

			if (d >= uint64_t(span - 1u))
				return NO_GROUP;
			slot = uint(d);
		}
		gid = gid * span + slot;
	}
	return gid;
}
