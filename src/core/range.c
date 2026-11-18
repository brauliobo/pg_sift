/*
 * core/range.c - bounds and ranges of the key of a partition (core/range.h)
 */
#include <string.h>

#include "core/range.h"

enum
{
	TAG_VALUE = 1,				/* followed by the 8 bytes of the integer, big-endian, sign bit flipped */
	TAG_NULL = 2,				/* alone: NULL sorts after every integer */
	TAG_TID = 3					/* followed by the 6 bytes of the address, big-endian: after every component */
};

#define VALUE_BYTES 8
#define TID_BYTES	6

static uint32_t
elem_bytes(uint8_t tag)
{
	return tag == TAG_VALUE ? VALUE_BYTES : tag == TAG_TID ? TID_BYTES : 0;
}

void
sift_bound_clear(SiftBound *b)
{
	b->len = 0;
	b->ncomp = 0;
}

static bool
has_tid(const SiftBound *b)
{
	uint32_t	at = 0;

	while (at < b->len)
	{
		if (b->bytes[at] == TAG_TID)
			return true;
		at += 1 + elem_bytes(b->bytes[at]);
	}
	return false;
}

static void
put_be(uint8_t *dst, uint64_t v, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		dst[i] = (uint8_t) (v >> (8 * (n - 1 - i)));
}

bool
sift_bound_push_i64(SiftBound *b, int64_t v)
{
	if (b->ncomp >= SIFT_BOUND_COMPS || has_tid(b))
		return false;
	b->bytes[b->len++] = TAG_VALUE;
	put_be(&b->bytes[b->len], (uint64_t) v ^ (UINT64_C(1) << 63), VALUE_BYTES);
	b->len += VALUE_BYTES;
	b->ncomp++;
	return true;
}

bool
sift_bound_push_null(SiftBound *b)
{
	if (b->ncomp >= SIFT_BOUND_COMPS || has_tid(b))
		return false;
	b->bytes[b->len++] = TAG_NULL;
	b->ncomp++;
	return true;
}

bool
sift_bound_push_tid(SiftBound *b, uint64_t tid)
{
	if (tid > SIFT_BOUND_TID_MAX || has_tid(b))
		return false;
	b->bytes[b->len++] = TAG_TID;
	put_be(&b->bytes[b->len], tid, TID_BYTES);
	b->len += TID_BYTES;
	return true;
}

int
sift_bound_cmp(const SiftBound *a, const SiftBound *b)
{
	uint32_t	n = a->len < b->len ? a->len : b->len;
	int			c = memcmp(a->bytes, b->bytes, n);

	return c != 0 ? c : (int) a->len - (int) b->len;
}

uint32_t
sift_bound_depth(const SiftBound *a, const SiftBound *b)
{
	uint32_t	at = 0,
				depth = 0;

	while (at < a->len && at < b->len && a->bytes[at] != TAG_TID && b->bytes[at] != TAG_TID)
	{
		uint32_t	n = 1 + elem_bytes(a->bytes[at]);

		if (a->bytes[at] != b->bytes[at] || at + n > b->len || memcmp(&a->bytes[at], &b->bytes[at], n) != 0)
			break;
		at += n;
		depth++;
	}
	return depth;
}

/* The offset of the last element of a bound of at least one */
static uint32_t
last_elem(const SiftBound *b)
{
	uint32_t	at = 0,
				last = 0;

	while (at < b->len)
	{
		last = at;
		at += 1 + elem_bytes(b->bytes[at]);
	}
	return last;
}

/* Add one to the n big-endian bytes; false when they were all 0xFF (they are then all zero) */
static bool
increment_be(uint8_t *p, uint32_t n)
{
	for (uint32_t i = n; i > 0; i--)
		if (++p[i - 1] != 0)
			return true;
	return false;
}

bool
sift_bound_prefix_next(const SiftBound *p, SiftBound *out)
{
	*out = *p;
	while (out->len > 0)
	{
		uint32_t	at = last_elem(out);
		uint8_t		tag = out->bytes[at];

		if (tag == TAG_NULL)
		{
			out->len = (uint8_t) at;
			out->ncomp--;
			continue;
		}
		if (increment_be(&out->bytes[at + 1], elem_bytes(tag)))
			return true;
		out->len = (uint8_t) at;
		if (tag == TAG_TID)
			continue;
		out->bytes[out->len++] = TAG_NULL;
		return true;
	}
	sift_bound_clear(out);
	return false;
}

bool
sift_range_unbounded_hi(const SiftRange *r)
{
	return r->hi.len == 0;
}

static bool
starts_below_end(const SiftBound *lo, const SiftRange *r)
{
	return sift_range_unbounded_hi(r) || sift_bound_cmp(lo, &r->hi) < 0;
}

bool
sift_range_overlap(const SiftRange *a, const SiftRange *b)
{
	return starts_below_end(&a->lo, a) && starts_below_end(&b->lo, b) && starts_below_end(&a->lo, b) && starts_below_end(&b->lo, a);
}

bool
sift_range_contains(const SiftRange *r, const SiftBound *key)
{
	return sift_bound_cmp(&r->lo, key) <= 0 && starts_below_end(key, r);
}

bool
sift_range_meets_prefix(const SiftRange *r, const SiftBound *prefix)
{
	SiftRange	keys = {.lo = *prefix};

	sift_bound_prefix_next(prefix, &keys.hi);
	return sift_range_overlap(r, &keys);
}

/* The number of the n ascending lower bounds that are below the key (strictly), or at or below it with inclusive */
static uint32_t
count_below(const void *refs, SiftBoundAt at, uint32_t n, const SiftBound *key, bool inclusive)
{
	uint32_t	lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32_t	mid = lo + (hi - lo) / 2;
		int			c = sift_bound_cmp(at(refs, mid), key);

		if (c < 0 || (inclusive && c == 0))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* The lowest key of the index with the prefix: the prefix, then the least integer in each component it lacks and the address 0 */
static SiftBound
lowest_with_prefix(const SiftBound *prefix, uint32_t ncomp)
{
	SiftBound	low = *prefix;

	if (has_tid(&low))
		return low;
	while (low.ncomp < ncomp)
		sift_bound_push_i64(&low, INT64_MIN);
	sift_bound_push_tid(&low, 0);
	return low;
}

void
sift_prefix_span(const void *refs, SiftBoundAt at, uint32_t n, uint32_t ncomp, const SiftBound *prefix, uint32_t *first, uint32_t *last)
{
	SiftBound	low = lowest_with_prefix(prefix, ncomp),
				next;
	uint32_t	upto = count_below(refs, at, n, &low, true);

	*first = upto > 0 ? upto - 1 : 0;
	*last = sift_bound_prefix_next(prefix, &next) ? count_below(refs, at, n, &next, false) : n;
}
