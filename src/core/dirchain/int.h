/*
 * core/dirchain/int.h - the per-backend memo of whole directory chains (core/dirchain/memo.c) as core/dirchain/dirchain.c uses it.
 */
#ifndef SIFT_CORE_DIRCHAIN_INT_H
#define SIFT_CORE_DIRCHAIN_INT_H

#include "core/host.h"

#include "lib/ilist.h"

#include "core/dirchain/dirchain.h"

typedef struct DirMemo
{
	RelFileLocator rel;			/* hash key */
	BlockNumber root;
	uint64		version;
	uint64		used;
	Size		bytes;
	SiftDirChain chain;			/* leaf, inner and blocks point into data */
	int			nsegs;
	SiftSegEntry *segs;
	char	   *data;
	dlist_node	lru;			/* in memo_lru, least recently used first */
} DirMemo;

extern DirMemo *sift_dirchain_memo_keep(Relation index, uint64 version, const SiftSegEntry *segs, int nsegs, const SiftDirChain *chain);
extern DirMemo *sift_dirchain_memo_find(Relation index, BlockNumber root, uint64 version);

#endif
