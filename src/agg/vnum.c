/*
 * agg/vnum.c - sum and avg of a numeric program (SiftAggNum: + - * of numeric(p <= 18, s), int2, int4 and int8 leaves and constants) on int64 vectors.
 *
 * The program of agg/leaf.c (eval_nx) computes every value in int128, one tuple at a time through a stack of int128 vectors. Here the same program runs on
 * blocks of VN_BLK tuples as int64 vectors by the kernels of agg/kern.h (lin_i64, mulw_i64: no 64-bit vector multiply exists on NEON or AVX2, so a multiplied
 * operand must fit int32) and sums per group by sum_gid_i64. What makes it exact is a proof for each block, from the data: the largest absolute value of each leaf
 * (absmax_i64) is carried through the steps as an upper bound; the block is run in int64 only when every multiplied operand is below 2^31 and no value can reach 2^62.
 * Any other block (large values, a NaN, which is INT64_MAX in a leaf) goes through eval_nx, which the planner proved exact for the program's declared digits.
 * The values of a NULL leaf are undefined: they only make the bound larger (the tuple is dropped by ok), and the arithmetic wraps, so the result is the same
 * wherever the bound holds.
 *
 * The sums are added with agg_sum_add (w and a carry count), as the interpreter's are.
 */
#include "postgres.h"

#include "col/kern.h"
#include "agg/agg_int.h"
#include "agg/leaf_int.h"
#include "agg/vnum.h"

#define VN_BLK		512
#define VN_NBLK		(SIFT_AGG_VEC / VN_BLK)
#define VN_STK		SIFT_AGG_MAX_VX
#define VN_MAXG		64					/* groups up to which a block sums into four private copies, folded once per chunk */
#define VN_LIM		(UINT64CONST(1) << 62)
#define VN_NARROW	((uint64) INT32_MAX)

typedef unsigned __int128 u128;

/*
 * The leaves of the chunk, read once for all the aggregates that use them (Q1 shape: seven sums over four leaves): the values of the selected tuples
 * (the column itself when they are consecutive, else gathered) and the bound of each block, found when first asked.
 */
typedef struct VnLeaf
{
	uint32		gen;
	const int64 *v;
	int64	   *buf;
	uint64		bound[VN_NBLK];		/* UINT64_MAX: not computed */
} VnLeaf;

struct VnCache
{
	uint32		gen;
	int			nleaves;
	VnLeaf	   *leaf;
};

void
vnum_chunk_reset(AggExec *ex)
{
	if (ex->vn)
		ex->vn->gen++;
}

static VnLeaf *
vn_leaf(AggExec *ex, int leaf, int nsel)
{
	struct VnCache *c = ex->vn;
	VnLeaf	   *l;
	const SiftSrcVec *v;
	const uint32 *idx;
	uint32		diff = 0;

	if (c == NULL)
	{
		c = ex->vn = MemoryContextAllocZero(ex->cxt, sizeof(*c));
		c->nleaves = ex->lay->nleaves;
		c->leaf = MemoryContextAllocZero(ex->cxt, c->nleaves * sizeof(VnLeaf));
		c->gen = 1;
	}
	l = &c->leaf[leaf];
	if (l->gen == c->gen)
		return l;
	v = get_vec(ex, leaf);
	idx = leaf_lvl(ex, leaf) == ex->qlevel ? ex->tidx : ex->ridx;
	if (l->buf == NULL)
		l->buf = MemoryContextAlloc(ex->cxt, (SIFT_AGG_VEC + SIFT_AGG_PAD / 8) * sizeof(int64));
	for (int j = 0; j < nsel; j++)
		diff |= idx[j] ^ (idx[0] + j);
	if (ex->lay->leaf[leaf].vtype == SCV_I32)
	{
		for (int j = 0; j < nsel; j++)
			l->buf[j] = ((const int32 *) v->data)[idx[j]];
		l->v = l->buf;
	}
	else if (diff == 0)
		l->v = (const int64 *) v->data + idx[0];
	else
	{
		sift_colk->for_i64_sel[SIFT_COLW_64](v->data, idx, nsel, 0, l->buf);
		l->v = l->buf;
	}
	for (int b = 0; b < VN_NBLK; b++)
		l->bound[b] = UINT64_MAX;
	l->gen = c->gen;
	return l;
}

bool
vnum_program_ok(const SiftAggNum *p)
{
	int			sp = 0;

	for (int k = 0; k < p->n; k++)
	{
		const SiftAggNx *x = &p->step[k];

		switch (x->op)
		{
			case SNX_LEAF:
				sp++;
				break;
			case SNX_CONST:
				if (x->c < -(sift_i128) PG_INT64_MAX || x->c > PG_INT64_MAX)
					return false;
				sp++;
				break;
			case SNX_ADD:
			case SNX_SUB:
			case SNX_MUL:
				sp--;
				break;
			case SNX_NEG:
				break;
			default:
				return false;
		}
		if (sp > VN_STK)
			return false;
	}
	return sp == 1;
}

/* the bound of a * k as a term of a sum: k is 1 or -1, or the operand is multiplied (it must fit int32, and so must k) */
static inline bool
term_bound(uint64 bound, sift_i128 k, u128 *out)
{
	if (k == 1 || k == -1)
		*out = bound;
	else
	{
		if (bound > VN_NARROW || k > INT32_MAX || k < -(sift_i128) INT32_MAX)
			return false;
		*out = (u128) bound * (uint64) (k < 0 ? -k : k);
	}
	return true;
}

/*
 * The values of the program for the cnt tuples at iq / ir (offsets of this block) in stk[0], and a bound of their absolute value. false: the proof failed, nothing
 * is known about the contents. A leaf whose rows are consecutive is read where it is (ptr), the others are gathered into the slot.
 */
static bool
vn_block(AggExec *ex, const SiftAggNum *p, int nsel, int off, int64 (*stk)[VN_BLK], const int64 **res, uint64 *res_bound)
{
	const int64 *ptr[VN_STK];
	uint64		bnd[VN_STK];
	int			sp = 0;
	int			cnt = Min(VN_BLK, nsel - off);

	for (int k = 0; k < p->n; k++)
	{
		const SiftAggNx *x = &p->step[k];
		u128		ba,
					bb;

		switch (x->op)
		{
			case SNX_LEAF:
				{
					VnLeaf	   *l = vn_leaf(ex, x->leaf, nsel);

					ptr[sp] = l->v + off;
					if (l->bound[off / VN_BLK] == UINT64_MAX)
						l->bound[off / VN_BLK] = sift_aggk->absmax_i64(ptr[sp], cnt);
					bnd[sp] = l->bound[off / VN_BLK];
					if (bnd[sp] > VN_LIM)
						return false;
					sp++;
				}
				break;
			case SNX_CONST:
				if (x->c > (sift_i128) VN_LIM || x->c < -(sift_i128) VN_LIM)
					return false;
				for (int j = 0; j < cnt; j++)
					stk[sp][j] = (int64) x->c;
				ptr[sp] = stk[sp];
				bnd[sp] = x->c < 0 ? (uint64) -(int64) x->c : (uint64) x->c;
				sp++;
				break;
			case SNX_ADD:
			case SNX_SUB:
				{
					sift_i128	ka = sift_pow10(x->a),
								kb = x->op == SNX_ADD ? sift_pow10(x->b) : -sift_pow10(x->b);

					if (!term_bound(bnd[sp - 2], ka, &ba) || !term_bound(bnd[sp - 1], kb, &bb) || ba + bb > VN_LIM)
						return false;
					sift_aggk->lin_i64(stk[sp - 2], ptr[sp - 2], (int32) ka, ptr[sp - 1], (int32) kb, cnt);
					ptr[sp - 2] = stk[sp - 2];
					bnd[sp - 2] = (uint64) (ba + bb);
					sp--;
				}
				break;
			case SNX_MUL:
				if (bnd[sp - 2] > VN_NARROW || bnd[sp - 1] > VN_NARROW)
					return false;
				sift_aggk->mulw_i64(stk[sp - 2], ptr[sp - 2], ptr[sp - 1], cnt);
				ptr[sp - 2] = stk[sp - 2];
				bnd[sp - 2] = bnd[sp - 2] * bnd[sp - 1];
				sp--;
				break;
			default:			/* SNX_NEG */
				sift_aggk->lin_i64(stk[sp - 1], ptr[sp - 1], -1, ptr[sp - 1], 0, cnt);
				ptr[sp - 1] = stk[sp - 1];
				break;
		}
	}
	*res = ptr[0];
	*res_bound = bnd[0];
	return true;
}

static void
fold(AggAcc *acc, int64 *part, uint32 ng)
{
	for (uint32 g = 0; g < ng; g++)
		for (int c = 0; c < 4; c++)
			agg_sum_add(acc, g, part[c * ng + g]);
	memset(part, 0, 4 * (Size) ng * sizeof(int64));
}

void
vnum_sum_update(AggExec *ex, int ai, const SiftAggNum *p, int nsel, const uint64 *ok)
{
	AggAcc	   *acc = &ex->acc[ai];
	uint32		ng = ex->hash ? ex->ngroups : ex->gcap;
	int64		stk[VN_STK][VN_BLK] pg_attribute_aligned(64);
	int64		part[4 * VN_MAXG] = {0};
	u128		pbound = 0;
	bool		use_part = ng <= VN_MAXG;

	for (int off = 0; off < nsel; off += VN_BLK)
	{
		int			cnt = Min(VN_BLK, nsel - off);
		const uint64 *okb = ok ? ok + off / 64 : NULL;
		const uint32 *gid = ex->gid + off;
		const int64 *x;
		uint64		bound;

		if (vn_block(ex, p, nsel, off, stk, &x, &bound))
		{
			/* a block whose sum may not fit int64 is added to the groups as it is (a bound of 2^54 on 512 values, never reached by a sum of prices) */
			if (ex->spec->nkeys == 0 && (u128) bound * cnt < (u128) 1 << 62)
				agg_sum_add(acc, 0, sift_aggk->sum_i64(x, okb, cnt));	/* one group: the sum of the block, not a scatter through the group ids */
			else if (use_part && (u128) bound * cnt < (u128) 1 << 62)
			{
				if (pbound + (u128) bound * cnt >= (u128) 1 << 63)
				{
					fold(acc, part, ng);
					pbound = 0;
				}
				pbound += (u128) bound * cnt;
				sift_aggk->sum_gid_i64(part, ng, gid, x, okb, cnt);
			}
			else
				for (int j = 0; j < cnt; j++)
					if (!okb || sift_colsrc_bit(okb, j))
						agg_sum_add(acc, gid[j], x[j]);
		}
		else
		{
			const sift_i128 *nx = eval_nx(ex, p, cnt, ex->tidx + off, ex->ridx + off, NULL, okb);

			for (int j = 0; j < cnt; j++)
			{
				uint32		g = gid[j];

				if (okb && !sift_colsrc_bit(okb, j))
					continue;
				if (nx[j] == SIFT_NX_NAN)
					acc->nf[g] = 1;
				else
					agg_sum_add(acc, g, nx[j]);
			}
		}
	}
	if (use_part)
		fold(acc, part, ng);
}
