/*
 * core/f2/manifest/rehome.c - which pages of the manifest lie at or above a block (core/f2/manifest.h): the pages of the part lists and of the directory that a commit of a partition
 * writes again, so that the partitions to commit for the pages to come down to the lowest free pages of the slabs are named without a page being written.
 */
#include "core/host.h"

#include "core/f2/manifest/manifest_int.h"
#include "core/page_io.h"

static BlockNumber
list_hi(Relation rel, const SiftPartitionEntry *e)
{
	BlockNumber hi = 0;

	for (BlockNumber blk = e->manifest_blk; blk != InvalidBlockNumber;)
	{
		Page		p = sift_mf_read_page(rel, blk, SIFT_PT_PARTLIST, SIFT_PARTLIST_MAGIC, e->manifest_gen, (uint32) e->part_id, NULL);

		hi = Max(hi, blk);
		blk = PLIST(p)->next_blk;
		pfree(p);
	}
	return hi;
}

static void
add_entry(SiftManifestRead *out, int *cap, const SiftPartitionEntry *e)
{
	if (out->n == *cap)
	{
		*cap = Max(8, *cap * 2);
		/* check-memory: allow the partitions that a page of the manifest at or above the block hangs from, which the partitions of the index bound */
		out->entries = out->n == 0 ? palloc(*cap * sizeof(*e)) : repalloc(out->entries, *cap * sizeof(*e));
	}
	out->entries[out->n++] = *e;
}

SiftManifestRead
sift_manifest_pages_from(Relation rel, BlockNumber lo)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	Tree	   *t = sift_mf_read_tree(rel, &meta);
	SiftManifestRead out = {0};
	int			cap = 0,
				leaf = 0;

	for (int j = 0; j < Max(t->nnodes, 1); j++)
	{
		bool		node_high = t->nnodes > 0 && t->nodes[j].blk >= lo;

		for (int k = 0; k < (t->nnodes > 0 ? t->nodes[j].n : 1); k++, leaf++)
		{
			BlockNumber leaf_blk = t->leaves[leaf].child_blk;
			Page		p = sift_mf_read_dir(rel, leaf_blk, 0, NULL);
			bool		path_high = leaf_blk >= lo || (k == 0 && node_high);

			for (int i = 0; i < PDIR(p)->nentries; i++)
				if ((i == 0 && path_high) || list_hi(rel, &LEAF(p)[i]) >= lo)
					add_entry(&out, &cap, &LEAF(p)[i]);
			pfree(p);
		}
	}
	if (out.n == 0 && t->has_top && t->top.blk >= lo)
	{
		Page		p = sift_mf_read_dir(rel, t->leaves[0].child_blk, 0, NULL);

		add_entry(&out, &cap, &LEAF(p)[0]);
		pfree(p);
	}
	return out;
}
