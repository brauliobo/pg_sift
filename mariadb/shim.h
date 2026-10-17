/*
 * shim.h - what shim.c, store.c and the engine share: the error state of the backend shim and the "relation" of an
 * index, a directory of immutable segment files that store.c maps into a reserved address range so that the
 * block numbers of the page layout (src/sift_page.h) address them.
 *
 * Every piece of state the engine writes while it runs (memory contexts, the error state) is per thread, so queries of
 * different connections run in parallel; the rest (the mapped range, the registry of mapped slots) is guarded by locks.
 */
#ifndef SIFT_MDB_SHIM_H
#define SIFT_MDB_SHIM_H

#include <setjmp.h>

#include "sift.h"
#include "sift_filter.h"
#include "sift_page.h"
#include "sift_store.h"

/* ereport(ERROR) longjmps to *sift_err_jmp and leaves the SQLSTATE and the message here */
extern __thread sigjmp_buf *sift_err_jmp;
extern __thread int sift_err_code;
extern __thread char sift_err_msg[1024];

/* directory of the temporary files (runs, dictionary spool) */
extern const char *sift_tmpdir;

/* Run body; returns 0, or -1 with sift_err_msg set when the engine raised an error */
#define SIFT_TRY(body) \
	({ sigjmp_buf jb_; sigjmp_buf *saved_ = sift_err_jmp; int rc_ = 0; \
	   if (sigsetjmp(jb_, 0) == 0) { sift_err_jmp = &jb_; body; } else rc_ = -1; \
	   sift_err_jmp = saved_; rc_; })

/* A segment file is mapped at (slot << SLOT_SHIFT) blocks of the reserved range: 4 GiB per segment */
#define SIFT_SLOT_SHIFT		19
#define SIFT_NSLOTS			256		/* segment slots of one index (the block numbers in its manifest) */
#define SIFT_DELTA_SLOT		(SIFT_NSLOTS - 1)	/* the slot of the live segment built from the queue */
#define SIFT_NGSLOTS		1024	/* slots of the reserved range, shared by every index of the process */

typedef struct SiftRel
{
	char		name[64];
	char		dir[512];
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
} SiftRel;

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

/* Creates the relation handle the engine functions take (its rd_amcache is the SiftRel) */
extern Relation sift_rel_create(const char *name, const char *dir, uint32 dpp, SiftRel **out);
extern void sift_rel_destroy(Relation rel);

/* A private handle on the same mapped segments: the copy a query edits (visibility, delta slot) without the others seeing it */
extern Relation sift_rel_view(const SiftRel *base, SiftRel **out);
extern void sift_rel_view_free(Relation rel);

/* The buffer of a block of the index (a pointer into the reserved range) and the block of a buffer */
extern Buffer sift_buf_of(Relation rel, BlockNumber blk);
extern BlockNumber sift_buf_block(Buffer buf);

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

extern void sift_shim_init(void);

#endif							/* SIFT_MDB_SHIM_H */
