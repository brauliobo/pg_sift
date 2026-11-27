/*
 * agg/star/dense.c - the dense form of the table of a dimension (agg/star/dense.h).
 */
/* check-memory: allow-allocs the slots of a dense table are held to the budget of its shape by sift_star_dense_add, which grows them */
#include "core/host.h"

#include "agg/star/dense.h"

/* keys past these leave the form: the arithmetic of the slots of any span below them stays in an int64 */
#define DENSE_KEY_MAX	(PG_INT64_MAX / 4)
#define DENSE_MIN_CAP	64

static uint64
bitmap_bytes(uint64 cap)
{
	return (cap + 63) / 64 * sizeof(uint64);
}

uint64
sift_star_dense_bytes(const SiftStarShape *sh, uint64 cap, uint64 arena_cap)
{
	uint64		slot = 0;

	for (int c = 0; c < sh->npay; c++)
		slot += sift_star_pay_width(sh, c) + (sh->pcls[c] == SSC_STR ? sizeof(uint32) : 0);
	return cap * slot + bitmap_bytes(cap) * (1 + sh->npay) + arena_cap;
}

uint64
sift_star_arena_grown(const SiftStarDim *d, uint64 len)
{
	const uint64 need = TYPEALIGN(MAXIMUM_ALIGNOF, d->arena_used) + len;

	return need <= d->arena_cap ? d->arena_cap : Max(d->arena_cap * 2, Max(need, 1024));
}

static void
set_bit(uint64 *m, uint64 s, bool on)
{
	if (on)
		m[s >> 6] |= UINT64CONST(1) << (s & 63);
}

static void
put_value(SiftStarDim *d, int pay, uint64 s, uint64 v)
{
	switch (sift_star_pay_width(&d->shape, pay))
	{
		case 1:
			((int8 *) d->dpay[pay])[s] = (int8) v;
			break;
		case 2:
			((int16 *) d->dpay[pay])[s] = (int16) v;
			break;
		case 4:
			((int32 *) d->dpay[pay])[s] = (int32) v;
			break;
		default:
			((uint64 *) d->dpay[pay])[s] = v;
			break;
	}
}

/* the slots [base, base + cap) for the keys the table holds, which move to them */
static void
regrid(SiftStarDim *d, int64 base, uint64 cap)
{
	const SiftStarShape *sh = &d->shape;
	uint64	   *has = palloc0(bitmap_bytes(cap));
	void	   *pay[SIFT_STAR_MAX_PAY];
	uint64	   *nul[SIFT_STAR_MAX_PAY];
	uint32	   *len[SIFT_STAR_MAX_PAY] = {0};

	for (int c = 0; c < sh->npay; c++)
	{
		pay[c] = palloc((Size) cap * sift_star_pay_width(sh, c));
		nul[c] = palloc0(bitmap_bytes(cap));
		if (sh->pcls[c] == SSC_STR)
			len[c] = palloc(cap * sizeof(uint32));
	}
	for (uint64 s = 0; s < d->dcap; s++)
	{
		const uint64 t = (uint64) (d->dbase + (int64) s - base);

		if (!sift_star_dense_has(d, s))
			continue;
		set_bit(has, t, true);
		for (int c = 0; c < sh->npay; c++)
		{
			const uint32 w = sift_star_pay_width(sh, c);

			memcpy((char *) pay[c] + t * w, (const char *) d->dpay[c] + s * w, w);
			set_bit(nul[c], t, (d->dnull[c][s >> 6] >> (s & 63)) & 1);
			if (len[c] != NULL)
				len[c][t] = d->dlen[c][s];
		}
	}
	sift_star_dense_free(d);
	d->dhas = has;
	for (int c = 0; c < sh->npay; c++)
	{
		d->dpay[c] = pay[c];
		d->dnull[c] = nul[c];
		d->dlen[c] = len[c];
	}
	d->dbase = base;
	d->dcap = (uint32) cap;
}

/* slots for key k too, half as many again as the keys span so that the next keys out of it are few; false when they pass the budget */
static bool
widen(SiftStarDim *d, int64 k)
{
	const int64 lo = d->nrows > 0 ? Min(d->hull_min, k) : k;
	const int64 hi = d->nrows > 0 ? Max(d->hull_max, k) : k;
	const uint64 span = (uint64) (hi - lo) + 1;
	const uint64 cap = Max(span + span / 2, DENSE_MIN_CAP);

	if (cap > PG_UINT32_MAX / 2 || sift_star_dense_bytes(&d->shape, cap, d->arena_cap) > Min(d->shape.budget, MaxAllocSize))
		return false;
	regrid(d, lo - (int64) ((cap - span) / 2), cap);
	return true;
}

bool
sift_star_dense_reserve(SiftStarDim *d, const SiftStarRow *row)
{
	const SiftStarShape *sh = &d->shape;
	const int64 k = row->key_i[0];
	uint64		strs = 0;

	if (row->key_null[0] || k < -DENSE_KEY_MAX || k > DENSE_KEY_MAX || sift_star_dense_row(d, k) != 0)
		return false;
	if ((uint64) k - (uint64) d->dbase >= d->dcap && !widen(d, k))
		return false;
	for (int c = 0; c < sh->npay; c++)
		strs += sh->pcls[c] == SSC_STR && !row->pay_null[c] ? row->pay_len[c] + MAXIMUM_ALIGNOF : 0;
	return strs == 0 || sift_star_dense_bytes(sh, d->dcap, sift_star_arena_grown(d, strs)) <= Min(sh->budget, MaxAllocSize);
}

void
sift_star_dense_put(SiftStarDim *d, const SiftStarRow *row)
{
	const SiftStarShape *sh = &d->shape;
	const int64 k = row->key_i[0];
	const uint64 s = (uint64) (k - d->dbase);

	set_bit(d->dhas, s, true);
	for (int c = 0; c < sh->npay; c++)
	{
		const bool	isnull = row->pay_null[c];

		set_bit(d->dnull[c], s, isnull);
		put_value(d, c, s, isnull ? 0 : (uint64) row->pay_i[c]);
		if (sh->pcls[c] == SSC_STR)
			d->dlen[c][s] = isnull ? 0 : row->pay_len[c];
	}
	d->hull_min = Min(d->hull_min, k);
	d->hull_max = Max(d->hull_max, k);
	d->nrows++;
	d->bytes = sift_star_dense_bytes(sh, d->dcap, d->arena_cap);
}

void
sift_star_dense_take(const SiftStarDim *d, uint32 s, SiftStarRow *row)
{
	*row = (SiftStarRow) {0};
	row->key_i[0] = d->dbase + (int64) s;
	for (int c = 0; c < d->shape.npay; c++)
	{
		const uint64 v = sift_star_dense_bits(d, c, s);

		row->pay_null[c] = sift_star_pay_null(d, c, s);
		if (d->shape.pcls[c] != SSC_STR)
			row->pay_i[c] = (int64) v;
		else if (!row->pay_null[c])
		{
			row->pay_p[c] = d->arena + v;
			row->pay_len[c] = d->dlen[c][s];
		}
	}
}

void
sift_star_dense_finish(SiftStarDim *d)
{
	d->unique_keys = true;
	d->ndistinct = (uint32) d->nrows;
	if (d->nrows <= SIFT_STAR_EXACT_MAX)
	{
		d->exact = palloc(Max(d->nrows, 1) * sizeof(int64));
		for (uint64 s = 0; s < d->dcap; s++)
			if (sift_star_dense_has(d, s))
				d->exact[d->nexact++] = d->dbase + (int64) s;
	}
	d->finished = true;
}

void
sift_star_dense_free(SiftStarDim *d)
{
	if (d->dhas != NULL)
		pfree(d->dhas);
	d->dhas = NULL;
	for (int c = 0; c < d->shape.npay; c++)
	{
		if (d->dpay[c] != NULL)
			pfree(d->dpay[c]);
		if (d->dnull[c] != NULL)
			pfree(d->dnull[c]);
		if (d->dlen[c] != NULL)
			pfree(d->dlen[c]);
		d->dpay[c] = d->dnull[c] = NULL;
		d->dlen[c] = NULL;
	}
}
