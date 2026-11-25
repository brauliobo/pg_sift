/*
 * col/f2/scope/ent_batch.c - the cubes of a batch of members of the entity parts (SiftScopeEntity.members_cubes): the granules whose member zone holds one of them are decoded once for
 * all of them, however many of the members the granule holds.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/scope/int.h"

#define BATCH_MAX 256

/* The slot of a member in the batch: open-addressed, 2 * BATCH_MAX slots of idx + 1 */
static inline uint32
member_at(const int64 *members, const uint16 *ht, int64 member)
{
	uint32		at = (uint32) (((uint64) member * UINT64_C(0x9E3779B97F4A7C15)) >> 40) & (2 * BATCH_MAX - 1);

	while (ht[at] != 0 && members[ht[at] - 1] != member)
		at = (at + 1) & (2 * BATCH_MAX - 1);
	return at;
}

/* A granule whose time zone leaves the span holds no row of it */
static bool
zone_meets_span(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const SiftScopeSpan *span)
{
	SiftZone	z;

	if (span->all)
		return true;
	z = sift_f2s_slot_zone(p, s, p->sc->time_leaf, g);
	return !(z.flags & SIFT_ZONE_NONE) && z.max * p->sc->time_unit >= span->lo && z.min * p->sc->time_unit < span->hi;
}

void
sift_f2s_members_cubes(void *arg, const int64 *members, uint32 n, SiftScopeSpan span, SiftScopeCubeAcc *out, bool *has)
{
	SiftF2ScopeParts *p = arg;
	uint16		ht[2 * BATCH_MAX] = {0};
	const bool	coded = p->sc->mem != NULL;

	SIFT_INVARIANT(n <= BATCH_MAX, "scope entity: a batch of %u members", n);
	for (uint32 i = 0; i < n; i++)
	{
		sift_scube_init(&out[i], p->sc->shape);
		ht[member_at(members, ht, members[i])] = (uint16) (i + 1);
	}
	for (int i = 0; n > 0 && i < p->n; i++)
	{
		SiftF2ScopeSlot *s = &p->s[i];

		if (s->part_gen == 0)
			continue;
		for (uint32 g = 0; g < s->ng; g++)
		{
			const SiftZone z = sift_f2s_slot_zone(p, s, p->sc->member_leaf, g);
			const uint16 *gone;
			SiftScopeRows r = {0};
			uint32		lo = 0,
						hi = n,
						ng;
			MemoryContext old;

			if ((!coded && (z.flags & SIFT_ZONE_NONE)) || !zone_meets_span(p, s, g, &span))
				continue;
			while (!coded && lo < hi)
			{
				uint32		mid = lo + (hi - lo) / 2;

				if (members[mid] < z.min)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (!coded && (lo == n || members[lo] > z.max))
				continue;
			ng = sift_f2s_granule_excl(p, s, g, &gone);
			MemoryContextReset(p->gcx);
			old = MemoryContextSwitchTo(p->gcx);
			sift_f2s_decode_cols(p, s, g, NULL, 0, SIFT_F2S_NEED_ALL, &r);
			MemoryContextSwitchTo(old);
			for (uint32 row = 0; row < r.n; row++)
			{
				uint16		slot;

				if (r.member_null[row] || r.member[row] < members[0] || r.member[row] > members[n - 1])
					continue;
				slot = ht[member_at(members, ht, r.member[row])];
				if (slot == 0 || sift_f2s_has_row(gone, ng, row))
					continue;
				if (!span.all && !sift_scope_span_has(&span, r.time[row], r.time_null[row]))
					continue;
				sift_scube_add_row(&out[slot - 1], &r, row, NULL);
			}
		}
	}
	for (uint32 i = 0; i < n; i++)
	{
		has[i] = out[i].row_count > 0;
		if (!has[i])
			sift_scube_free(&out[i]);
	}
}

bool
sift_f2s_member_cube(void *arg, int64 member, SiftScopeSpan span, SiftScopeCubeAcc *out)
{
	bool		has;

	sift_f2s_members_cubes(arg, &member, 1, span, out, &has);
	return has;
}
