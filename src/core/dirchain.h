/*
 * core/dirchain.h - the chained directory of core/page.h: what store.c, verify.c and the tests share.
 */
#ifndef SIFT_DIRCHAIN_H
#define SIFT_DIRCHAIN_H

#include "core/host.h"

#include "core/page.h"

/* A chain as read: its root, its leaves in directory order and every block it occupies (the root first) */
typedef struct SiftDirChain
{
	BlockNumber root;
	uint32		nleaves;
	SiftChainRef *leaf;
	BlockNumber *blocks;
	int			nblocks;
	int			maxblocks;
} SiftDirChain;

extern int	sift_dirchain_read(Relation index, BlockNumber root, uint64 version, SiftSegEntry *dst, int cap, SiftDirChain *chain);
extern BlockNumber sift_dirchain_write(Relation index, Buffer metabuf, uint64 version, const SiftDirChain *old, const SiftSegEntry *oldsegs,
									   const SiftSegEntry *segs, int n, BlockNumber below, SiftExtent *release, int *nrelease);

/* The entries a directory keeps in block 1: more go to a chain */
extern int	sift_dir_flat_max(void);

#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_dir_flat_max;
extern PGDLLIMPORT int sift_test_dir_leaf_max;
extern PGDLLIMPORT int sift_test_dir_root_max;
#endif

#endif							/* SIFT_DIRCHAIN_H */
