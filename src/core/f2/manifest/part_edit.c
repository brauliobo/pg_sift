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
	SiftPartitionEntry *inl;
	int			ninl;
	SiftColMetaPageData meta = sift_col_meta_read_inline(rel, &inl, &ninl);

	if (inl == NULL)
		return sift_mf_find_entry_at(rel, &meta, part_id, key_lo, out, NULL);
	*out = inl[sift_mf_leaf_search(inl, ninl, key_lo)];
	return out->part_id == part_id;
}

/* An update replaces a descriptor as the caller prepared it: it must have been prepared from the descriptor the manifest holds now, or the blocks it frees were freed already */
static void
check_expected(Relation rel, const SiftPartDesc *now, const SiftPartDesc *expect)
{
	SIFT_INVARIANT(now->header_blk == expect->header_blk && now->dv_blk == expect->dv_blk && now->dv_gen == expect->dv_gen && now->col_delta_blk == expect->col_delta_blk,
				   "index \"%s\": the update of part %llu was prepared from a descriptor that is not the manifest's (header %u, expected %u; deletion vector %u, expected %u)",
				   RelationGetRelationName(rel), (unsigned long long) now->part_gen, now->header_blk, expect->header_blk, now->dv_blk, expect->dv_blk);
}

/*
 * The parts of the edit's drop list leave parts[0..n): each is looked up by its generation in a map made once, marked, and the array is closed over them in one pass; returns the parts left. The
 * generations among them that are in object storage are *expire (NULL when none), their refs stay, waiting for the deletion of their objects
 */
int
sift_mf_drop_parts(Relation rel, SiftPartDesc *parts, int n, const SiftManifestEdit *e, uint64 **expire, int *nexpire)
{
	SiftGenMap	map;
	bool	   *gone;
	int			kept = 0;

	*expire = NULL;
	*nexpire = 0;
	if (e->ndrop == 0)
		return n;
	sift_gen_map_init(&map, parts, n, 0);
	/* check-memory: allow a flag for each part of one partition */
	gone = palloc0(Max(n, 1) * sizeof(bool));
	for (int i = 0; i < e->ndrop; i++)
	{
		int			at = sift_gen_map_find(&map, e->drop_gens[i]);

		if (at < 0 || gone[at])
			ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift index \"%s\": partition %llu has no part of generation %llu", RelationGetRelationName(rel),
																   (unsigned long long) e->part_id, (unsigned long long) e->drop_gens[i])));
		gone[at] = true;
		if (parts[at].ext_flags & SIFT_PART_EXT_REMOTE)
		{
			*expire = *expire ? *expire : palloc(e->ndrop * sizeof(uint64));
			(*expire)[(*nexpire)++] = e->drop_gens[i];
		}
	}
	for (int i = 0; i < n; i++)
		if (!gone[i])
			parts[kept++] = parts[i];
	sift_gen_map_free(&map);
	pfree(gone);
	return kept;
}

void
sift_mf_apply_updates(Relation rel, const SiftPartitionEntry *ent, SiftPartDesc *parts, const SiftGenMap *map, const SiftManifestEdit *e)
{
	for (int i = 0; i < e->nupdate; i++)
	{
		int			at = sift_gen_map_find(map, e->update_parts[i].part_gen);

		if (at < 0)
			ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift index \"%s\": partition %llu has no part of generation %llu to update", RelationGetRelationName(rel),
																   (unsigned long long) e->part_id, (unsigned long long) e->update_parts[i].part_gen)));
		sift_mf_check_added(rel, ent, &e->update_parts[i]);
		if (e->expect_parts != NULL)
			check_expected(rel, &parts[at], &e->expect_parts[i]);
		parts[at] = e->update_parts[i];
	}
}
