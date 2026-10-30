/*
 * agg/group/gids.c - the group ids of the tuples of a chunk
 * The hash of the key ids of a tuple and the group id of each selected tuple of the current
 * grouping set.
 */
#include "postgres.h"

#include "agg/group/int.h"

/* ids_hash32 of every tuple of the chunk's key ids (kid, as compute_kids lays them out), key by key */
static void
hash_cols(const uint32 *kid, int nk, int nsel, uint32 *hv)
{
	memset(hv, 0, nsel * sizeof(uint32));
	for (int j = 0; j < nk; j++)
	{
		const uint32 *col = kid + (Size) j * (VEC + PADW);

		for (int i = 0; i < nsel; i++)
		{
			uint32		h = (hv[i] ^ col[i]) * IDS_HASH_MUL;

			hv[i] = h ^ (h >> 16);
		}
	}
}

void
compute_gids(AggExec *ex, int nsel)
{
	int			nk = ex->spec->nkeys;
	uint32		hv[VEC + PADW];

	if (nk == 0)
	{
		memset(ex->gid, 0, nsel * sizeof(uint32));
		return;
	}
	if (!ex->hash)
	{
		uint32	   *maps[SIFT_AGG_MAX_KEYS];
		int32		bases[SIFT_AGG_MAX_KEYS];
		uint32	   *vals[SIFT_AGG_MAX_KEYS];
		bool		grow = false;

		/* make every id first: capacity checks need the final counts */
		for (int j = 0; j < nk; j++)
		{
			const uint32 *idx = NULL;

			if (key_lf(ex, j))
				gather_idx(ex, ex->spec->key[j].leaf, nsel, &idx);
			vals[j] = ex->kids + (Size) j * (VEC + PADW);
			key_prepare(ex, j, nsel, idx, &maps[j], &bases[j], vals[j]);
			if (key_used(&ex->ks[j]) > ex->ks[j].cap)
				grow = true;
		}
		if (grow)
			direct_grow(ex);
		if (!ex->hash)
		{
			memset(ex->gid, 0, nsel * sizeof(uint32));
			for (int j = 0; j < nk; j++)
			{
				const SiftSrcLeaf *lf = key_lf(ex, j);

				if (maps[j] == NULL)
					for (int i = 0; i < nsel; i++)
						ex->gid[i] += vals[j][i] * ex->ks[j].mult;
				else if (lf->vtype == SCV_CODE)
					sift_aggk->gid_code(ex->gid, vals[j], maps[j], nsel, ex->ks[j].mult);
				else
				{
					sift_aggk->gid_i32(ex->gid, (const int32 *) vals[j], bases[j], maps[j], nsel, ex->ks[j].mult);
					key_null_fix(ex, j, nsel, vals[j], ex->gid, ex->ks[j].mult, NULL);
				}
			}
			return;
		}
		/* the table just replaced the arrays: recompute below, in hash mode */
	}
	compute_kids(ex, nsel, ex->tmpu32);
	/* the slot of a tuple is prefetched GROUP_AHEAD tuples early */
	hash_cols(ex->tmpu32, nk, nsel, hv);
	for (int i = 0; i < Min(GROUP_AHEAD, nsel); i++)
		__builtin_prefetch(&ex->hslot[hv[i] & (ex->nhslot - 1)]);
	for (int i = 0; i < nsel; i++)
	{
		uint32		ids[SIFT_AGG_MAX_KEYS];

		if (i + GROUP_AHEAD < nsel)
			__builtin_prefetch(&ex->hslot[hv[i + GROUP_AHEAD] & (ex->nhslot - 1)]);
		for (int j = 0; j < nk; j++)
			ids[j] = ex->tmpu32[(Size) j * (VEC + PADW) + i];
		ex->gid[i] = ex->hashgh(ex, ids, hv[i]);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}

/*
 * The group ids of the current grouping set in hash mode from the key ids of the chunk (kid: compute_kids with every key): a key outside the set is
 * NULL (id 0) and the key of the sets is the set's number, as key_prepare gives them, so the sets that read the chunk's own tuples prepare the keys once.
 */
void
compute_set_gids(AggExec *ex, int nsel, const uint32 *kid)
{
	int			nk = ex->spec->nkeys;
	const uint32 *col[SIFT_AGG_MAX_KEYS];
	uint32		fixed[SIFT_AGG_MAX_KEYS];

	for (int j = 0; j < nk; j++)
	{
		bool		in = (ex->arm_mask >> j & 1) != 0;

		col[j] = in && ex->spec->key[j].kind != SAK_SET ? kid + (Size) j * (VEC + PADW) : NULL;
		fixed[j] = in ? ex->curset + 1 : 0;
	}
	for (int i = 0; i < nsel; i++)
	{
		uint32		ids[SIFT_AGG_MAX_KEYS];

		for (int j = 0; j < nk; j++)
			ids[j] = col[j] ? col[j][i] : fixed[j];
		ex->gid[i] = hash_group(ex, ids);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}
