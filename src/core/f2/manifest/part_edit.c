/*
 * core/f2/manifest/part_edit.c - the checks and edits of the parts of one partition that a commit applies (core/f2/manifest/manifest.c), and the read of the live entry of a partition.
 */
#include "core/host.h"

#include "col/f2/part/format.h"
#include "core/f2/manifest/manifest.h"
#include "core/f2/manifest/int.h"

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

/* Every edit of a sift_table that adds parts names their key */
void
sift_mf_note_key(Relation rel, Change *ch, const SiftManifestEdit *e)
{
	if (e->nadd == 0)
		return;
	SIFT_INVARIANT(e->key_sig != 0 || rel->rd_rel->relkind == RELKIND_INDEX, "sift_table \"%s\": an edit adds parts in no key", RelationGetRelationName(rel));
	sift_mf_note_key_sig(ch, e->key_sig);
}

void
sift_mf_note_key_sig(Change *ch, uint32 sig)
{
	if (sig == 0)
		return;
	ch->key_mixed |= ch->key_sig != 0 && sig != ch->key_sig;
	ch->key_sig = sig;
}

void
sift_mf_note_list(Change *ch, uint64 least_gen)
{
	ch->key_least_gen = ch->key_lists++ == 0 ? least_gen : Min(ch->key_least_gen, least_gen);
}

uint64
sift_mf_least_gen(const SiftPartDesc *parts, int n)
{
	uint64		least = PG_UINT64_MAX;

	for (int i = 0; i < n; i++)
		least = Min(least, parts[i].part_gen);
	return least;
}

static bool
key_moves(const SiftColMetaPageData *m, const Change *ch)
{
	return ch->key_sig != 0 && (m->active_key_order_hash != ch->key_sig || ch->key_mixed);
}

/*
 * The generation a commit that moves the key starts it at, taken once the manifest is read and before Block 0 is locked: the generations are handed out in order, so every part of a commit
 * before it has a smaller one (a commit that comes in between moves the version, and the publish is done again)
 */
void
sift_mf_key_cut(Relation rel, const SiftColMetaPageData *read, Change *ch)
{
	if (key_moves(read, ch) && (read->active_key_order_hash != 0 || ch->key_mixed))
		ch->key_cut = sift_manifest_new_gen(rel, 1);
}

/*
 * A commit that adds parts of another key than the metapage names makes it that key's from its cut on (a first commit says the parts before it are in its key). A commit that writes again
 * the list of every partition, when each part it lists is of the cut or above, says every part is in the key.
 */
void
sift_mf_stamp_key(SiftColMetaPageData *m, const Change *ch)
{
	if (key_moves(m, ch))
	{
		SIFT_INVARIANT(ch->key_cut != 0 || (m->active_key_order_hash == 0 && !ch->key_mixed), "the commit moves the key of the parts without a cut");
		m->key_order_change_gen = ch->key_cut;
		m->active_key_order_hash = ch->key_sig;
	}
	else if (m->key_order_change_gen != 0 && ch->key_lists == (int) m->npartitions && ch->key_least_gen >= m->key_order_change_gen)
		m->key_order_change_gen = 0;
}
