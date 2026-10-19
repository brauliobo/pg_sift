/*
 * store.h - the "relation" of an index: a directory of immutable segment files that store.c maps into a reserved address
 * range, so that the block numbers of the page layout (src/core/sift_page.h) address them.
 *
 * A segment is mapped MAP_PRIVATE at a slot of 4 GiB of the range; the block number of a page is slot << SIFT_SLOT_SHIFT | the
 * block of the file, so the Buffer of a block (host_store.h) is the pointer to its page and nothing is pinned or locked. A merge
 * clears liveness bits in its private copy of the pages only. The rest of the mapped range and the registry of mapped slots are
 * guarded by locks, every other piece of state the engine writes while it runs is per thread (sqlite/host/host.c).
 */
#ifndef SIFT_MDB_STORE_H
#define SIFT_MDB_STORE_H

#include "sift.h"
#include "sift_filter.h"
#include "sift_page.h"
#include "sift_store.h"

#define SIFT_SLOT_SHIFT		19
#define SIFT_NSLOTS			256		/* segment slots of one index (the block numbers in its manifest) */
#define SIFT_DELTA_SLOT		(SIFT_NSLOTS - 1)	/* the slot of the live segment built from the queue */
#define SIFT_NGSLOTS		1024	/* slots of the reserved range, shared by every index of the process */

/* What SqStore is for this host (host_store.h declares it): the state of one index, whose RelationData is `rel` */
struct SqStore
{
	RelationData rel;
	char		dir[512];		/* the index directory: new segments are written there */
	SiftOptions opts;
	char		optstr[256];	/* the strings of opts (stemmer, fields), which sit right after it */
	SiftDirSnapshot snap;		/* what sift_dir_read returns */
	uint32		dpp;			/* documents per page: docid d is key (d / dpp) << 16 | (d % dpp + 1) */
	bool		slot_used[SIFT_NSLOTS];
	int			gslot[SIFT_NSLOTS];	/* where each mapped slot sits in the reserved range */
	uint32		slot_blocks[SIFT_NSLOTS];
	/* visibility: documents that do not count (superseded, deleted, outside the filter), see sift_vis_filter_page */
	bool		(*is_dead) (void *ctx, int64 docid);
	void	   *dead_ctx;
	bool		any_dead;
};

typedef struct SqStore SiftRel;

static inline SiftKey
sift_docid_key(const SiftRel *sr, int64 docid)
{
	return ((uint64) (docid / sr->dpp) << 16) | (uint32) (docid % sr->dpp + 1);
}

static inline int64
sift_key_docid(const SiftRel *sr, SiftKey key)
{
	return (int64) sift_key_block(key) * sr->dpp + (int) (key & 0xFFFF) - 1;
}

/* Reserves the address range (once) */
extern void sift_store_init(void);

/* Creates the relation handle the engine functions take; *out is its state */
extern Relation sift_rel_create(const char *name, const char *dir, uint32 dpp, SiftRel **out);
extern void sift_rel_destroy(Relation rel);

/* A private handle on the same mapped segments: the copy a query edits (visibility, delta slot) without the others seeing it */
extern Relation sift_rel_view(const SiftRel *base, SiftRel **out);
extern void sift_rel_view_free(Relation rel);

/*
 * Mapping a segment file takes a slot of the reserved range (returned) without touching any index; a slot is then
 * attached to the index at a local slot number (under the index's lock) and detached before it is unmapped.
 */
extern int	sift_map_path(const char *path, int local, uint32 *blocks);
extern int	sift_map_fd(int fd, int local, uint32 *blocks);
extern void sift_unmap(int g, uint32 blocks);
extern void sift_slot_attach(SiftRel *sr, int slot, int g, uint32 blocks);
extern void sift_slot_detach(SiftRel *sr, int slot);

/* Maps the file at the local slot of sr (opening an index) / unmaps it */
extern void sift_slot_map(SiftRel *sr, int slot, const char *path);
extern void sift_slot_unmap(SiftRel *sr, int slot);

/*
 * A segment being written at a local slot: to the file new-<slot>.tmp of the index directory, or (mem) to an anonymous
 * memory file that outlives the target (fd) for the live segment.
 */
extern SiftWriteTarget *sift_wt_begin_at(const SiftRel *sr, int slot, bool mem);
extern int	sift_wt_slot(const SiftWriteTarget *wt);
extern const char *sift_wt_path(const SiftWriteTarget *wt);
extern int	sift_wt_fd(const SiftWriteTarget *wt);
extern void sift_wt_abort(SiftWriteTarget *wt);

/* A filter of the documents (ascending docids) a ranked scan may return, in the masks the ranker takes */
extern SiftFilter *sift_filter_from_docids(const SiftRel *sr, const int64 *docids, size_t n, MemoryContext mcxt);

#endif							/* SIFT_MDB_STORE_H */
