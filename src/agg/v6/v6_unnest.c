/*
 * agg/v6/v6_unnest.c - the pairs of a LATERAL unnest and the answers of one array a parent (agg/v6/v6_unnest.h).
 */
#include "core/host.h"

#include "agg/v6/v6_unnest.h"
#include "agg/vexpr_str.h"

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
sift_unnest_gather64(uint64 *out, uint64 *valid_out, const uint64 *src, const uint8 *src_null, const uint32 *idx, uint32 n)
{
	memset(valid_out, 0, ((n + 63) / 64) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
	{
		const bool	ok = idx[i] != SIFT_UNNEST_NONE && (src_null == NULL || !src_null[idx[i]]);

		out[i] = ok ? src[idx[i]] : 0;
		valid_out[i >> 6] |= (uint64) ok << (i & 63);
	}
}

void
sift_unnest_gather_str(SiftStrView *out, uint64 *valid_out, const SiftStrView *src, const uint8 *src_null, const uint32 *idx, uint32 n)
{
	memset(valid_out, 0, ((n + 63) / 64) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
	{
		const bool	ok = idx[i] != SIFT_UNNEST_NONE && (src_null == NULL || !src_null[idx[i]]);

		out[i] = ok ? src[idx[i]] : (SiftStrView) {0};
		valid_out[i >> 6] |= (uint64) ok << (i & 63);
	}
}

void
sift_unnest_lengths(const SiftArrayLayout *lay, const uint64 *doc_valid, int32 *out, uint64 *valid_out)
{
	memset(valid_out, 0, ((lay->n_parents + 63) / 64) * sizeof(uint64));
	for (uint32 p = 0; p < lay->n_parents; p++)
	{
		const bool	ok = bit_at(doc_valid, p);

		out[p] = ok ? (int32) lay->lengths[p] : 0;
		valid_out[p >> 6] |= (uint64) ok << (p & 63);
	}
}

void
sift_unnest_any(const SiftArrayLayout *lay, const uint64 *hits, const uint64 *parent_mask, const uint64 *doc_valid, uint64 *true_out, uint64 *known_out)
{
	const uint32 words = (lay->n_parents + 63) / 64;

	memset(true_out, 0, words * sizeof(uint64));
	memset(known_out, 0, words * sizeof(uint64));
	for (uint32 p = 0; p < lay->n_parents; p++)
	{
		bool		any = false;

		if (!bit_at(parent_mask, p) || !bit_at(doc_valid, p))
			continue;
		for (uint32 e = next_elem(hits, lay->offsets[p], lay->offsets[p + 1]); e < lay->offsets[p + 1] && !any; e++)
			any = bit_at(hits, e);
		known_out[p >> 6] |= UINT64CONST(1) << (p & 63);
		true_out[p >> 6] |= (uint64) any << (p & 63);
	}
}

void
sift_unnest_hits_from_codes(const uint32 *codes, uint32 n_elems, const uint64 *elem_valid, const uint64 *dict_bits, uint32 ndict, uint64 *hits)
{
	memset(hits, 0, ((n_elems + 63) / 64) * sizeof(uint64));
	for (uint32 e = 0; e < n_elems; e++)
		if (bit_at(elem_valid, e) && codes[e] < ndict && ((dict_bits[codes[e] >> 6] >> (codes[e] & 63)) & 1))
			hits[e >> 6] |= UINT64CONST(1) << (e & 63);
}

void
sift_map_lookup(const SiftArrayLayout *lay, const SiftStrView *keys, const char *key, uint32 keylen, const uint64 *parent_mask, uint32 *elem_of)
{
	for (uint32 p = 0; p < lay->n_parents; p++)
	{
		elem_of[p] = SIFT_UNNEST_NONE;
		if (!bit_at(parent_mask, p))
			continue;
		for (uint32 e = lay->offsets[p + 1]; e > lay->offsets[p]; e--)
			if (keys[e - 1].len == keylen && memcmp(sift_vstr_bytes(&keys[e - 1]), key, keylen) == 0)
			{
				elem_of[p] = e - 1;
				break;
			}
	}
}

void
sift_map_lookup_code(const SiftArrayLayout *lay, const uint32 *codes, uint32 code, const uint64 *parent_mask, uint32 *elem_of)
{
	for (uint32 p = 0; p < lay->n_parents; p++)
	{
		elem_of[p] = SIFT_UNNEST_NONE;
		if (code == SIFT_UNNEST_NONE || !bit_at(parent_mask, p))
			continue;
		for (uint32 e = lay->offsets[p + 1]; e > lay->offsets[p]; e--)
			if (codes[e - 1] == code)
			{
				elem_of[p] = e - 1;
				break;
			}
	}
}

uint32
sift_unpivot_expand(const uint64 *const *leaf_present, int nleaves, const uint64 *parent_mask, uint32 n_parents, SiftUnpivotCursor *cur, uint32 *parent_idx, int *leaf_idx, uint32 cap)
{
	uint32		out = 0;

	while (cur->parent < n_parents && out < cap)
	{
		if (!bit_at(parent_mask, cur->parent) || cur->leaf >= nleaves)
		{
			cur->parent++;
			cur->leaf = 0;
			continue;
		}
		if (bit_at(leaf_present[cur->leaf], cur->parent))
		{
			parent_idx[out] = cur->parent;
			leaf_idx[out++] = cur->leaf;
		}
		cur->leaf++;
	}
	return out;
}
