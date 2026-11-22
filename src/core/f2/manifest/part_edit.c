/*
 * core/f2/manifest/part_edit.c - the checks and edits of the parts of one partition that a commit applies (core/f2/manifest.c), and the read of the live entry of a partition.
 */
#include "core/host.h"

#include "col/f2/part_format.h"
#include "core/f2/manifest.h"
#include "core/f2/manifest/manifest_int.h"

bool
sift_manifest_find_entry(Relation rel, uint64 part_id, const uint8 *key_lo, SiftPartitionEntry *out)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, key_lo, NULL);

	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		return false;
	*out = mr.entries[0];
	return true;
}

int
sift_mf_part_gen_index(const SiftPartDesc *parts, int n, uint64 gen)
{
	for (int i = 0; i < n; i++)
		if (parts[i].part_gen == gen)
			return i;
	return -1;
}

/* A chain that patches a time column leaves the bounds of the parts unknown for it: the claim comes off every part, and the entry's bounds follow from the parts */
void
sift_mf_unclaim_patched_time(SiftPartDesc *parts, int n, const SiftColDeltaAnchor *a)
{
	uint16		patched = (uint16) (((a->flags & SIFT_CDLA_DIRTY_TIME0) != 0 ? SIFT_PART_FLAG_HAS_TIME0 : 0) | ((a->flags & SIFT_CDLA_DIRTY_TIME1) != 0 ? SIFT_PART_FLAG_HAS_TIME1 : 0));

	for (int i = 0; i < n; i++)
		parts[i].flags &= (uint16) ~patched;
}

/* An update replaces a descriptor as the caller prepared it: it must have been prepared from the descriptor the manifest holds now, or the blocks it frees were freed already */
static void
check_expected(Relation rel, const SiftPartDesc *now, const SiftPartDesc *expect)
{
	SIFT_INVARIANT(now->header_blk == expect->header_blk && now->dv_blk == expect->dv_blk && now->dv_gen == expect->dv_gen && now->col_delta_blk == expect->col_delta_blk,
				   "index \"%s\": the update of part %llu was prepared from a descriptor that is not the manifest's (header %u, expected %u; deletion vector %u, expected %u)",
				   RelationGetRelationName(rel), (unsigned long long) now->part_gen, now->header_blk, expect->header_blk, now->dv_blk, expect->dv_blk);
}

void
sift_mf_apply_updates(Relation rel, const SiftPartitionEntry *ent, SiftPartDesc *parts, int n, const SiftManifestEdit *e)
{
	for (int i = 0; i < e->nupdate; i++)
	{
		int			at = sift_mf_part_gen_index(parts, n, e->update_parts[i].part_gen);

		if (at < 0)
			ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift index \"%s\": partition %llu has no part of generation %llu to update", RelationGetRelationName(rel),
																   (unsigned long long) e->part_id, (unsigned long long) e->update_parts[i].part_gen)));
		sift_mf_check_added(rel, ent, &e->update_parts[i]);
		if (e->expect_parts != NULL)
			check_expected(rel, &parts[at], &e->expect_parts[i]);
		parts[at] = e->update_parts[i];
	}
}
