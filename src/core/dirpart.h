/*
 * core/dirpart.h - the partitions of a directory update (core/dirpart.c): which partition each entry is in while a mutator edits the directory
 */
#ifndef SIFT_DIRPART_H
#define SIFT_DIRPART_H

#include "core/dirchain.h"
#include "core/store.h"

/* The partition an entry is in, by generation: the lineage and the bound of the partition */
typedef struct SiftPartOf
{
	uint64		gen;
	uint64		lineage;
	uint8		lo[SIFT_BOUND_BYTES];
} SiftPartOf;

/* The entries of the partitions the directory had, and those the update put in one (sift_dir_part_replace), by generation */
typedef struct SiftDirParts
{
	SiftPartOf *of;
	int			n;
	int			max;
} SiftDirParts;

/* Reads the partitions of the chain whose entries are old; the mutators of the update that follows (sift_dir_part_replace) work on them until sift_dirpart_leave */
extern void sift_dirpart_open(SiftDirParts *e, const SiftDirChain *chain, const SiftSegEntry *old);
extern void sift_dirpart_enter(SiftDirParts *e);
extern void sift_dirpart_leave(SiftDirParts *e);
/* The entries of no partition first, then each partition's, by bound; the partitions as the chain keeps them. Returns their number */
extern int	sift_dirpart_arrange(Relation index, const SiftDirParts *e, SiftSegEntry *segs, int n, SiftDirPart **parts);

/* The partitions the directory of index has as snap read it, in directory order (palloc'd), and the entries they are of: none, and the entries of snap, when it is not chained */
extern int	sift_dir_parts_read(Relation index, const SiftDirSnapshot *snap, SiftSegEntry **segs, SiftDirPart **parts);

#endif							/* SIFT_DIRPART_H */
