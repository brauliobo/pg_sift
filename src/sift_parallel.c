/*
 * sift_parallel.c - work distribution and threshold sharing of a parallel
 * SiftScan (sift_parallel.h).
 */
#include "sift_host.h"

#include <math.h>

#include "sift_parallel.h"

/* Superblocks or units per claim: small enough that every participant gets several, at most this */
#define MORSEL_MAX		16
#define MORSELS_PER_PARTICIPANT	4

static uint32
morsel_size(int64 n, int nparticipants)
{
	return Max(1, Min(MORSEL_MAX, n / ((int64) MORSELS_PER_PARTICIPANT * nparticipants)));
}

Size
sift_par_estimate(int nworkers)
{
	return add_size(offsetof(SiftParShared, slot), mul_size(nworkers + 1, sizeof(SiftParSlot)));
}

static uint32
float_bits(float4 f)
{
	uint32		b;

	memcpy(&b, &f, sizeof(b));
	return b;
}

static float4
bits_float(uint32 b)
{
	float4		f;

	memcpy(&f, &b, sizeof(f));
	return f;
}

void
sift_par_init(SiftParShared *sh, int nworkers, uint32 nsb)
{
	sh->nplanned = nworkers + 1;
	sh->nslots = nworkers + 1;
	sh->nsb = nsb;
	sh->morsel = morsel_size(nsb, sh->nplanned);
	pg_atomic_init_u64(&sh->next, 0);
	pg_atomic_init_u32(&sh->theta, float_bits(-INFINITY));
	SpinLockInit(&sh->mutex);
	sh->nattached = sh->nfinished = 0;
	sh->emitter_taken = sh->mutable_taken = sh->units_set = false;
	sh->units_sig = 0;
	sh->units_n = 0;
	sh->bind = InvalidDsaPointer;
	sh->bind_len = 0;
	memset(sh->slot, 0, sh->nslots * sizeof(SiftParSlot));
}

/* A rescan: what the previous run left in the area goes, the counters start again */
void
sift_par_reinit(SiftParShared *sh, dsa_area *area, int nworkers, uint32 nsb)
{
	for (int i = 0; i < sh->nslots; i++)
		if (sh->slot[i].nhits > 0)
			dsa_free(area, sh->slot[i].hits);
	if (DsaPointerIsValid(sh->bind))
		dsa_free(area, sh->bind);
	sift_par_init(sh, nworkers, nsb);
}

void
sift_par_set_bind(SiftParShared *sh, dsa_area *area, const void *data, Size len)
{
	sh->bind = dsa_allocate_extended(area, len, DSA_ALLOC_HUGE);
	sh->bind_len = len;
	memcpy(dsa_get_address(area, sh->bind), data, len);
}

const void *
sift_par_get_bind(const SiftPar *par, Size *len)
{
	*len = par->sh->bind_len;
	return DsaPointerIsValid(par->sh->bind) ? dsa_get_address(par->area, par->sh->bind) : NULL;
}

void
sift_par_attach(SiftPar *par)
{
	SiftParShared *sh = par->sh;

	SpinLockAcquire(&sh->mutex);
	par->slot = sh->nattached++;
	SpinLockRelease(&sh->mutex);
	if (par->slot >= sh->nslots)
		elog(ERROR, "sift: more parallel participants than planned");
}

bool
sift_par_next_range(void *arg, uint32 *lo, uint32 *hi)
{
	SiftParShared *sh = ((SiftPar *) arg)->sh;
	uint64		m = pg_atomic_fetch_add_u64(&sh->next, 1);

	if (m * sh->morsel >= sh->nsb)
		return false;
	*lo = m * sh->morsel;
	*hi = Min(*lo + sh->morsel, sh->nsb);
	return true;
}

void
sift_par_check_units(SiftPar *par, int64 nunit, uint64 sig)
{
	SiftParShared *sh = par->sh;
	bool		same;

	SpinLockAcquire(&sh->mutex);
	if (!sh->units_set)
	{
		sh->units_set = true;
		sh->units_n = nunit;
		sh->units_sig = sig;
	}
	same = sh->units_n == nunit && sh->units_sig == sig;
	SpinLockRelease(&sh->mutex);
	if (!same)
		elog(ERROR, "sift: parallel participants bound different unit lists");
}

bool
sift_par_next_units(SiftPar *par, int64 nunit, int64 *lo, int64 *hi)
{
	SiftParShared *sh = par->sh;
	uint32		size = morsel_size(nunit, sh->nplanned);
	uint64		m = pg_atomic_fetch_add_u64(&sh->next, 1);

	if (m * size >= nunit)
		return false;
	*lo = m * size;
	*hi = Min(*lo + size, nunit);
	return true;
}

int64
sift_par_units_claimed(const SiftPar *par, int64 nunit)
{
	return Min(pg_atomic_read_u64(&par->sh->next) * morsel_size(nunit, par->sh->nplanned), nunit);
}

bool
sift_par_claim_mutable(SiftPar *par)
{
	SiftParShared *sh = par->sh;
	bool		mine;

	SpinLockAcquire(&sh->mutex);
	mine = !sh->mutable_taken;
	sh->mutable_taken = true;
	SpinLockRelease(&sh->mutex);
	return mine;
}

float4
sift_par_theta(const SiftPar *par)
{
	return bits_float(pg_atomic_read_u32(&par->sh->theta));
}

void
sift_par_raise_theta(SiftPar *par, float4 theta)
{
	uint32		cur = pg_atomic_read_u32(&par->sh->theta);

	while (theta > bits_float(cur))
		if (pg_atomic_compare_exchange_u32(&par->sh->theta, &cur, float_bits(theta)))
			break;
}

void
sift_par_publish(SiftPar *par, const SiftParHit *hits, int64 n)
{
	SiftParSlot *slot = &par->sh->slot[par->slot];

	if (n > 0)
	{
		slot->hits = dsa_allocate_extended(par->area, n * sizeof(SiftParHit), DSA_ALLOC_HUGE);
		memcpy(dsa_get_address(par->area, slot->hits), hits, n * sizeof(SiftParHit));
	}
	slot->nhits = n;
}

/*
 * The finishing participant that finds all attached ones finished emits. A
 * late worker attaches after the work is gone and finishes empty-handed:
 * emitter_taken keeps it from emitting.
 */
bool
sift_par_finish(SiftPar *par)
{
	SiftParShared *sh = par->sh;
	bool		last;

	SpinLockAcquire(&sh->mutex);
	sh->nfinished++;
	last = sh->nfinished == sh->nattached && !sh->emitter_taken;
	sh->emitter_taken |= last;
	SpinLockRelease(&sh->mutex);
	return last;
}

SiftParHit *
sift_par_collect(SiftPar *par, int64 *n)
{
	SiftParShared *sh = par->sh;
	SiftParHit *all;
	int64		total = 0;

	for (int i = 0; i < sh->nattached; i++)
		total += sh->slot[i].nhits;
	all = palloc_extended(Max(total, 1) * sizeof(SiftParHit), MCXT_ALLOC_HUGE);
	total = 0;
	for (int i = 0; i < sh->nattached; i++)
		if (sh->slot[i].nhits > 0)
		{
			memcpy(all + total, dsa_get_address(par->area, sh->slot[i].hits), sh->slot[i].nhits * sizeof(SiftParHit));
			total += sh->slot[i].nhits;
		}
	*n = total;
	return all;
}
