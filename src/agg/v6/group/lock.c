/*
 * agg/v6/group/lock.c - the locked group table of GROUP BY .. LIMIT K (agg/v6/group/lock.h).
 */
#include "postgres.h"

#include "agg/kern6/kern6.h"
#include "agg/v6/group/lock.h"

static bool
key0_of(const SiftSaltedHT *ht, const SiftAggSlot64 *s, bool packed, const SiftPackSpec *ps, bool wide, int64 *x)
{
	if (wide)
	{
		if (s->key.wide.key_nulls & 1)
			return false;
		memcpy(x, ht->arena + s->key.wide.arena_off, sizeof(*x));
		return true;
	}
	if (packed)
		return sift_pack_get(ps, &s->key, 0, x);
	*x = s->key.i64[0];
	return s->key.i64[1] == 0;
}

void
sift_lock_set_build(SiftLockSet *ls, const SiftSaltedHT *ht, bool packed, const SiftPackSpec *ps, bool int_key0)
{
	const SiftAggSlot64 *s;

	ls->built = true;
	ls->usable = int_key0;
	ls->n = 0;
	SIFT_HT_FOREACH(ht, s)
	{
		int64		x;
		int			i = 0;

		if (!ls->usable)
			return;
		if (!key0_of(ht, s, packed, ps, ht->key_is_wide, &x))
			ls->usable = false;
		else
		{
			while (i < ls->n && ls->v[i] != x)
				i++;
			if (i == ls->n && ls->n == SIFT_LOCK_SET_MAX)
				ls->usable = false;
			else if (i == ls->n)
				ls->v[ls->n++] = x;
		}
	}
}

uint16
sift_lock_set_rows(const SiftLockSet *ls, const SiftVec *k0, const uint64 *mask, uint16 n_raw, uint64 *out)
{
	sift_aggk6->in_set_i64(out, (const int64 *) k0->data, n_raw, ls->v, ls->n);
	if (!k0->all_valid)
		sift_aggk6->mask_and(out, out, k0->valid, n_raw);
	return sift_aggk6->mask_and(out, out, mask, n_raw);
}

uint16
sift_lock_compact(uint16 *sel, SiftAggSlot64 **slots, uint16 n)
{
	uint16		m = 0;

	for (uint16 k = 0; k < n; k++)
		if (slots[k] != NULL)
		{
			sel[m] = sel[k];
			slots[m++] = slots[k];
		}
	return m;
}
