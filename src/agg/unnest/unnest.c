/*
 * agg/unnest/unnest.c - the pairs of a LATERAL unnest and the answers of one array a parent (agg/unnest/unnest.h).
 */
#include "core/host.h"

#include "agg/unnest/unnest.h"
#include "agg/vexpr/str.h"

static inline bool
bit_at(const uint64 *m, uint32 i)
{
	return m == NULL || ((m[i >> 6] >> (i & 63)) & 1);
}

/* the first element of [from, to) that passes the mask, or to */
static uint32
next_elem(const uint64 *mask, uint32 from, uint32 to)
{
	if (mask == NULL)
		return from;
	while (from < to)
	{
		const uint64 w = mask[from >> 6] >> (from & 63);

		if (w != 0)
			return Min(to, from + (uint32) __builtin_ctzll(w));
		from = (from | 63) + 1;
	}
	return to;
}

bool
sift_unnest_done(const SiftUnnestIn *in, const SiftUnnestCursor *cur)
{
	return cur->parent >= in->lay->n_parents;
}

uint32
sift_unnest_expand(const SiftUnnestIn *in, SiftUnnestCursor *cur, uint32 *parent_idx, uint32 *elem_idx, uint32 cap, uint64 *fallback)
{
	const SiftArrayLayout *lay = in->lay;
	uint32		out = 0;

	while (cur->parent < lay->n_parents && out < cap)
	{
		const uint32 p = cur->parent;
		uint32		to;

		if (!bit_at(in->parent_mask, p))
		{
			cur->parent++;
			continue;
		}
		if (in->poison != NULL && bit_at(in->poison, p))
		{
			fallback[p >> 6] |= UINT64CONST(1) << (p & 63);
			cur->parent++;
			continue;
		}
		if (!cur->in_parent)
		{
			cur->elem = bit_at(in->doc_valid, p) ? lay->offsets[p] : lay->offsets[p + 1];
			cur->in_parent = true;
			cur->emitted = false;
		}
		to = lay->offsets[p + 1];
		cur->elem = next_elem(in->elem_mask, cur->elem, to);
		if (cur->elem < to)
		{
			parent_idx[out] = p;
			elem_idx[out++] = cur->elem++;
			cur->emitted = true;
			continue;
		}
		if (in->left && !cur->emitted)
		{
			parent_idx[out] = p;
			elem_idx[out++] = SIFT_UNNEST_NONE;
		}
		cur->parent++;
		cur->in_parent = false;
	}
	return out;
}

void
sift_unnest_any(const SiftArrayLayout *lay, const uint64 *hits, const uint64 *parent_mask, const uint64 *doc_valid, uint64 *true_out, uint64 *known_out)
{
	const uint32 words = (lay->n_parents + 63) / 64;

	memset(true_out, 0, words * sizeof(uint64));
	memset(known_out, 0, words * sizeof(uint64));
	for (uint32 p = 0; p < lay->n_parents; p++)
	{
		if (!bit_at(parent_mask, p) || !bit_at(doc_valid, p))
			continue;
		known_out[p >> 6] |= UINT64CONST(1) << (p & 63);
		true_out[p >> 6] |= (uint64) (next_elem(hits, lay->offsets[p], lay->offsets[p + 1]) < lay->offsets[p + 1]) << (p & 63);
	}
}

/* the bits of word w of [from, to) (from < to) */
static inline uint64
range_mask(uint32 w, uint32 from, uint32 to)
{
	uint64		m = ~UINT64CONST(0);

	if (w == from >> 6)
		m &= ~UINT64CONST(0) << (from & 63);
	if (w == (to - 1) >> 6 && (to & 63) != 0)
		m &= (UINT64CONST(1) << (to & 63)) - 1;
	return m;
}

uint32
sift_unnest_count_bits(const uint64 *bits, uint32 lo, uint32 hi)
{
	uint32		n = 0;

	if (lo >= hi)
		return 0;
	if (bits == NULL)
		return hi - lo;
	for (uint32 w = lo >> 6; w <= (hi - 1) >> 6; w++)
		n += sift_pc64(bits[w] & range_mask(w, lo, hi));
	return n;
}

/* the elements parent p gives: those of its range that pass the elements' mask (a parent without a document has none) */
static inline uint32
parent_elems(const SiftUnnestIn *in, uint32 p)
{
	if (!bit_at(in->doc_valid, p))
		return 0;
	return sift_unnest_count_bits(in->elem_mask, in->lay->offsets[p], in->lay->offsets[p + 1]);
}

uint64
sift_unnest_weights(const SiftUnnestIn *in, uint32 *weight, uint64 *fallback)
{
	uint64		total = 0;

	memset(weight, 0, in->lay->n_parents * sizeof(uint32));
	for (uint32 p = next_elem(in->parent_mask, 0, in->lay->n_parents); p < in->lay->n_parents; p = next_elem(in->parent_mask, p + 1, in->lay->n_parents))
	{
		if (in->poison != NULL && bit_at(in->poison, p))
		{
			fallback[p >> 6] |= UINT64CONST(1) << (p & 63);
			continue;
		}
		weight[p] = parent_elems(in, p);
		if (weight[p] == 0 && in->left)
			weight[p] = 1;
		total += weight[p];
	}
	return total;
}

uint32
sift_unnest_elem_mask(const SiftUnnestIn *in, uint64 *out, uint32 *lo, uint32 *hi)
{
	const SiftArrayLayout *lay = in->lay;

	*lo = lay->offsets[0];
	*hi = lay->offsets[lay->n_parents];
	if (*lo >= *hi)
		return 0;
	memset(out + (*lo >> 6), 0, (((*hi - 1) >> 6) - (*lo >> 6) + 1) * sizeof(uint64));
	for (uint32 p = next_elem(in->parent_mask, 0, in->lay->n_parents); p < lay->n_parents; p = next_elem(in->parent_mask, p + 1, in->lay->n_parents))
	{
		const uint32 from = lay->offsets[p],
					to = lay->offsets[p + 1];

		if (from >= to || !bit_at(in->doc_valid, p) || (in->poison != NULL && bit_at(in->poison, p)))
			continue;
		for (uint32 w = from >> 6; w <= (to - 1) >> 6; w++)
			out[w] |= range_mask(w, from, to) & (in->elem_mask != NULL ? in->elem_mask[w] : ~UINT64CONST(0));
	}
	return sift_unnest_count_bits(out, *lo, *hi);
}

void
sift_unnest_count_codes(const uint32 *code0, const uint64 *valid0, const uint32 *code1, const uint64 *valid1, uint32 ncodes1, const uint64 *sel, uint32 lo, uint32 hi, uint64 *cells)
{
	const uint32 stride = code1 != NULL ? ncodes1 + 1 : 1;

	if (lo >= hi)
		return;
	for (uint32 w = lo >> 6; w <= (hi - 1) >> 6; w++)
		for (uint64 bits = (sel != NULL ? sel[w] : ~UINT64CONST(0)) & range_mask(w, lo, hi); bits != 0; bits &= bits - 1)
		{
			const uint32 e = w * 64 + (uint32) __builtin_ctzll(bits);
			const uint32 s0 = bit_at(valid0, e) ? code0[e] + 1 : 0;
			const uint32 s1 = code1 != NULL && bit_at(valid1, e) ? code1[e] + 1 : 0;

			cells[(uint64) s0 * stride + s1]++;
		}
}

uint32
sift_unnest_semi(const SiftUnnestIn *in, uint64 *hit_out, uint64 *fallback)
{
	uint32		n = 0;

	memset(hit_out, 0, ((in->lay->n_parents + 63) / 64) * sizeof(uint64));
	for (uint32 p = next_elem(in->parent_mask, 0, in->lay->n_parents); p < in->lay->n_parents; p = next_elem(in->parent_mask, p + 1, in->lay->n_parents))
	{
		if (in->poison != NULL && bit_at(in->poison, p))
			fallback[p >> 6] |= UINT64CONST(1) << (p & 63);
		else if (bit_at(in->doc_valid, p) && next_elem(in->elem_mask, in->lay->offsets[p], in->lay->offsets[p + 1]) < in->lay->offsets[p + 1])
		{
			hit_out[p >> 6] |= UINT64CONST(1) << (p & 63);
			n++;
		}
	}
	return n;
}
