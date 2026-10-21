/*
 * bm25/kernels_test.c - test-only SQL functions sift._test_kern_*, compiled
 * only with SIFT_TESTING and registered by the pg_sift_test extension. The
 * standalone harness (tools/kernels) is the exhaustive check; these run the
 * kernels inside the backend, through the sift.simd dispatch.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "common/pg_prng.h"
#include "fmgr.h"

#include "bm25/kernels_int.h"

PG_FUNCTION_INFO_V1(sift_test_kern_digest);
PG_FUNCTION_INFO_V1(sift_test_kern_equiv);
PG_FUNCTION_INFO_V1(sift_test_kern_bm25);
PG_FUNCTION_INFO_V1(sift_test_kern_bound);

#define NPAGES		4
#define NSLOTS		(NPAGES * SIFT_KERN_SLOTS)
#define NTF			3000

/* Random inputs of every kernel */
typedef struct Case
{
	uint64		mask[NPAGES * SIFT_OFFMASK_WORDS];
	uint64		cand[NPAGES * SIFT_OFFMASK_WORDS];
	float4		kn[NSLOTS];
	float4		sc[NSLOTS];
	float4		other[NSLOTS];
	uint16		dl[NPAGES][SIFT_KERN_SLOTS];
	uint8		tf[NSLOTS + SIFT_KERN_PAD];
	uint8		stream[NTF * 2 + SIFT_KERN_PAD];
	SiftTfPos	p0;
	uint32		count;
	uint32		l1n;
	uint32		nibn;
	SiftBm25	bm;
	float4		w;
	float4		theta;
	uint64		base[8];
	uint64		rank[8];
	uint8		vm[64];
	uint8		bounds[64];
} Case;

/* Every kernel output; tails past the used length are zeroed before comparing */
typedef struct Out
{
	float4		kn[NSLOTS];
	float4		sc[NSLOTS];
	float4		sc1[NSLOTS];
	float4		sel[NSLOTS];
	float4		mx[NSLOTS];
	float4		vals[NSLOTS];
	uint16		slot[NSLOTS];
	uint8		tf[NTF + SIFT_KERN_PAD];
	uint64		ex[8];
	uint64		pm[4];
	uint64		pc[NPAGES * SIFT_OFFMASK_WORDS];
	float4		ub[64];
	SiftTfPos	pos;
	int32		r;
	int32		r1;
	int32		rs;
	int32		npr;
	int32		nsel;
	int32		nex;
} Out;

static void
gen_case(pg_prng_state *rs, Case *c)
{
	double		d = pg_prng_double(rs);
	uint8		tf[NTF];

	memset(c, 0, sizeof(*c));
	for (int s = 0; s < NSLOTS; s++)
	{
		if (pg_prng_double(rs) < d && s % SIFT_KERN_SLOTS < 300)
			c->mask[s / 64] |= UINT64CONST(1) << (s % 64);
		if (pg_prng_double(rs) < 0.5)
			c->cand[s / 64] |= UINT64CONST(1) << (s % 64);
		c->kn[s] = (float4) pg_prng_double(rs);
		c->sc[s] = (float4) (pg_prng_double(rs) * 100);
		c->other[s] = (float4) (pg_prng_double(rs) * 100);
		c->dl[s / SIFT_KERN_SLOTS][s % SIFT_KERN_SLOTS] = pg_prng_uint32(rs) % 2000;
		c->tf[s] = pg_prng_uint32(rs) % 4 ? 1 : 1 + pg_prng_uint32(rs) % 255;
	}
	c->count = pg_prng_uint32(rs) % NTF;
	c->p0 = (SiftTfPos) {pg_prng_uint32(rs) % 64, pg_prng_uint32(rs) % 64, pg_prng_uint32(rs) % 16};
	c->l1n = (c->p0.rank + c->count + 7) / 8;
	c->nibn = (c->p0.nib + c->count + 1) / 2;
	for (uint32 i = 0; i < c->count; i++)
		tf[i] = pg_prng_uint32(rs) % 4 ? 1 : 1 + pg_prng_uint32(rs) % 255;
	{
		SiftTfPos	p = c->p0;

		sift_kern_tf_encode(c->stream, c->stream + c->l1n, c->stream + c->l1n + c->nibn, &p, tf, c->count);
	}
	sift_kern_bm25_init(&c->bm, (float4) (pg_prng_double(rs) * 3), (float4) pg_prng_double(rs),
						(float4) (1 + pg_prng_double(rs) * 500));
	c->w = (float4) (pg_prng_double(rs) * 50);
	c->theta = c->sc[pg_prng_uint32(rs) % NSLOTS];
	for (int i = 0; i < 8; i++)
	{
		c->base[i] = pg_prng_uint64(rs) & pg_prng_uint64(rs);
		c->rank[i] = pg_prng_uint64(rs);
	}
	for (int i = 0; i < 64; i++)
	{
		c->vm[i] = (uint8) pg_prng_uint32(rs);
		c->bounds[i] = (uint8) pg_prng_uint32(rs);
	}
}

static void
run_case(const SiftKernels *k, const Case *c, Out *o)
{
	const uint16 *rows[NPAGES];
	SiftTfPos	p = c->p0;

	memset(o, 0, sizeof(*o));
	for (int i = 0; i < NPAGES; i++)
		rows[i] = c->dl[i];
	memcpy(o->kn, c->kn, sizeof(o->kn));
	k->knorm(o->kn, rows, c->mask, NPAGES, &c->bm);
	memcpy(o->sc, c->sc, sizeof(o->sc));
	memcpy(o->sc1, c->sc, sizeof(o->sc1));
	o->r = k->bm25_accum(o->sc, c->mask, NPAGES, c->tf, o->kn, c->w);
	o->r1 = k->bm25_accum(o->sc1, c->mask, NPAGES, NULL, o->kn, c->w);
	memcpy(o->sel, c->sc, sizeof(o->sel));
	o->rs = k->bm25_sel(o->sel, c->mask, c->cand, NPAGES, c->tf, o->kn, c->w);
	memcpy(o->pc, c->cand, sizeof(o->pc));
	o->npr = k->prune_gt(o->pc, c->sc, NPAGES, c->theta);
	memcpy(o->mx, o->sc, sizeof(o->mx));
	k->max_merge(o->mx, c->other, c->mask, NPAGES);
	o->nsel = k->select_gt(o->slot, o->vals, o->sc, c->mask, NPAGES, c->theta);
	memset(o->slot + o->nsel, 0, (NSLOTS - o->nsel) * sizeof(uint16));
	memset(o->vals + o->nsel, 0, (NSLOTS - o->nsel) * sizeof(float4));
	k->tf_decode(o->tf, c->stream, c->stream + c->l1n, c->stream + c->l1n + c->nibn, &p, c->count);
	memset(o->tf + c->count, 0, sizeof(o->tf) - c->count);
	o->pos = p;
	o->nex = k->expand(o->ex, c->mask, c->base, c->rank, c->p0.nib, 8);
	k->vm_allvisible(o->pm, c->vm);
	k->ub_accum(o->ub, c->bounds, c->w, 61);
}

static Case *
alloc_case(Out **o)
{
	*o = palloc_aligned(sizeof(Out), SIFT_SIMD_ALIGN, 0);
	return palloc_aligned(sizeof(Case), SIFT_SIMD_ALIGN, 0);
}

/* Hash of every output of the active kernels over iters seeded random cases */
Datum
sift_test_kern_digest(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	Out		   *o;
	Case	   *c = alloc_case(&o);
	uint64		h = UINT64CONST(0xCBF29CE484222325);

	pg_prng_seed(&rs, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		gen_case(&rs, c);
		run_case(sift_kern, c, o);
		for (size_t j = 0; j < sizeof(*o); j++)
			h = (h ^ ((const uint8 *) o)[j]) * UINT64CONST(0x100000001B3);
	}
	PG_RETURN_INT64((int64) h);
}

/* Cases where a SIMD level the CPU supports differs from scalar */
Datum
sift_test_kern_equiv(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	Out		   *ref,
			   *o = palloc_aligned(sizeof(Out), SIFT_SIMD_ALIGN, 0);
	Case	   *c = alloc_case(&ref);
	const SiftKernels *tabs[] = {sift_kernels_for(SIFT_SIMD_AVX2), sift_kernels_for(SIFT_SIMD_AVX512),
#if defined(__x86_64__)
		sift_kernels_for(SIFT_SIMD_AVX2) ? &sift_kernels_avx2_nopdep : NULL,
#endif
	};
	int64		bad = 0;

	pg_prng_seed(&rs, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		gen_case(&rs, c);
		run_case(&sift_kernels_scalar, c, ref);
		for (int t = 0; t < lengthof(tabs); t++)
		{
			if (!tabs[t])
				continue;
			run_case(tabs[t], c, o);
			bad += memcmp(o, ref, sizeof(*o)) != 0;
		}
	}
	PG_RETURN_INT64(bad);
}

/* One BM25 term score through the active knorm and bm25_accum */
Datum
sift_test_kern_bm25(PG_FUNCTION_ARGS)
{
	uint64		mask[SIFT_OFFMASK_WORDS] = {1};
	uint16		dl = PG_GETARG_INT32(1);
	const uint16 *row = &dl;
	uint8		tf[1 + SIFT_KERN_PAD] = {PG_GETARG_INT32(0)};
	float4	   *kn = palloc_aligned(SIFT_KERN_SLOTS * sizeof(float4), SIFT_SIMD_ALIGN, 0),
			   *sc = palloc_aligned(SIFT_KERN_SLOTS * sizeof(float4), SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	SiftBm25	bm;

	sift_kern_bm25_init(&bm, PG_GETARG_FLOAT4(2), PG_GETARG_FLOAT4(3), PG_GETARG_FLOAT4(4));
	sift_kern->knorm(kn, &row, mask, 1, &bm);
	sift_kern->bm25_accum(sc, mask, 1, tf, kn, PG_GETARG_FLOAT4(5));
	PG_RETURN_FLOAT4(sc[0]);
}

Datum
sift_test_kern_bound(PG_FUNCTION_ARGS)
{
	SiftBm25	ref;

	sift_kern_bm25_init(&ref, PG_GETARG_FLOAT4(2), PG_GETARG_FLOAT4(3), PG_GETARG_FLOAT4(4));
	PG_RETURN_INT32(sift_kern_bound_u8(&ref, PG_GETARG_INT32(0), PG_GETARG_INT32(1)));
}

#endif							/* SIFT_TESTING */
