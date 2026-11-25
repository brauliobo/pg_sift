/*
 * core/f2/manifest/tree_io.c - the partition directory as the manifest reads it: a page at a time, binary searched inside the page, and down a single path to the partition of a point
 * key (core/f2/manifest/manifest.c builds the changes of it).
 */
#include "core/host.h"

#include "col/f2/part/format.h"
#include "core/f2/manifest/manifest.h"
#include "core/f2/manifest/int.h"
#include "core/page_io.h"

Page
sift_mf_read_dir(Relation rel, BlockNumber blk, uint16 level, int *pages)
{
	Page		p = sift_mf_read_page(rel, blk, SIFT_PT_PARTDIR, SIFT_PARTDIR_MAGIC, 0, 0, pages);
	size_t		cap = level == 0 ? SIFT_PARTDIR_LEAF_CAP : SIFT_PARTDIR_INT_CAP;

	if (PDIR(p)->level != level || PDIR(p)->nentries == 0 || PDIR(p)->nentries > cap)
		SIFT_CORRUPT(rel, "Block %u: a partition directory page of level %u holds %u entries, expected level %u.", blk, PDIR(p)->level, PDIR(p)->nentries, level);
	return p;
}

OldNode
sift_mf_read_node(Relation rel, BlockNumber blk, uint16 level, int *pages)
{
	Page		p = sift_mf_read_dir(rel, blk, level, pages);

	return (OldNode) {.blk = blk,.n = PDIR(p)->nentries,.keys = INTK(p)};
}

Tree *
sift_mf_read_tree(Relation rel, const SiftColMetaPageData *meta)
{
	Tree	   *t = palloc0(sizeof(Tree));
	int			total = 0;

	t->meta = *meta;
	if (meta->partdir_level == 0)
	{
		t->leaves = palloc(sizeof(SiftPartDirKey));
		t->leaves[0] = (SiftPartDirKey) {.child_blk = meta->partdir_root_blk,.npartitions = meta->npartitions};
		t->nleaves = 1;
		return t;
	}
	if (meta->partdir_level == 1)
	{
		t->nodes = palloc(sizeof(OldNode));
		t->nodes[t->nnodes++] = sift_mf_read_node(rel, meta->partdir_root_blk, 1, &t->pages);
	}
	else
	{
		t->top = sift_mf_read_node(rel, meta->partdir_root_blk, 2, &t->pages);
		t->has_top = true;
		t->nodes = palloc(t->top.n * sizeof(OldNode));
		for (int i = 0; i < t->top.n; i++)
			t->nodes[t->nnodes++] = sift_mf_read_node(rel, t->top.keys[i].child_blk, 1, &t->pages);
	}
	for (int i = 0; i < t->nnodes; i++)
		total += t->nodes[i].n;
	/* check-memory: allow the leaf references of the directory pages, at most what three levels hold */
	t->leaves = palloc(total * sizeof(SiftPartDirKey));
	for (int i = 0; i < t->nnodes; i++)
	{
		memcpy(t->leaves + t->nleaves, t->nodes[i].keys, t->nodes[i].n * sizeof(SiftPartDirKey));
		t->nleaves += t->nodes[i].n;
	}
	return t;
}

/*
 * The tree down the one path to the leaf that holds key_lo: the level 2 page, if there is one, and the interior page under it that the key falls in. A change that keeps the number of leaves
 * (a commit of one partition) rewrites only these two pages, so it reads no other; a level 0 tree has no interior page, and a level 1 tree is its one page, so those are read whole.
 */
Tree *
sift_mf_read_tree_path(Relation rel, const SiftColMetaPageData *meta, const uint8 *key_lo)
{
	Tree	   *t;

	if (meta->partdir_level < 2)
		return sift_mf_read_tree(rel, meta);
	t = palloc0(sizeof(Tree));
	t->meta = *meta;
	t->partial = true;
	t->top = sift_mf_read_node(rel, meta->partdir_root_blk, 2, &t->pages);
	t->has_top = true;
	t->top_pos = sift_mf_int_search(t->top.keys, t->top.n, key_lo);
	t->nodes = palloc(sizeof(OldNode));
	t->nodes[t->nnodes++] = sift_mf_read_node(rel, t->top.keys[t->top_pos].child_blk, 1, &t->pages);
	t->leaves = palloc(t->nodes[0].n * sizeof(SiftPartDirKey));
	memcpy(t->leaves, t->nodes[0].keys, t->nodes[0].n * sizeof(SiftPartDirKey));
	t->nleaves = t->nodes[0].n;
	return t;
}

/* The leaf whose range holds the key: the last leaf whose first key is not above it */
int
sift_mf_find_leaf(const Tree *t, const uint8 *key_lo)
{
	int			lo = 0,
				hi = t->nleaves;

	while (hi - lo > 1)
	{
		int			mid = lo + (hi - lo) / 2;

		if (bound_cmp(t->leaves[mid].key_lo, key_lo) <= 0)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* The partition entry the point key_lo falls in, read down one page at each level (no page of the directory is held, none of another branch is read); false where that partition is not part_id */
bool
sift_mf_find_entry_at(Relation rel, const SiftColMetaPageData *meta, uint64 part_id, const uint8 *key_lo, SiftPartitionEntry *out, int *pages)
{
	BlockNumber blk = meta->partdir_root_blk;

	for (int level = meta->partdir_level;; level--)
	{
		Page		p = sift_mf_read_dir(rel, blk, level, pages);
		const SiftPartitionEntry *e;
		bool		found;

		if (level > 0)
		{
			blk = INTK(p)[sift_mf_int_search(INTK(p), PDIR(p)->nentries, key_lo)].child_blk;
			pfree(p);
			continue;
		}
		e = &LEAF(p)[sift_mf_leaf_search(LEAF(p), PDIR(p)->nentries, key_lo)];
		found = e->part_id == part_id;
		if (found)
			*out = *e;
		pfree(p);
		return found;
	}
}

/* The first leaf entry whose range ends above lo (NULL: the first): the entries tile the key space, so that is the one that holds lo */
static int
leaf_first(const SiftPartitionEntry *ents, int n, const uint8 *lo)
{
	return lo == NULL ? 0 : sift_mf_leaf_search(ents, n, lo);
}

void
sift_mf_collect_range(Relation rel, BlockNumber blk, int level, const uint8 *lo, const uint8 *hi, SiftManifestRead *out, int *cap)
{
	Page		p = sift_mf_read_dir(rel, blk, level, &out->pages);
	int			n = PDIR(p)->nentries;

	if (level == 0)
	{
		for (int i = leaf_first(LEAF(p), n, lo); i < n; i++)
		{
			const SiftPartitionEntry *e = &LEAF(p)[i];

			if (hi != NULL && !bound_inf(hi) && bound_cmp(e->key_lo, hi) >= 0)
				break;
			if (!sift_manifest_overlaps(e->key_lo, e->key_hi, lo, hi))
				continue;
			if (out->n == *cap)
			{
				*cap *= 2;
				out->entries = repalloc(out->entries, *cap * sizeof(SiftPartitionEntry));
			}
			out->entries[out->n++] = *e;
		}
		return;
	}
	for (int i = lo == NULL ? 0 : sift_mf_int_search(INTK(p), n, lo); i < n; i++)
	{
		const SiftPartDirKey *k = &INTK(p)[i];

		if (hi != NULL && !bound_inf(hi) && bound_cmp(k->key_lo, hi) >= 0)
			break;
		sift_mf_collect_range(rel, k->child_blk, level - 1, lo, hi, out, cap);
	}
}
