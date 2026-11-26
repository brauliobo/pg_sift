/*
 * core/manifest/manifest_int.h - what the files of the manifest share: the vectors of blocks, the directory as read, a change of it and the page helpers (core/manifest.c).
 */
#ifndef SIFT_MANIFEST_INT_H
#define SIFT_MANIFEST_INT_H

#include "core/host.h"

#include "col/f2/meta.h"
#include "core/f2/manifest/manifest.h"
#include "core/f2/meta.h"

typedef struct BlkVec
{
	BlockNumber *v;
	int			n;
	int			max;
} BlkVec;

/* The directory as it is at one version: the references to its leaves in key order, and the interior pages above them */
typedef struct OldNode
{
	BlockNumber blk;
	int			n;
	SiftPartDirKey *keys;
	bool		kept;
} OldNode;

typedef struct Tree
{
	SiftColMetaPageData meta;
	SiftPartDirKey *leaves;
	int			nleaves;
	OldNode    *nodes;			/* the interior pages the leaves hang from */
	int			nnodes;
	OldNode		top;			/* the level 2 root, when there is one */
	bool		has_top;
	int			pages;
	bool		partial;		/* read down one path (sift_mf_read_tree_path): leaves are those of the one interior page nodes[0], which top.keys[top_pos] refers to */
	int			top_pos;
} Tree;

/* A change of the directory: the leaves [first, first + span) are replaced by the ones that hold ents, and the totals of the metapage move by the deltas */
typedef struct Change
{
	int			first;
	int			span;
	SiftPartitionEntry *ents;
	int			nents;
	int64		d_rows;
	int64		d_dead;
	int64		d_bytes;
	int			d_partitions;
	BlkVec		old;			/* the pages of the lists the change replaces */
	const SiftExtent *release;
	int			nrelease;
	bool		set_scopes;		/* the scope directory is replaced: the head page, the refs in all and the generation of the new chain */
	BlockNumber scope_dir_blk;
	uint32		nscopes;
	uint64		scope_dir_gen;
	bool		set_projs;		/* the projection directory is replaced (core/f2/manifest/projdir.c) */
	BlockNumber proj_dir_blk;
	uint32		nprojs;
	uint64		proj_dir_gen;
	SiftExtent *proj_release;	/* the extents of the projection parts that leave with the commit */
	int			nproj_release;
	int			proj_release_cap;
	bool		set_tier;		/* the tier directory is replaced: the head page, the refs in all and the generation of the new chain */
	BlockNumber tier_dir_blk;
	uint32		ntier_parts;
	uint64		tier_dir_gen;
	bool		set_cd_dir;		/* a partition's column delta anchor names a SIFT_PT_COL_DELTA_DIR: the metapage says so for every reader (SIFT_COMPAT_COL_DELTA_DIR) */
	FullTransactionId new_min_xid;
} Change;

#define PLIST(p)	((SiftPartListPageData *) SiftPageGetData(p))
#define PDIR(p)		((SiftPartDirPageData *) SiftPageGetData(p))
#define LEAF(p)		((SiftPartitionEntry *) (SiftPageGetData(p) + sizeof(SiftPartDirPageData)))
#define INTK(p)		((SiftPartDirKey *) (SiftPageGetData(p) + sizeof(SiftPartDirPageData)))

static const uint8 sift_mf_zero_bound[SIFT_BOUND_BYTES];

static inline bool
bound_inf(const uint8 *hi)
{
	return memcmp(hi, sift_mf_zero_bound, SIFT_BOUND_BYTES) == 0;
}

static inline int
bound_cmp(const uint8 *a, const uint8 *b)
{
	return memcmp(a, b, SIFT_BOUND_BYTES);
}

/* The last entry of a page, in key order, whose lower bound is not above key: the one whose range holds it (0 for a key under the first, which holds the lowest range) */
static inline int
sift_mf_leaf_search(const SiftPartitionEntry *ents, int n, const uint8 *key)
{
	int			lo = 0,
				hi = n;

	while (hi - lo > 1)
	{
		int			mid = lo + (hi - lo) / 2;

		if (bound_cmp(ents[mid].key_lo, key) <= 0)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

static inline int
sift_mf_int_search(const SiftPartDirKey *keys, int n, const uint8 *key)
{
	int			lo = 0,
				hi = n;

	while (hi - lo > 1)
	{
		int			mid = lo + (hi - lo) / 2;

		if (bound_cmp(keys[mid].key_lo, key) <= 0)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* The index of a part in an array of parts by its generation, for the edits that look up many: open addressing over the array, which must not move while the map is used */
typedef struct SiftGenMap
{
	const SiftPartDesc *parts;
	int32	   *slot;			/* the index plus one, 0 where empty */
	uint32		mask;
	int32		stack[128];
} SiftGenMap;

static inline uint32
sift_gen_slot(const SiftGenMap *m, uint64 gen)
{
	return (uint32) ((gen * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & m->mask;
}

static inline void
sift_gen_map_add(SiftGenMap *m, int idx)
{
	uint32		p = sift_gen_slot(m, m->parts[idx].part_gen);

	while (m->slot[p] != 0)
		p = (p + 1) & m->mask;
	m->slot[p] = idx + 1;
}

/* A map over parts[0..n) with room for extra more (their parts[] entries added later with sift_gen_map_add) */
static inline void
sift_gen_map_init(SiftGenMap *m, const SiftPartDesc *parts, int n, int extra)
{
	uint32		cap = 64;

	while (cap < 2 * ((uint32) n + (uint32) extra))
		cap *= 2;
	m->parts = parts;
	m->mask = cap - 1;
	/* check-memory: allow a slot for each part of one partition and of the edit, twice over */
	m->slot = cap <= lengthof(m->stack) ? m->stack : palloc(cap * sizeof(int32));
	memset(m->slot, 0, cap * sizeof(int32));
	for (int i = 0; i < n; i++)
		sift_gen_map_add(m, i);
}

static inline int
sift_gen_map_find(const SiftGenMap *m, uint64 gen)
{
	for (uint32 p = sift_gen_slot(m, gen); m->slot[p] != 0; p = (p + 1) & m->mask)
		if (m->parts[m->slot[p] - 1].part_gen == gen)
			return m->slot[p] - 1;
	return -1;
}

static inline void
sift_gen_map_free(SiftGenMap *m)
{
	if (m->slot != m->stack)
		pfree(m->slot);
}

/* The oldest of a and xid, where only a normal id counts: a frozen part (min_xid FrozenTransactionId) holds nothing back */
static inline FullTransactionId
sift_mf_older_active(FullTransactionId a, FullTransactionId xid)
{
	return FullTransactionIdIsNormal(xid) && (!FullTransactionIdIsValid(a) || FullTransactionIdPrecedes(xid, a)) ? xid : a;
}

extern void sift_mf_blk_add(BlkVec *b, BlockNumber blk);
extern int	sift_mf_drop_parts(Relation rel, SiftPartDesc *parts, int n, const SiftManifestEdit *e, uint64 **expire, int *nexpire);
extern void sift_mf_apply_updates(Relation rel, const SiftPartitionEntry *ent, SiftPartDesc *parts, const SiftGenMap *map, const SiftManifestEdit *e);
extern void sift_mf_collect_range(Relation rel, BlockNumber blk, int level, const uint8 *lo, const uint8 *hi, SiftManifestRead *out, int *cap);
extern void sift_mf_fold_entry(SiftPartitionEntry *e, const SiftPartDesc *parts, int n);
extern void sift_mf_refresh_hot_active(SiftPartitionEntry *e);
extern Page sift_mf_read_page(Relation rel, BlockNumber blk, SiftPageType type, uint32 magic, uint64 gen, uint32 part_id, int *pages);
extern Page sift_mf_read_dir(Relation rel, BlockNumber blk, uint16 level, int *pages);
extern OldNode sift_mf_read_node(Relation rel, BlockNumber blk, uint16 level, int *pages);
extern Tree *sift_mf_read_tree(Relation rel, const SiftColMetaPageData *meta);
extern Tree *sift_mf_read_tree_path(Relation rel, const SiftColMetaPageData *meta, const uint8 *key_lo);
extern int	sift_mf_find_leaf(const Tree *t, const uint8 *key_lo);
extern bool sift_mf_find_entry_at(Relation rel, const SiftColMetaPageData *meta, uint64 part_id, const uint8 *key_lo, SiftPartitionEntry *out, int *pages);
/* The change of a commit: its leaves read into ch, the entry of a partition in it, its effect on the totals, the record that makes it the manifest (false: another commit moved the version, read again) and the pages of a record that was not made */
extern void sift_mf_read_leaves(Relation rel, Tree *t, int first, int span, Change *ch, int slack);
extern int	sift_mf_find_entry_idx(const Change *ch, uint64 part_id, const uint8 *key_lo);
extern void sift_mf_account(Change *ch, const SiftPartitionEntry *before, const SiftPartitionEntry *after);
extern bool sift_mf_publish(Relation rel, Tree *t, Change *ch, BlkVec *created, uint64 *version);
extern void sift_mf_free_created(Relation rel, BlkVec *created);

/* combine.c: the commit of an edit of one partition that touches nothing else of the manifest joins the commits of the partitions of the relation that wait for Block 0 (false: it does not, the caller publishes it alone) */
extern bool sift_mf_commit_combined(Relation rel, Change *ch, uint64 part_id, const uint8 *key_lo, uint64 *version);

extern BlockNumber sift_mf_new_page_blk(Relation rel, BlkVec *created);
extern void sift_mf_write_image(Relation rel, BlockNumber blk, const char *image);
extern void sift_mf_log_image(SiftNewPageLog *log, BlockNumber blk, const char *image);

/* The scope directory (core/manifest/scopedir.c) */
extern void sift_mf_build_scopes(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, BlkVec *created);

/* The projection directory (core/f2/manifest/projdir.c): the commit of an edit applies the deltas of its update and drops the projection parts of the base parts it drops */
struct SiftManifestProjUpdate;
extern void sift_mf_build_commit_projs(Relation rel, Tree *t, Change *ch, const struct SiftManifestProjUpdate *u, const uint64 *drop_gens, int ndrop, BlkVec *created);
extern void sift_proj_check(Relation rel, const SiftColMetaPageData *meta, uint64 *base_gens, int nbase);

/* The tier directory (core/manifest/tierdir.c) */
extern void sift_mf_build_tier(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, const uint64 *expire_gens, int nexpire, BlkVec *created);
extern void sift_mf_apply_tier(SiftColMetaPageData *m, const Change *ch);
extern void sift_mf_check_added(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *p);

/* What the blocks a commit releases wait for (release.c) */
extern bool sift_manifest_unshared(Relation rel);
extern bool sift_manifest_waits_own_xid(Relation rel, int nrel);

#endif							/* SIFT_MANIFEST_INT_H */
