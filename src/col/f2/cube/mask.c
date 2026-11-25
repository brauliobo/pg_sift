/*
 * col/f2/cube/mask.c - the granule mask of a scan for some keys of a summary (col/f2/cube/mask.h).
 */
/* check-memory: allow-allocs the arrays follow the granules and the keys of one summary */
#include "core/host.h"

#include "col/f2/cube/mask.h"
#include "core/popcount.h"

static void
set_bit(uint64 *bits, uint32 g)
{
	bits[g >> 6] |= UINT64CONST(1) << (g & 63);
}

static void
mark_key(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeReadMask *k)
{
	const uint32 nrefs = sift_scope_key_refs(m, r, NULL);
	SiftScopeGranRef *refs = palloc(sizeof(*refs) * Max(nrefs, 1));

	sift_scope_key_refs(m, r, refs);
	for (uint32 i = 0; i < nrefs; i++)
		set_bit(k->want, refs[i].global_granule_idx);
	pfree(refs);
}

static bool
members_hit(const int64 *v, uint32 n, int64 lo, int64 hi)
{
	uint32		a = 0,
				b = n;

	while (a < b)
	{
		const uint32 mid = a + (b - a) / 2;

		if (v[mid] < lo)
			a = mid + 1;
		else
			b = mid;
	}
	return a < n && v[a] <= hi;
}

/* A granule whose member zone holds a changed member may hold rows of a member that came in the keys */
static void
mark_changed(const SiftScopeMeta *m, const int64 *changed, uint32 nchanged, const SiftScopeGranSource *src, SiftScopeReadMask *k)
{
	for (uint32 e = 0; e < m->hdr.nparts; e++)
	{
		if (m->parts[e].part_gen == 0)
			continue;
		for (uint32 g = m->parts[e].base_granule_idx; g < m->parts[e].base_granule_idx + m->parts[e].ngranules; g++)
		{
			SiftScopeGranHead h = {0};

			src->head(src->arg, g, &h);
			if (!h.member.has_min_max || members_hit(changed, nchanged, h.member.min, h.member.max))
				set_bit(k->want, g);
		}
	}
}

void
sift_scope_rmask_build(const SiftScopeMeta *m, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged, const SiftScopeGranSource *src, SiftScopeReadMask *out)
{
	const uint32 words = (uint32) ((m->hdr.total_granules + 63) / 64);

	*out = (SiftScopeReadMask) {.n = (uint32) m->hdr.total_granules,.want = palloc0(sizeof(uint64) * Max(words, 1))};
	for (uint32 i = 0; i < nkeys; i++)
	{
		const SiftScopeL2Root *r = sift_scope_l2_find(m, keys[i]);

		if (r != NULL)
			mark_key(m, r, out);
	}
	if (nchanged > 0)
		mark_changed(m, changed, nchanged, src, out);
	for (uint32 w = 0; w < words; w++)
		out->kept += (uint64) sift_pc64(out->want[w]);
}
