/*
 * agg/sip.c - the runtime join filter of agg/sip.h: the build, the finalize that decides which of the range, the exact keys and the Bloom filter
 * prune, and the two probes (a zone, a batch of keys). The Bloom filter is core/sbbf.c; the batch probe runs the slots of the active level of
 * agg/kern6.h, so that it is the same code, bit for bit, at every level.
 */
#include "postgres.h"

#include "miscadmin.h"

#include "agg/kern6_int.h"
#include "agg/sip.h"

uint64
sift_sip_budget_bytes(void)
{
	return Max(Min((uint64) SIFT_SIP_MAX_BYTES, (uint64) work_mem * 1024 / 8), (uint64) SIFT_SIP_MIN_BLOCKS * SIFT_SBBF_BLOCK_BYTES);
}

uint32
sift_sip_blocks_for(uint64 est_ndv, uint64 max_bytes)
{
	const uint32 max_blocks = pg_prevpower2_32((uint32) Max(Min(max_bytes / SIFT_SBBF_BLOCK_BYTES, UINT32_C(1) << 31), SIFT_SIP_MIN_BLOCKS));
	const uint32 want = sift_sbbf_blocks_for(est_ndv, SIFT_SIP_BITS_PER_KEY);

	return Min(Max(want > UINT32_C(1) << 31 ? UINT32_C(1) << 31 : pg_nextpower2_32(want), SIFT_SIP_MIN_BLOCKS), max_blocks);
}

Size
sift_sip_size(uint32 n_blocks)
{
	return add_size(SIFT_SIP_HDR_BYTES, mul_size(n_blocks, SIFT_SBBF_BLOCK_BYTES));
}

SiftSipFilter *
sift_sip_init_at(void *raw, SiftVecType key_type, uint32 n_blocks)
{
	SiftSipFilter *sip = raw;

	Assert(key_type == SVEC_I32 || key_type == SVEC_I64);
	Assert(((uintptr_t) raw) % SIFT_SBBF_BLOCK_BYTES == 0);
	memset(sip, 0, sift_sip_size(n_blocks));
	sip->key_type = key_type;
	sip->n_blocks = n_blocks;
	sip->min_key = INT64_MAX;
	sip->max_key = INT64_MIN;
	sip->exact_open = true;
	return sip;
}

SiftSipFilter *
sift_sip_create(MemoryContext cxt, SiftVecType key_type, uint64 est_ndv, uint64 max_bytes)
{
	const uint32 n_blocks = sift_sip_blocks_for(est_ndv, max_bytes);

	return sift_sip_init_at(MemoryContextAllocAligned(cxt, sift_sip_size(n_blocks), SIFT_SBBF_BLOCK_BYTES, 0), key_type, n_blocks);
}

void
sift_sip_free(SiftSipFilter *sip)
{
	pfree(sip);
}

static void
exact_add(SiftSipFilter *sip, int64 key)
{
	int			lo = 0,
				hi = sip->n_exact;

	while (lo < hi)
	{
		const int	mid = (lo + hi) / 2;

		if (sip->exact_keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < sip->n_exact && sip->exact_keys[lo] == key)
		return;
	if (sip->n_exact == SIFT_SIP_EXACT_MAX)
	{
		sip->exact_open = false;
		return;
	}
	memmove(&sip->exact_keys[lo + 1], &sip->exact_keys[lo], (sip->n_exact - lo) * sizeof(int64));
	sip->exact_keys[lo] = key;
	sip->n_exact++;
}

void
sift_sip_insert_i64_hashed(SiftSipFilter *sip, int64 key, uint64 hash)
{
	Assert(!sip->hash_only && !(sift_sip_flags(sip) & SIFT_SIP_FLAG_READY));
	sip->min_key = Min(sip->min_key, key);
	sip->max_key = Max(sip->max_key, key);
	if (sip->exact_open)
		exact_add(sip, key);
	sip->n_inserted++;
	sift_sbbf_insert(sift_sip_blocks(sip), sip->n_blocks, hash);
}

void
sift_sip_insert_i64(SiftSipFilter *sip, int64 key)
{
	sift_sip_insert_i64_hashed(sip, key, sift_aggk6_hash1((uint64) key));
}

void
sift_sip_insert_hash(SiftSipFilter *sip, uint64 hash)
{
	Assert(!(sift_sip_flags(sip) & SIFT_SIP_FLAG_READY));
	sip->hash_only = true;
	sip->exact_open = false;
	sip->n_inserted++;
	sift_sbbf_insert(sift_sip_blocks(sip), sip->n_blocks, hash);
}

void
sift_sip_insert_vec(SiftSipFilter *sip, const SiftVec *keys, const uint64 *mask)
{
	Assert(keys->dense && keys->type == sip->key_type);
	for (uint32 r = 0; r < keys->len; r++)
	{
		if (mask && !((mask[r >> 6] >> (r & 63)) & 1))
			continue;
		if (!keys->all_valid && !((keys->valid[r >> 6] >> (r & 63)) & 1))
			continue;
		sift_sip_insert_i64(sip, keys->type == SVEC_I32 ? (int64) ((const int32 *) keys->data)[r] : ((const int64 *) keys->data)[r]);
	}
}

double
sift_sip_fpr(const SiftSipFilter *sip)
{
	double		fpr = 0;

	if ((sift_sip_flags(sip) & (SIFT_SIP_FLAG_READY | SIFT_SIP_FLAG_HAS_EXACT)) == (SIFT_SIP_FLAG_READY | SIFT_SIP_FLAG_HAS_EXACT))
		return 0;
	for (uint32 b = 0; b < sip->n_blocks; b++)
	{
		double		p = 1;

		for (int j = 0; j < 8; j++)
			p *= (double) sift_pc64(sift_sip_blocks(sip)[b].w[j]) / 32;
		fpr += p;
	}
	return fpr / sip->n_blocks;
}

void
sift_sip_finalize(SiftSipFilter *sip)
{
	uint32		flags = SIFT_SIP_FLAG_READY;

	if (sip->n_inserted == 0)
		flags |= SIFT_SIP_FLAG_EMPTY;
	else
	{
		if (!sip->hash_only)
			flags |= SIFT_SIP_FLAG_HAS_RANGE;
		if (sip->exact_open)
			flags |= SIFT_SIP_FLAG_HAS_EXACT;
		flags |= sift_sip_fpr(sip) <= SIFT_SIP_MAX_FPR ? SIFT_SIP_FLAG_HAS_BLOOM : SIFT_SIP_FLAG_BLOOM_OFF;
	}
	__atomic_store_n(&sip->state_flags, flags, __ATOMIC_RELEASE);
}

static void
atomic_min(int64 *p, int64 v)
{
	int64		cur = __atomic_load_n(p, __ATOMIC_RELAXED);

	while (v < cur && !__atomic_compare_exchange_n(p, &cur, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static void
atomic_max(int64 *p, int64 v)
{
	int64		cur = __atomic_load_n(p, __ATOMIC_RELAXED);

	while (v > cur && !__atomic_compare_exchange_n(p, &cur, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

void
sift_sip_merge_atomic(SiftSipFilter *dst, const SiftSipFilter *src)
{
	SiftSbbfBlock *d = sift_sip_blocks(dst);
	const SiftSbbfBlock *b_src = sift_sip_blocks(src);

	Assert(dst->n_blocks == src->n_blocks);
	for (uint32 b = 0; b < src->n_blocks; b++)
		for (int j = 0; j < 8; j++)
			if (b_src[b].w[j])
				__atomic_fetch_or(&d[b].w[j], b_src[b].w[j], __ATOMIC_RELAXED);
	atomic_min(&dst->min_key, src->min_key);
	atomic_max(&dst->max_key, src->max_key);
	__atomic_fetch_add(&dst->n_inserted, src->n_inserted, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->exact_open, false, __ATOMIC_RELAXED);
	if (src->hash_only)
		__atomic_store_n(&dst->hash_only, true, __ATOMIC_RELAXED);
}

static inline bool
count_prune(SiftSipStats *st, bool keep)
{
	if (st)
	{
		st->granules_probed++;
		st->granules_pruned += !keep;
	}
	return keep;
}

bool
sift_sip_probe_granule(const SiftSipFilter *sip, int64 zmin, int64 zmax, SiftSipStats *st)
{
	const uint32 f = sift_sip_flags(sip);

	Assert(zmin <= zmax);
	if (!(f & SIFT_SIP_FLAG_READY))
		return true;
	if (f & SIFT_SIP_FLAG_EMPTY)
		return count_prune(st, false);
	if ((f & SIFT_SIP_FLAG_HAS_RANGE) && (zmax < sip->min_key || zmin > sip->max_key))
		return count_prune(st, false);
	if (f & SIFT_SIP_FLAG_HAS_EXACT)
	{
		int			lo = 0,
					hi = sip->n_exact;

		while (lo < hi)
		{
			const int	mid = (lo + hi) / 2;

			if (sip->exact_keys[mid] < zmin)
				lo = mid + 1;
			else
				hi = mid;
		}
		return count_prune(st, lo < sip->n_exact && sip->exact_keys[lo] <= zmax);
	}
	if (zmin == zmax && (f & SIFT_SIP_FLAG_HAS_BLOOM))
		return count_prune(st, sift_sbbf_probe_scalar(sift_sip_blocks(sip), sip->n_blocks, sift_aggk6_hash1((uint64) zmin)));
	return count_prune(st, true);
}

static uint16
rows_left(uint64 *io_mask, uint16 n, const uint64 *keep)
{
	return sift_aggk6->mask_and(io_mask, io_mask, keep, n);
}

static uint16
rows_of(const uint64 *mask, uint16 n)
{
	uint32		pc = 0;

	for (int w = 0; w < AK6_NW(n); w++)
		pc += (uint32) sift_pc64(mask[w]);
	return (uint16) pc;
}

static uint16
count_rows(SiftSipStats *st, uint16 in, uint16 out)
{
	if (st)
	{
		st->rows_in += in;
		st->rows_out += out;
	}
	return out;
}

uint16
sift_sip_probe_vec(const SiftSipFilter *sip, const SiftVec *keys, uint64 *io_mask, SiftSipStats *st)
{
	const uint16 n = keys->len;
	const uint32 f = sift_sip_flags(sip);
	const SiftAggKernelsV6 *k = sift_aggk6;
	int64		wide[SIFT_VEC_LEN];
	const int64 *v = keys->data;
	uint16		in = rows_of(io_mask, n),
				left;

	Assert(keys->dense && keys->type == sip->key_type && n <= SIFT_VEC_LEN && !sip->hash_only);
	if (!keys->all_valid)
		in = rows_left(io_mask, n, keys->valid);
	if (!(f & SIFT_SIP_FLAG_READY))
		return count_rows(st, in, in);
	if (f & SIFT_SIP_FLAG_EMPTY)
	{
		memset(io_mask, 0, AK6_NW(n) * sizeof(uint64));
		return count_rows(st, in, 0);
	}
	if (keys->type == SVEC_I32)
	{
		for (uint16 r = 0; r < n; r++)
			wide[r] = ((const int32 *) keys->data)[r];
		v = wide;
	}
	if (f & SIFT_SIP_FLAG_HAS_EXACT)
	{
		uint64		hit[SIFT_VEC_WORDS];

		k->in_set_i64(hit, v, n, sip->exact_keys, sip->n_exact);
		left = rows_left(io_mask, n, hit);
	}
	else if (f & SIFT_SIP_FLAG_HAS_BLOOM)
		left = k->sbbf_probe_vec_i64(sift_sip_blocks(sip), sip->n_blocks, sip->min_key, sip->max_key, v, io_mask, n);
	else
	{
		uint64		ge[SIFT_VEC_WORDS],
					le[SIFT_VEC_WORDS];

		k->cmp_i64(ge, v, n, SCMP_GE, sip->min_key);
		k->cmp_i64(le, v, n, SCMP_LE, sip->max_key);
		k->mask_and(ge, ge, le, n);
		left = rows_left(io_mask, n, ge);
	}
	return count_rows(st, in, left);
}

uint16
sift_sip_probe_hashes(const SiftSipFilter *sip, const uint64 *hashes, uint64 *io_mask, uint16 n, SiftSipStats *st)
{
	const uint32 f = sift_sip_flags(sip);
	const uint16 in = rows_of(io_mask, n);

	if (!(f & SIFT_SIP_FLAG_READY))
		return count_rows(st, in, in);
	if (f & SIFT_SIP_FLAG_EMPTY)
	{
		memset(io_mask, 0, AK6_NW(n) * sizeof(uint64));
		return count_rows(st, in, 0);
	}
	if (!(f & SIFT_SIP_FLAG_HAS_BLOOM))
		return count_rows(st, in, in);
	return count_rows(st, in, sift_aggk6->sbbf_probe_vec_hash(sift_sip_blocks(sip), sip->n_blocks, hashes, io_mask, n));
}

uint16
sift_sip_probe_vec_adapt(const SiftSipFilter *sip, SiftSipProbeState *ps, const SiftVec *keys, uint64 *io_mask)
{
	const uint16 in = rows_of(io_mask, keys->len);
	uint16		left;
	double		hit;

	if (ps->vec_disabled && !ps->reprobing)
		return in;
	left = sift_sip_probe_vec(sip, keys, io_mask, &ps->stats);
	hit = in > 0 ? (double) left / in : 1.0;
	ps->ewma_hit_rate = ps->batches_probed == 0 ? hit : ps->ewma_hit_rate + SIFT_SIP_EWMA_ALPHA * (hit - ps->ewma_hit_rate);
	ps->batches_probed++;
	if (ps->reprobing)
	{
		ps->reprobe_in += in;
		ps->reprobe_out += left;
	}
	else if (ps->batches_probed >= SIFT_SIP_ADAPT_WARMUP_BATCHES && ps->ewma_hit_rate > SIFT_SIP_DISABLE_HIT_RATE)
	{
		ps->vec_disabled = true;
		ps->granules_skipped_probe = 0;
		ps->auto_disables++;
		if (ps->op)
			ps->op->disabled = true;
	}
	return left;
}

void
sift_sip_adapt_granule_end(SiftSipProbeState *ps)
{
	if (ps->reprobing)
	{
		const double hit = ps->reprobe_in > 0 ? (double) ps->reprobe_out / ps->reprobe_in : 1.0;

		ps->reprobing = false;
		ps->reprobe_in = ps->reprobe_out = 0;
		if (hit < SIFT_SIP_REENABLE_HIT_RATE)
		{
			ps->vec_disabled = false;
			ps->ewma_hit_rate = hit;
			ps->auto_reenables++;
			if (ps->op)
				ps->op->disabled = false;
		}
		else
			ps->granules_skipped_probe = 0;
	}
	else if (ps->vec_disabled && ++ps->granules_skipped_probe >= SIFT_SIP_REPROBE_GRANULES)
		ps->reprobing = true;
}
