/*
 * agg/par/split.c - the groups of a parallel scan split by a key
 * The participants of a scan of many groups do not split the rows: each reads every row of the units and aggregates the rows whose key falls in its
 * share, so that the tables are disjoint, small enough to stay in the cache, and the leader only adds them to its own (SiftAggSpec.gpart).
 * A row is aggregated by exactly one participant whatever the share its key hashes to (the shares are of the stored value, the same in every
 * process), so a key that two shares both hold would only be merged; the shares keep the groups apart, they do not make the result right.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "optimizer/optimizer.h"

#include "agg/par/int.h"

/* participants, the leader included: the shares are bits of a word */
#define SPLIT_MAX	32

/* the shape of a scan whose rows each participant can take a share of by a key alone (the paths that answer a unit or a group without its rows are not shares of rows) */
bool
split_shape_ok(const SiftAggSpec *s)
{
	if (s->nkeys == 0 || s->nsets > 0 || s->narms > 0 || s->spine || s->topn || s->group >= 0 || s->nparts > 0 || s->left_lateral || s->search ||
		s->nworkers + 1 > SPLIT_MAX)
		return false;
	for (int ji = 0; ji < s->njoin; ji++)
		if (s->join[ji].params != NIL)
			return false;
	return true;
}

/* the values that are equal when their bytes are, and a word each: an integer, a date or a timestamp (a text key would be hashed for every row of every participant: 16,000 rows a unit, a dictionary of its own) */
bool
split_key_ok(const SiftAggSpec *s, int j, const SiftSrcLeaf *lf)
{
	const SiftAggKey *k = &s->key[j];

	if (k->kind != SAK_LEAF || lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->wide || lf->lossy)
		return false;
	switch (lf->typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return lf->vtype == SCV_I32 || lf->vtype == SCV_I64;
	}
	return false;
}

/* may the scan split the groups by the key the plan chose: the spec still fits, and no unit is answered without its rows */
bool
split_run_ok(const AggExec *ex)
{
	int			j = ex->spec->gpart - 1;

	return j >= 0 && split_shape_ok(ex->spec) && parallel_leader_participation && !ex->pa_on && !ex->pcount && ex->fuse == NULL && !ex->topn &&
		split_key_ok(ex->spec, j, &ex->lay->leaf[ex->spec->key[j].leaf]);
}

/* the share of a 32-bit hash: its high bits scaled to [0, n) without a division */
static inline uint32
share_of(uint32 h, uint32 n)
{
	return (uint32) (((uint64) (h * 0x9E3779B1u) * n) >> 32);
}

#if defined(__x86_64__) && defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
#define SPLIT_CLONES __attribute__((target_clones("avx2", "default")))
#else
#define SPLIT_CLONES
#endif

/*
 * The rows of the 64 from `d` (the vectors are padded past their end) that are in the share, as bits. A process with one share tests the hash against
 * the range of hashes that share is (share_of is monotonic: the share of h is s when lo <= h * K < lo + w), in loops of fixed length that the
 * compiler vectorizes; the flags (a 0/1 byte a row) become bits eight at a time, by one multiplication.
 */
static inline uint64
pack_flags(const uint8 *flag)
{
	uint64		keep = 0;

	for (int b = 0; b < 64; b += 8)
	{
		uint64		x;

		memcpy(&x, flag + b, sizeof(x));
		keep |= ((x * UINT64CONST(0x0102040810204080)) >> 56) << b;
	}
	return keep;
}

SPLIT_CLONES static uint64
word_i32(const int32 *d, uint32 lo, uint32 w)
{
	uint8		flag[64];

	for (int b = 0; b < 64; b++)
		flag[b] = ((uint32) d[b] * 0x9E3779B1u - lo) < w;
	return pack_flags(flag);
}

SPLIT_CLONES static uint64
word_i64(const int64 *d, uint32 lo, uint32 w)
{
	uint8		flag[64];

	for (int b = 0; b < 64; b++)
		flag[b] = (((uint32) d[b] ^ (uint32) ((uint64) d[b] >> 32)) * 0x9E3779B1u - lo) < w;
	return pack_flags(flag);
}

/* the same for the shares of a process that has several (the workers that did not start): by the share of each row */
static uint64
word_shares(const SiftSrcVec *v, bool wide, int at, uint32 n, uint32 mine)
{
	uint8		flag[64];

	for (int b = 0; b < 64; b++)
	{
		uint32		h = wide ? (uint32) ((const int64 *) v->data)[at + b] ^ (uint32) ((uint64) ((const int64 *) v->data)[at + b] >> 32) : (uint32) ((const int32 *) v->data)[at + b];

		flag[b] = (mine >> share_of(h, n)) & 1;
	}
	return pack_flags(flag);
}

/* The shares of this process (gp_mine) out of the n of the scan: the leader's is 0, a worker's its number + 1; a NULL key is the leader's */
void
split_begin(AggExec *ex, int key, uint32 n, uint32 mine)
{
	ex->gp_leaf = ex->spec->key[key].leaf;
	ex->gp_n = n;
	ex->gp_mine = mine;
	/* one share: the hashes in it are [lo, lo + w), the first hash of share s being ceil(s * 2^32 / n) */
	ex->gp_one = (mine & (mine - 1)) == 0;
	if (ex->gp_one)
	{
		uint32		s = (uint32) __builtin_ctz(mine);
		uint64		lo = ((((uint64) s) << 32) + n - 1) / n,
					hi = ((((uint64) s + 1) << 32) + n - 1) / n;

		ex->gp_lo = (uint32) lo;
		ex->gp_w = (uint32) (hi - lo);
	}
}

/* Clears from the rows of a unit those whose key is in the share of another process; a NULL key is in the first */
void
split_rows(AggExec *ex, uint64 *rmask, int nrow)
{
	const SiftSrcLeaf *lf = &ex->lay->leaf[ex->gp_leaf];
	const SiftSrcVec *v = get_vec(ex, ex->gp_leaf);

	for (int w = 0; w < (nrow + 63) / 64; w++)
	{
		uint64		keep;

		if (rmask[w] == 0)
			continue;
		if (!ex->gp_one)
			keep = word_shares(v, lf->vtype == SCV_I64, w * 64, ex->gp_n, ex->gp_mine);
		else if (lf->vtype == SCV_I32)
			keep = word_i32((const int32 *) v->data + w * 64, ex->gp_lo, ex->gp_w);
		else
			keep = word_i64((const int64 *) v->data + w * 64, ex->gp_lo, ex->gp_w);
		if (v->isnull)
		{
			uint64		nulls = 0;

			for (int b = 0; b < 64; b++)
				nulls |= (uint64) (v->isnull[w * 64 + b] != 0) << b;
			keep = (keep & ~nulls) | ((ex->gp_mine & 1) ? nulls : 0);
		}
		rmask[w] &= keep;
	}
}
