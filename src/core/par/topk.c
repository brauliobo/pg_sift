/*
 * core/par/topk.c - the heap, the merge and the bound of core/par/topk.h.
 */
#include "core/par/topk.h"

static inline uint8 *
elem(const SiftParTopK *tk, uint32 i)
{
	return tk->slot + (size_t) i * tk->elem_size;
}

static void
swap_elems(const SiftParTopK *tk, uint32 i, uint32 j)
{
	uint8	   *a = elem(tk, i),
			   *b = elem(tk, j);

	for (size_t off = 0; off < tk->elem_size;)
	{
		uint8		tmp[64];
		const size_t n = Min(sizeof(tmp), tk->elem_size - off);

		memcpy(tmp, a + off, n);
		memcpy(a + off, b + off, n);
		memcpy(b + off, tmp, n);
		off += n;
	}
}

static void
sift_up(const SiftParTopK *tk, uint32 i)
{
	for (; i > 0 && tk->better(elem(tk, (i - 1) / 2), elem(tk, i), tk->ctx); i = (i - 1) / 2)
		swap_elems(tk, i, (i - 1) / 2);
}

static void
sift_down(const SiftParTopK *tk, uint32 i)
{
	for (;;)
	{
		uint32		worst = i;

		for (uint32 c = 2 * i + 1; c <= 2 * i + 2 && c < tk->n; c++)
			if (tk->better(elem(tk, worst), elem(tk, c), tk->ctx))
				worst = c;
		if (worst == i)
			return;
		swap_elems(tk, i, worst);
		i = worst;
	}
}

bool
sift_par_topk_init(SiftParTopK *tk, SiftArena *a, uint32 k, size_t elem_size, size_t str_bytes, SiftTopKBetter better, SiftTopKStore store, const void *ctx)
{
	uint8	   *str;

	*tk = (SiftParTopK) {.elem_size = elem_size,.k = k,.better = better,.store = store,.ctx = ctx};
	tk->slot = sift_arena_alloc0(a, elem_size * k, SIFT_ARENA_ALIGN);
	str = str_bytes > 0 ? sift_arena_alloc(a, str_bytes, SIFT_ARENA_ALIGN) : NULL;
	if (tk->slot == NULL || (str_bytes > 0 && str == NULL))
		return false;
	if (str_bytes > 0)
		sift_arena_init(&tk->str, str, str_bytes);
	return true;
}

static bool
store_into(SiftParTopK *tk, uint32 i, const void *cand)
{
	if (tk->store != NULL)
		return tk->store(elem(tk, i), cand, &tk->str, tk->ctx);
	memcpy(elem(tk, i), cand, tk->elem_size);
	return true;
}

bool
sift_par_topk_offer(SiftParTopK *tk, const void *cand)
{
	if (tk->n < tk->k)
	{
		if (!store_into(tk, tk->n, cand))
			return false;
		sift_up(tk, tk->n++);
		return true;
	}
	if (tk->k == 0 || !tk->better(cand, tk->slot, tk->ctx))
		return true;
	if (!store_into(tk, 0, cand))
		return false;
	sift_down(tk, 0);
	return true;
}

bool
sift_par_topk_merge(SiftParTopK *const *heap, int nw, SiftArena *out, const void ***sorted, uint32 *n)
{
	const SiftParTopK *first = heap[0];
	size_t		total = 0,
				fill = 0;
	const void **v,
			  **tmp;

	for (int t = 0; t < nw; t++)
		total += heap[t]->n;
	v = sift_arena_alloc(out, 2 * Max(total, 1) * sizeof(*v), sizeof(*v));
	if (v == NULL)
		return false;
	tmp = v + Max(total, 1);
	for (int t = 0; t < nw; t++)
		for (uint32 i = 0; i < heap[t]->n; i++)
			v[fill++] = elem(heap[t], i);
	for (size_t width = 1; width < total; width *= 2)
	{
		for (size_t lo = 0; lo < total; lo += 2 * width)
		{
			const size_t mid = Min(lo + width, total),
						hi = Min(lo + 2 * width, total);
			size_t		i = lo,
						j = mid,
						o = lo;

			while (i < mid && j < hi)
				tmp[o++] = first->better(v[j], v[i], first->ctx) ? v[j++] : v[i++];
			while (i < mid)
				tmp[o++] = v[i++];
			while (j < hi)
				tmp[o++] = v[j++];
		}
		memcpy(v, tmp, total * sizeof(*v));
	}
	*sorted = v;
	*n = (uint32) Min(total, first->k);
	return true;
}

void
sift_bound_init(SiftAtomicBound *b, uint64 key)
{
	atomic_init(&b->key, key);
}

uint64
sift_bound_load(const SiftAtomicBound *b)
{
	return atomic_load_explicit(&b->key, memory_order_relaxed);
}

void
sift_bound_raise(SiftAtomicBound *b, uint64 key)
{
	uint64		cur = atomic_load_explicit(&b->key, memory_order_relaxed);

	while (cur < key && !atomic_compare_exchange_weak_explicit(&b->key, &cur, key, memory_order_relaxed, memory_order_relaxed))
		;
}

uint64
sift_bound_key_i64(int64 v, bool desc)
{
	const uint64 up = (uint64) v ^ (UINT64CONST(1) << 63);

	return desc ? up : ~up;
}

uint64
sift_bound_key_f32(float v)
{
	uint32		u;

	memcpy(&u, &v, sizeof(u));
	return (u & 0x80000000u) != 0 ? ~u : u | 0x80000000u;
}

float
sift_bound_f32(uint64 key)
{
	uint32		u = (uint32) key;
	float		v;

	u = (u & 0x80000000u) != 0 ? u & 0x7fffffffu : ~u;
	memcpy(&v, &u, sizeof(v));
	return v;
}
