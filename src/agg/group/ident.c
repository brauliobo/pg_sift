/*
 * agg/group/ident.c - the ids of ident keys and of time buckets
 * The id of an ident value, the wide table it grows into, and the bucket numbers of a
 * timestamp key in UTC, in a zone and in calendar steps.
 */
#include "postgres.h"

#include "agg/group/int.h"

static uint32 bucket_id(AggExec *ex, KeyState *k, int64 bucket);
static uint32 cal_id(AggExec *ex, KeyState *k, int64 day);

/* the next id of an ident key, for the value v */
uint32
ident_add(AggExec *ex, KeyState *k, int64 v)
{
	if (k->nident == k->capident)
	{
		k->capident = Max(k->capident * 2, 64);
		k->idval = k->idval ? repalloc(k->idval, k->capident * sizeof(int64)) :
			MemoryContextAlloc(ex->tcxt, k->capident * sizeof(int64));
	}
	k->idval[k->nident++] = v;
	return k->nident;
}

/*
 * The id of an ident key's value when the range table is not used: ids are given in order of appearance, as the table does. A
 * bucket key (a calendar expression of a timestamp) keeps the id of the value of its expression for each bucket number it sees, a calendar key
 * of a date for each day.
 */
uint32
ident_wide_id(AggExec *ex, KeyState *k, int64 v)
{
	uint32		h = wide_hash(v, k->wcap);
	uint32		id;

	while (k->wid[h])
	{
		if (k->wval[h] == v)
			return k->wid[h];
		h = (h + 1) & (k->wcap - 1);
	}
	k->wval[h] = v;
	id = k->bucket ? bucket_id(ex, k, v) : k->cal ? cal_id(ex, k, v) : k->ident ? ident_add(ex, k, v) : key_value_id(ex, (int) (k - ex->ks), leaf_datum(k->param->paramtype, v), false);
	k->wid[h] = id;
	if ((uint64) ++k->nwide * 2 > k->wcap)
	{
		uint32		ocap = k->wcap;
		int64	   *ov = k->wval;
		uint32	   *oi = k->wid;

		k->wcap *= 2;
		k->wval = MemoryContextAlloc(ex->tcxt, k->wcap * sizeof(int64));
		k->wid = MemoryContextAllocZero(ex->tcxt, k->wcap * sizeof(uint32));
		for (uint32 s = 0; s < ocap; s++)
			if (oi[s])
			{
				uint32		p = wide_hash(ov[s], k->wcap);

				while (k->wid[p])
					p = (p + 1) & (k->wcap - 1);
				k->wval[p] = ov[s];
				k->wid[p] = oi[s];
			}
		pfree(ov);
		pfree(oi);
	}
	return id;
}

/* from now on the ident key's ids come from the hash table; the values the range table gave ids to are in it */
void
ident_go_wide(AggExec *ex, KeyState *k)
{
	uint32		n = k->nident;

	/* a table after a spill starts with room for the keys of the one written, as its group arrays do (spill_keep) */
	k->wcap = 1024;
	while (k->wcap < 4 * Max(n, spill_keep(ex)))
		k->wcap *= 2;
	k->wval = MemoryContextAlloc(ex->tcxt, k->wcap * sizeof(int64));
	k->wid = MemoryContextAllocZero(ex->tcxt, k->wcap * sizeof(uint32));
	k->nident = 0;				/* ident_wide_id gives the same ids back, in the same order */
	k->nwide = 0;
	for (uint32 i = 0; i < n; i++)
		(void) ident_wide_id(ex, k, k->idval[i]);
}

void
key_range_fill(void *arg, int32 v, void *entry)
{
	KeyFill    *f = arg;
	KeyState   *k = &f->ex->ks[f->ki];

	if (k->ident)
		*(uint32 *) entry = ident_add(f->ex, k, v);
	else if (k->bucket)
		*(uint32 *) entry = bucket_id(f->ex, k, v);
	else
		*(uint32 *) entry = key_value_id(f->ex, f->ki, leaf_datum(k->param->paramtype, v), false);
}

/* the id of a bucket of a timestamp key: the key's expression of the bucket's first microsecond, or an instant of a step of the zone's clock (an infinity is a bucket of its own) */
static uint32
bucket_id(AggExec *ex, KeyState *k, int64 bucket)
{
	int64		us = bucket == PG_INT64_MAX || bucket == PG_INT64_MIN ? bucket : k->zcut ? k->witness : k->zone ? zone_instant(k->zone, bucket, k->bucket) : bucket * k->bucket;

	return key_value_id(ex, (int) (k - ex->ks), Int64GetDatum(us), false);
}

/* the id of the cell of a date key that starts on a day: the key's expression of that date */
static uint32
cal_id(AggExec *ex, KeyState *k, int64 day)
{
	return key_value_id(ex, (int) (k - ex->ks), Int32GetDatum((int32) day), false);
}

/* the number of the bucket of a timestamp, floor(v / us) */
static int64
bucket_floor(int64 v, const int64 us)
{
	return v / us - (v % us < 0);
}

/*
 * The bucket numbers of the selected rows as int32 (a NULL row gets 0) and their range; false: an infinity or a range a table
 * cannot hold is among them. us is a constant at each call, so the division is an unsigned multiplication: adding a multiple of us
 * above 2^63 makes every timestamp positive, and the same multiple of buckets comes off the quotient. shift: the offset of the zone's clock (a valid timestamp
 * is days short of the limits of int64, so the sum cannot overflow).
 */
static pg_attribute_always_inline bool
bucket_numbers(const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, const uint64 us, const int64 shift, int32 *mn, int32 *mx)
{
	const int64 *data = v->data;
	const uint8 *isnull = v->isnull;
	const uint64 off = us * (((uint64) 1 << 63) / us + 1),
				off_buckets = off / us;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]];

		if (isnull && isnull[idx[i]])
		{
			out[i] = 0;
			continue;
		}
		lo = Min(lo, d);
		hi = Max(hi, d);
		out[i] = (uint32) (((uint64) (d + shift) + off) / us - off_buckets);
	}
	if (lo > hi)
	{
		*mn = *mx = 0;
		return true;
	}
	if (lo == PG_INT64_MIN || hi == PG_INT64_MAX)
		return false;
	lo = bucket_floor(lo + shift, us);
	hi = bucket_floor(hi + shift, us);
	*mn = (int32) lo;
	*mx = (int32) hi;
	return lo >= PG_INT32_MIN && hi <= PG_INT32_MAX && hi - lo < MAX_RANGE;
}

/* bucket_numbers of a step of the UTC clock: a second, a minute, an hour or a day */
bool
bucket_numbers_utc(int64 us, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, int32 *mn, int32 *mx)
{
	switch (us)
	{
		case USECS_PER_SEC:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC, 0, mn, mx);
		case USECS_PER_SEC * 60:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC * 60, 0, mn, mx);
		case USECS_PER_HOUR:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_HOUR, 0, mn, mx);
		case USECS_PER_DAY:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_DAY, 0, mn, mx);
	}
	SIFT_INVARIANT(false, "sift: unexpected bucket of %lld microseconds", (long long) us);
	return false;
}

/* the zone's pieces cover the selected rows that are not an infinity or NULL (false: there are none) */
static bool
zone_rows(ZoneOff *z, const SiftSrcVec *v, const uint32 *idx, int nsel)
{
	const int64 *data = v->data;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]];

		if ((v->isnull && v->isnull[idx[i]]) || d == PG_INT64_MAX || d == PG_INT64_MIN)
			continue;
		lo = Min(lo, d);
		hi = Max(hi, d);
	}
	if (lo > hi)
		return false;
	zone_cover(z, lo, hi);
	return true;
}

/*
 * bucket_numbers for a key on the zone's clock: one piece of constant offset over the rows is a shift of the vector kernel, several are a lookup of the piece of each row.
 * False as bucket_numbers: an infinity or a range a table cannot hold is among the rows.
 */
bool
bucket_numbers_zoned(ZoneOff *z, int64 us, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, int32 *mn, int32 *mx)
{
	const int64 *data = v->data;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN,
				shift;
	int64		blo = PG_INT64_MAX,
				bhi = PG_INT64_MIN;
	bool		inf = false;

	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]];

		if (v->isnull && v->isnull[idx[i]])
			continue;
		inf |= d == PG_INT64_MAX || d == PG_INT64_MIN;
		if (!inf)
		{
			lo = Min(lo, d);
			hi = Max(hi, d);
		}
	}
	if (inf)
		return false;
	if (lo > hi)
	{
		for (int i = 0; i < nsel; i++)
			out[i] = 0;
		*mn = *mx = 0;
		return true;
	}
	zone_cover(z, lo, hi);
	if (zone_piece(z, lo) == zone_piece(z, hi))
	{
		shift = z->off[zone_piece(z, lo)];
		switch (us)
		{
			case USECS_PER_SEC:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC, shift, mn, mx);
			case USECS_PER_SEC * 60:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC * 60, shift, mn, mx);
			case USECS_PER_HOUR:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_HOUR, shift, mn, mx);
			case USECS_PER_DAY:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_DAY, shift, mn, mx);
			default:
				SIFT_INVARIANT(false, "sift: unexpected bucket of %lld microseconds", (long long) us);
		}
	}
	for (int i = 0; i < nsel; i++)
	{
		int64		b;

		if (v->isnull && v->isnull[idx[i]])
		{
			out[i] = 0;
			continue;
		}
		b = zone_bucket(z, data[idx[i]], us);
		blo = Min(blo, b);
		bhi = Max(bhi, b);
		out[i] = (uint32) b;
	}
	*mn = (int32) blo;
	*mx = (int32) bhi;
	return blo >= PG_INT32_MIN && bhi <= PG_INT32_MAX && bhi - blo < MAX_RANGE;
}

/* the bucket numbers of the n rows of v from `start` (iota: 0 .. n - 1), on the zone's clock or on UTC's, for the condition of a step (agg/pred_in.c); false as bucket_numbers */
bool
step_numbers(const SiftSrcVec *v, int start, int n, const uint32 *iota, ZoneOff *z, int64 us, uint32 *out, int32 *mn, int32 *mx)
{
	SiftSrcVec	w = *v;

	w.data = (const int64 *) v->data + start;
	if (v->isnull)
		w.isnull = v->isnull + start;
	return z ? bucket_numbers_zoned(z, us, &w, iota, n, out, mn, mx) : bucket_numbers_utc(us, &w, iota, n, out, mn, mx);
}

/* the key ids of the selected rows by the hash table of bucket numbers: infinities and wide ranges */
void
bucket_hash_ids(AggExec *ex, KeyState *k, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *ids)
{
	const int64 *data = v->data;
	int64		last = 0;
	uint32		lastid = 0;

	if (k->zone)
		zone_rows(k->zone, v, idx, nsel);
	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]],
					b = d == PG_INT64_MAX || d == PG_INT64_MIN ? d : k->zcut ? zone_cut(k->zone, d, k->bucket) : k->zone ? zone_bucket(k->zone, d, k->bucket) : bucket_floor(d, k->bucket);

		if (v->isnull && v->isnull[idx[i]])
			ids[i] = 0;			/* every function of a bucket key is strict: the key of a NULL is NULL */
		else
		{
			if (b != last || lastid == 0)
			{
				k->witness = d;
				lastid = ident_wide_id(ex, k, b);
				last = b;
			}
			ids[i] = lastid;
		}
	}
}
