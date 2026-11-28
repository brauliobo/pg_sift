/*
 * agg/star/flat.c - the table of a dimension as one block of bytes (agg/star/star.h): the struct, its pointers turned into offsets from the start of the block, then each array it holds. The leader of a
 * parallel scan writes its tables into the shared block once; every participant maps them in place and probes them without a copy.
 */
#include "core/host.h"

#include "agg/star/dense.h"

/* What is done with each array of a table: field is the pointer of the struct, len the bytes of it that hold the rows */
typedef void (*FlatFn) (void **field, uint64 len, void *arg);

static uint64
bitmap_bytes(uint64 bits)
{
	return (bits + 63) / 64 * sizeof(uint64);
}

static void
each_array(SiftStarDim *d, FlatFn fn, void *arg)
{
	const SiftStarShape *sh = &d->shape;

	for (int c = 0; c < sh->nkeys + sh->npay; c++)
	{
		fn((void **) &d->col[c], (uint64) d->nrows * sizeof(uint64), arg);
		fn((void **) &d->len[c], (uint64) d->nrows * sizeof(uint32), arg);
		fn((void **) &d->null[c], d->nrows, arg);
	}
	fn((void **) &d->arena, d->arena_used, arg);
	fn((void **) &d->lo, (uint64) d->nrows * sizeof(int64), arg);
	fn((void **) &d->hi, (uint64) d->nrows * sizeof(int64), arg);
	fn((void **) &d->period_ok, d->nrows, arg);
	fn((void **) &d->rng_ord, (uint64) d->nrng * sizeof(uint32), arg);
	fn((void **) &d->rng_lo, (uint64) d->nrng * sizeof(int64), arg);
	fn((void **) &d->rng_hi, (uint64) d->nrng * sizeof(int64), arg);
	fn((void **) &d->rng_max, 2 * (uint64) d->rng_size * sizeof(int64), arg);
	fn((void **) &d->head, (uint64) d->ht_cap * sizeof(uint32), arg);
	fn((void **) &d->tail, (uint64) d->ht_cap * sizeof(uint32), arg);
	fn((void **) &d->next, (uint64) d->nrows * sizeof(uint32), arg);
	fn((void **) &d->matched, bitmap_bytes(d->nrows), arg);
	fn((void **) &d->exact, (uint64) d->nexact * sizeof(int64), arg);
	fn((void **) &d->dhas, bitmap_bytes(d->dcap), arg);
	for (int c = 0; c < sh->npay; c++)
	{
		fn(&d->dpay[c], (uint64) d->dcap * sift_star_pay_width(sh, c), arg);
		fn((void **) &d->dnull[c], bitmap_bytes(d->dcap), arg);
		fn((void **) &d->dlen[c], (uint64) d->dcap * sizeof(uint32), arg);
	}
}

typedef struct FlatAt
{
	char	   *base;			/* the block; NULL: only its size is counted */
	uint64		at;
} FlatAt;

/* the array goes at the next aligned offset of the block, and the struct of the block holds that offset */
static void
put_array(void **field, uint64 len, void *arg)
{
	FlatAt	   *f = arg;

	if (*field == NULL)
		return;
	if (f->base != NULL)
	{
		memcpy(f->base + f->at, *field, len);
		*field = (void *) (uintptr_t) f->at;
	}
	f->at += MAXALIGN(len);
}

static void
map_array(void **field, uint64 len, void *arg)
{
	if (*field != NULL)
		*field = (char *) arg + (uintptr_t) *field;
}

uint64
sift_star_dim_flat_bytes(const SiftStarDim *d)
{
	SiftStarDim copy = *d;
	FlatAt		f = {NULL, MAXALIGN(sizeof(SiftStarDim))};

	each_array(&copy, put_array, &f);
	return f.at;
}

void
sift_star_dim_flat_write(const SiftStarDim *d, char *dst)
{
	SiftStarDim *head = (SiftStarDim *) dst;
	FlatAt		f = {dst, MAXALIGN(sizeof(SiftStarDim))};

	*head = *d;
	each_array(head, put_array, &f);
}

void
sift_star_dim_flat_map(SiftStarDim *d, const char *src)
{
	*d = *(const SiftStarDim *) src;
	each_array(d, map_array, (void *) src);
}
