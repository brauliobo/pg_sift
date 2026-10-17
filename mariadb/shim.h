/*
 * shim.h - what shim.c, store.c and engine.c share: the error state of the backend shim and the "relation" of an
 * index, a directory of immutable segment files that store.c maps into a reserved address range so that the
 * block numbers of the page layout (src/sift_page.h) address them.
 */
#ifndef SIFT_MDB_SHIM_H
#define SIFT_MDB_SHIM_H

#include <setjmp.h>

#include "sift.h"
#include "sift_page.h"
#include "sift_store.h"

/* ereport(ERROR) longjmps to *sift_err_jmp and leaves the SQLSTATE and the message here */
extern sigjmp_buf *sift_err_jmp;
extern int	sift_err_code;
extern char sift_err_msg[1024];

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
#define SIFT_NGSLOTS		1024	/* slots of the reserved range, shared by every index of the process */

typedef struct SiftRel
{
	char		name[64];
	char		dir[512];
	SiftOptions opts;
	SiftDirSnapshot snap;		/* what sift_dir_read returns */
	uint32		dpp;			/* documents per page: docid d is key (d / dpp) << 16 | (d % dpp + 1) */
	bool		slot_used[SIFT_NSLOTS];
	int			gslot[SIFT_NSLOTS];	/* where each mapped slot sits in the reserved range */
	uint32		slot_blocks[SIFT_NSLOTS];
	/* visibility: documents whose row was updated or deleted (sift_vis_filter_page) */
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

/* The buffer of a block of the index (a pointer into the reserved range) and the block of a buffer */
extern Buffer sift_buf_of(Relation rel, BlockNumber blk);
extern BlockNumber sift_buf_block(Buffer buf);

/* Maps a finished segment file at slot; unmaps it */
extern void sift_slot_map(SiftRel *sr, int slot, const char *path);
extern void sift_slot_unmap(SiftRel *sr, int slot);

/* The segment file being written: its slot, path and size in blocks once finished */
extern int	sift_wt_slot(const SiftWriteTarget *wt);
extern const char *sift_wt_path(const SiftWriteTarget *wt);
extern void sift_wt_abort(SiftWriteTarget *wt);

extern void sift_shim_init(void);

#endif							/* SIFT_MDB_SHIM_H */
