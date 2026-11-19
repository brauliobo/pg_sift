/*
 * core/dirchain.h - the chained directory of core/page.h: what store.c, verify.c and the tests share.
 */
#ifndef SIFT_DIRCHAIN_H
#define SIFT_DIRCHAIN_H

#include "core/host.h"

#include "core/page.h"
#include "core/range.h"

/* An interior page as its root lists it, and the first of its leaves in the leaves of the chain */
typedef struct SiftDirInner
{
	SiftChainRef ref;
	uint32		leaf0;
} SiftDirInner;

/* A chain as read: its root, its interior pages, its leaves in directory order and every block it occupies (the root first) */
typedef struct SiftDirChain
{
	BlockNumber root;
	uint32		nall;			/* the entries of the whole directory, which a read of a range leaves some of */
	uint32		ninner;
	SiftDirInner *inner;
	uint32		nleaves;
	SiftChainRef *leaf;
	BlockNumber *blocks;
	int			nblocks;
	int			maxblocks;
} SiftDirChain;

/*
 * A partition: entries [first, first + n) of the directory, one leaf of the chain, with the lineage that names it across the steps that rewrite it (page.h) and the
 * lower bound of its key range. The entries of no partition come first (lineage 0); the partitions follow, by bound.
 */
typedef struct SiftDirPart
{
	uint64		lineage;
	uint32		first;
	uint32		n;
	uint8		lo[SIFT_BOUND_BYTES];
} SiftDirPart;

/* The partitions of a chain as read, in directory order (palloc'd; the number) */
extern int	sift_dirchain_parts(const SiftDirChain *chain, SiftDirPart **parts);
extern int	sift_dirchain_read(Relation index, BlockNumber root, uint64 version, int room, SiftSegEntry **segs, SiftDirChain *chain);
/* The entries of no partition and of the partitions that may hold a key of the range only , and the pages read (the chain lists the leaves read) */
extern int	sift_dirchain_read_range(Relation index, BlockNumber root, uint64 version, const SiftKeyRange *keys, SiftSegEntry **segs, SiftDirChain *chain);
extern BlockNumber sift_dirchain_write(Relation index, Buffer metabuf, uint64 version, const SiftDirChain *old, const SiftSegEntry *oldsegs,
									   const SiftSegEntry *segs, int n, const SiftDirPart *parts, int nparts, BlockNumber below, SiftExtent *release, int *nrelease);

/* The entries a directory keeps in block 1: more go to a chain */
extern int	sift_dir_flat_max(void);
/* The entries a leaf holds (a partition holds at most this many) */
extern uint32 sift_dirchain_leaf_cap(void);

#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_dir_flat_max;
extern PGDLLIMPORT int sift_test_dir_leaf_max;
extern PGDLLIMPORT int sift_test_dir_root_max;
#endif

#endif							/* SIFT_DIRCHAIN_H */
