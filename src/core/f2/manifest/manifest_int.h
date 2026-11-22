/*
 * core/manifest/manifest_int.h - what the files of the manifest share: the vectors of blocks, the directory as read, a change of it and the page helpers (core/manifest.c).
 */
#ifndef SIFT_MANIFEST_INT_H
#define SIFT_MANIFEST_INT_H

#include "core/host.h"

#include "col/f2/col_meta.h"
#include "core/f2/manifest.h"

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
	bool		set_tier;		/* the tier directory is replaced: the head page, the refs in all and the generation of the new chain */
	BlockNumber tier_dir_blk;
	uint32		ntier_parts;
	uint64		tier_dir_gen;
	FullTransactionId new_min_xid;
} Change;

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

extern void sift_mf_blk_add(BlkVec *b, BlockNumber blk);
extern int	sift_mf_part_gen_index(const SiftPartDesc *parts, int n, uint64 gen);
extern void sift_mf_unclaim_patched_time(SiftPartDesc *parts, int n, const SiftColDeltaAnchor *a);
extern void sift_mf_apply_updates(Relation rel, const SiftPartitionEntry *ent, SiftPartDesc *parts, int n, const SiftManifestEdit *e);
extern void sift_mf_collect_range(Relation rel, BlockNumber blk, int level, const uint8 *lo, const uint8 *hi, SiftManifestRead *out, int *cap);
extern void sift_mf_fold_entry(SiftPartitionEntry *e, const SiftPartDesc *parts, int n);
extern void sift_mf_refresh_hot_active(SiftPartitionEntry *e);
extern Page sift_mf_read_page(Relation rel, BlockNumber blk, SiftPageType type, uint32 magic, uint64 gen, uint32 part_id, int *pages);
extern BlockNumber sift_mf_new_page_blk(Relation rel, BlkVec *created);
extern void sift_mf_write_image(Relation rel, BlockNumber blk, const char *image);

/* The scope directory (core/manifest/scopedir.c) */
extern void sift_mf_build_scopes(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, BlkVec *created);

/* The projection directory (core/f2/manifest/projdir.c): the commit of an edit applies the deltas of its update and drops the projection parts of the base parts it drops */
struct SiftManifestProjUpdate;
extern void sift_mf_build_commit_projs(Relation rel, Tree *t, Change *ch, const struct SiftManifestProjUpdate *u, const uint64 *drop_gens, int ndrop, BlkVec *created);
extern void sift_proj_check(Relation rel, const SiftColMetaPageData *meta, uint64 *base_gens, int nbase);

/* The tier directory (core/manifest/tierdir.c) */
extern void sift_mf_build_tier(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, const uint64 *expire_gens, int nexpire, BlkVec *created);
extern uint64 *sift_mf_remote_drops(const SiftPartDesc *parts, int n, const SiftManifestEdit *e, int *nexpire);
extern void sift_mf_apply_tier(SiftColMetaPageData *m, const Change *ch);
extern void sift_mf_check_added(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *p);

/* What the blocks a commit releases wait for (release.c) */
extern bool sift_manifest_unshared(Relation rel);
extern bool sift_manifest_waits_own_xid(Relation rel, int nrel);

#endif							/* SIFT_MANIFEST_INT_H */
