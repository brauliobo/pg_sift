/*
 * core/manifest/tierdir.c - the tier directory of the manifest of format major 2: a chain of pages of refs in ascending part_gen (core/f2/tier.h), copy on write like the scope directory. A commit of
 * the manifest that moves a part to an object, or drops one, writes a new chain with the edit applied, in the same record that swaps the part list and names the chain in the metapage: a crash
 * before the record leaves the old chain, the old part list and the local blocks of the part; a crash after leaves the object named. The pages of the old chain are released with the commit.
 */
#include "core/host.h"

#include "col/f2/col_meta.h"
#include "core/f2/manifest/manifest_int.h"
#include "core/page_io.h"
#include "storage/standby.h"
#include "utils/timestamp.h"

#define TDIR(p)		((SiftTierDirPageData *) SiftPageGetData(p))

static SiftTierPartRef *
read_tier_chain(Relation rel, const SiftColMetaPageData *meta, int *ntier, BlkVec *blocks, int *pages)
{
	SiftTierPartRef *out;
	int			n = 0;

	*ntier = 0;
	if (meta->tier_dir_blk == InvalidBlockNumber)
	{
		if (meta->ntier_parts != 0)
			SIFT_CORRUPT(rel, "The metapage counts %u parts in object storage and names no tier directory.", meta->ntier_parts);
		return NULL;
	}
	if (meta->ntier_parts == 0 || meta->ntier_parts > SIFT_TIER_MAX_REFS)
		SIFT_CORRUPT(rel, "The metapage counts %u parts in object storage.", meta->ntier_parts);
	out = palloc(meta->ntier_parts * sizeof(SiftTierPartRef));
	for (BlockNumber blk = meta->tier_dir_blk; blk != InvalidBlockNumber;)
	{
		Page		p = sift_mf_read_page(rel, blk, SIFT_PT_TIERDIR, SIFT_TIERDIR_MAGIC, meta->tier_dir_gen, 0, pages);
		const SiftTierDirPageData *d = TDIR(p);
		const char *why = sift_tier_page_check(p, meta->tier_dir_gen);

		if (why != NULL || n + d->nentries > meta->ntier_parts)
			SIFT_CORRUPT(rel, "Block %u: %s (the metapage counts %u refs in all).", blk, why != NULL ? why : "the tier directory holds more refs", meta->ntier_parts);
		memcpy(out + n, d->refs, d->nentries * sizeof(SiftTierPartRef));
		n += d->nentries;
		if (blocks != NULL)
			sift_mf_blk_add(blocks, blk);
		blk = d->next_blk;
	}
	if (n != (int) meta->ntier_parts)
		SIFT_CORRUPT(rel, "The tier directory holds %d refs, the metapage says %u.", n, meta->ntier_parts);
	if (sift_tier_refs_check(out, n) != NULL)
		SIFT_CORRUPT(rel, "The tier directory is damaged: %s.", sift_tier_refs_check(out, n));
	*ntier = n;
	return out;
}

SiftTierPartRef *
sift_manifest_read_tier(Relation rel, const SiftColMetaPageData *meta, int *ntier)
{
	return read_tier_chain(rel, meta, ntier, NULL, NULL);
}

static BlockNumber
write_tierdir(Relation rel, uint64 gen, uint64 version, const SiftTierPartRef *refs, int n, BlkVec *created)
{
	int			npages = (n + SIFT_TIERDIR_CAP - 1) / SIFT_TIERDIR_CAP;
	BlockNumber *blks = palloc(npages * sizeof(BlockNumber));
	PGAlignedBlock img;
	BlockNumber head;

	for (int p = 0; p < npages; p++)
		blks[p] = sift_mf_new_page_blk(rel, created);
	for (int p = npages - 1; p >= 0; p--)
	{
		int			from = p * SIFT_TIERDIR_CAP;

		sift_tier_page_fill(img.data, gen, version, p + 1 < npages ? blks[p + 1] : InvalidBlockNumber, refs + from, Min(n - from, (int) SIFT_TIERDIR_CAP));
		sift_mf_write_image(rel, blks[p], img.data);
	}
	head = blks[0];
	pfree(blks);
	return head;
}

/* The tier fields of the metapage after the commit; the format says it holds parts in object storage while it has refs */
void
sift_mf_apply_tier(SiftColMetaPageData *m, const Change *ch)
{
	m->tier_dir_blk = ch->tier_dir_blk;
	m->ntier_parts = ch->ntier_parts;
	m->tier_dir_gen = ch->tier_dir_gen;
	m->common.compat_flags = ch->ntier_parts > 0 ? (m->common.compat_flags | SIFT_COMPAT_REMOTE_PARTS) : (m->common.compat_flags & ~SIFT_COMPAT_REMOTE_PARTS);
}

/* The longest a standby may be behind the primary in replaying the drop of a part before its object must still be there */
static int64
delete_grace_us(void)
{
	return max_standby_streaming_delay < 0 ? SIFT_TIER_DELETE_GRACE_US * 24 : Max(SIFT_TIER_DELETE_GRACE_US, (int64) max_standby_streaming_delay * 1000);
}

/*
 * The directory the metapage names with the edit applied, written as a new chain (none when no ref is left); the pages it replaces are released with the commit. expire_gens are the parts the same
 * commit removes from the part list that were in object storage: their refs stay, waiting for the deletion of their objects.
 */
static void
build_tier_edit(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, const uint64 *expire_gens, int nexpire, BlkVec *created)
{
	int			n;
	SiftTierPartRef *cur = read_tier_chain(rel, &t->meta, &n, &ch->old, &t->pages);
	SiftTierPartRef *set = palloc((e->nset_tier + nexpire + 1) * sizeof(SiftTierPartRef));
	SiftTierPartRef *all = palloc((n + e->nset_tier + nexpire + 1) * sizeof(SiftTierPartRef));
	TimestampTz now = GetCurrentTimestamp();
	int			nset = e->nset_tier;
	int			nall;
	const char *why;

	if (e->nset_tier > 0)
		memcpy(set, e->set_tier, e->nset_tier * sizeof(SiftTierPartRef));
	for (int i = 0; i < nexpire; i++)
	{
		const SiftTierPartRef *live = sift_tier_refs_find(cur, n, expire_gens[i]);

		if (live == NULL || (live->flags & SIFT_TIER_DELETE_PENDING))
			SIFT_CORRUPT(rel, "Part %llu is in object storage and the tier directory has no live ref for it.", (unsigned long long) expire_gens[i]);
		set[nset++] = sift_tier_ref_expire(live, now, delete_grace_us());
	}
	nall = sift_tier_refs_apply(all, cur, n, set, nset, e->drop_tier_gens, e->ndrop_tier);
	why = nall < 0 ? "a ref is added twice or a ref that is dropped is not there" : nall > (int) SIFT_TIER_MAX_REFS ? "the directory would hold too many refs" : sift_tier_refs_check(all, nall);
	if (why != NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": the edit of the tier directory is not valid", RelationGetRelationName(rel)), errdetail("%s.", why)));
	ch->set_tier = true;
	ch->ntier_parts = nall;
	ch->tier_dir_gen = sift_manifest_new_gen(rel, 1);
	ch->tier_dir_blk = nall > 0 ? write_tierdir(rel, ch->tier_dir_gen, t->meta.manifest_version + 1, all, nall, created) : InvalidBlockNumber;
	pfree(set);
	pfree(all);
}

void
sift_mf_build_tier(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, const uint64 *expire_gens, int nexpire, BlkVec *created)
{
	if (e->nset_tier > 0 || e->ndrop_tier > 0 || nexpire > 0)
		build_tier_edit(rel, t, ch, e, expire_gens, nexpire, created);
}
