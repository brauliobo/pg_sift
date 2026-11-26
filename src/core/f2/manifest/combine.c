/*
 * core/f2/manifest/combine.c - the commits of the partitions of one relation that wait for Block 0 at the same time are one change of the directory (core/manifest_combiner.h).
 *
 * A writer holds the lock of its partition, has written the pages of the list of the partition and built the new entry of the partition; where nothing else of the manifest changes with it
 * (no scope, projection, tier or column delta directory, no split) it hands the entry and the extents it frees to the combiner of the relation. The one that finds the queue free applies the
 * entries of all that wait: the directory is read once, the leaves that hold the entries are written once, Block 0 is locked once, and a single record and a single publication to the registry
 * make them the manifest. The entries are the writers' to give: each partition is locked by its writer until the commit returns, so no other change of the partition and no split or merge
 * that moves its bounds can come between; a change of the directory that does not go through the combiner moves the version, and the leader reads the directory again, as a commit does.
 */
#include "core/host.h"

#include <limits.h>

#include "access/transam.h"

#include "col/dsmreg.h"
#include "core/f2/manifest/int.h"
#include "core/manifest_combiner.h"

#define MC_NAME		"sift_shm_mc"

StaticAssertDecl(sizeof(SiftPartitionEntry) == SIFT_MC_ENTRY_BYTES, "the entry of a partition is what the combiner carries");

typedef struct McTable
{
	int			tranche;
	SiftManifestCommitCombiner mc;
} McTable;

static McTable *mc_table;
static bool mc_tried;

static McTable *
mc_base(void *raw)
{
	return (McTable *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
mc_init(SIFT_DSM_INIT_ARGS)
{
	McTable    *t = mc_base(ptr);

	memset(t, 0, sizeof(*t));
	t->tranche = sift_tranche_new(MC_NAME);
	sift_mc_init(&t->mc, t->tranche);
}

/* NULL where the system has no room for the table: the commits are then published one by one */
static McTable *
mc_get(void)
{
	if (mc_table == NULL && !mc_tried)
	{
		/* MEM[MEM-SHM-11]: DSM manifest commit combiner sift_shm_mc (fixed; 64 queue lines and 256 slots of 448 B) */
		void	   *raw = sift_dsm_try(MC_NAME, sizeof(McTable) + PG_CACHE_LINE_SIZE, mc_init);

		mc_tried = true;
		if (raw != NULL)
		{
			mc_table = mc_base(raw);
			sift_tranche_register(mc_table->tranche, MC_NAME);
		}
	}
	return mc_table;
}

/* A change of one entry and the blocks it frees: nothing else of the manifest moves with it */
static bool
entry_only(const Change *ch)
{
	return !ch->set_scopes && !ch->set_projs && !ch->set_tier && !ch->set_cd_dir && ch->nproj_release == 0 && ch->d_partitions == 0 &&
		ch->old.n + ch->nrelease <= SIFT_MC_MAX_RELEASE;
}

/* Under the queue lock, once for the batch: the directory read, the entries put in their leaves, the record made; the version it made */
static uint64
publish_batch(void *ctx, const SiftManifestEntryDelta *const *deltas, uint32 n)
{
	Relation	rel = ctx;
	SiftExtent	freed[SIFT_MC_MAX_BATCH * SIFT_MC_MAX_RELEASE];

	for (;;)
	{
		SiftColMetaPageData meta = sift_col_meta_read(rel);
		Tree	   *t = sift_mf_read_tree(rel, &meta);
		Change		ch = {0};
		BlkVec		created = {0};
		int			lo = INT_MAX,
					hi = -1,
					nfreed = 0;
		uint64		version;

		for (uint32 i = 0; i < n; i++)
		{
			SiftPartitionEntry after;
			int			leaf;

			memcpy(&after, deltas[i]->entry, sizeof(after));
			leaf = sift_mf_find_leaf(t, after.key_lo);
			lo = Min(lo, leaf);
			hi = Max(hi, leaf);
		}
		sift_mf_read_leaves(rel, t, lo, hi - lo + 1, &ch, 2);
		for (uint32 i = 0; i < n; i++)
		{
			SiftPartitionEntry after,
					   *at;
			SiftPartitionEntry before;
			FullTransactionId min_xid = FullTransactionIdFromU64(deltas[i]->min_xid);

			memcpy(&after, deltas[i]->entry, sizeof(after));
			at = &ch.ents[sift_mf_find_entry_idx(&ch, after.part_id, after.key_lo)];
			before = *at;
			*at = after;
			sift_mf_account(&ch, &before, at);
			ch.new_min_xid = sift_mf_older_active(ch.new_min_xid, min_xid);
			for (uint32 j = 0; j < deltas[i]->nrelease; j++)
				freed[nfreed++] = (SiftExtent) {deltas[i]->release[j].start, deltas[i]->release[j].nblocks};
		}
		ch.release = freed;
		ch.nrelease = nfreed;
		if (sift_mf_publish(rel, t, &ch, &created, &version))
			return version;
		sift_mf_free_created(rel, &created);
	}
}

bool
sift_mf_commit_combined(Relation rel, Change *ch, uint64 part_id, const uint8 *key_lo, uint64 *version)
{
	McTable    *t = entry_only(ch) ? mc_get() : NULL;
	SiftManifestEntryDelta d = {.part_id = part_id,.min_xid = U64FromFullTransactionId(ch->new_min_xid)};

	if (t == NULL)
		return false;
	memcpy(d.entry, &ch->ents[sift_mf_find_entry_idx(ch, part_id, key_lo)], sizeof(SiftPartitionEntry));
	for (int i = 0; i < ch->old.n; i++)
		d.release[d.nrelease++] = (SiftManifestExtent) {ch->old.v[i], 1};
	for (int i = 0; i < ch->nrelease; i++)
		d.release[d.nrelease++] = (SiftManifestExtent) {ch->release[i].start, ch->release[i].nblocks};
	*version = sift_mc_commit(&t->mc, &rel->rd_locator, &d, publish_batch, rel);
	return true;
}
