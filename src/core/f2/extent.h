/*
 * core/extent.h - the indirect extent list of a part (format major 2).
 *
 * A part keeps the extents of its byte stream in its header page while they fit; the rest goes to a chain of SIFT_PT_EXTLIST pages, SIFT_EXTLIST_CAP extents each, so that a
 * part has no limit on its extents. The reader loads the whole list when it opens the part.
 */
#ifndef SIFT_EXTENT_H
#define SIFT_EXTENT_H

#include "core/host.h"

#include "core/f2/meta.h"

#define SIFT_EXTLIST_MAGIC		0x45585432U		/* "EXT2" */

typedef struct SiftExtentListPageData
{
	uint32		magic;
	uint16		nentries;
	uint16		flags;
	uint32		crc32c;
	BlockNumber next_blk;
	uint64		part_gen;
	SiftExtent	ext[FLEXIBLE_ARRAY_MEMBER];
} SiftExtentListPageData;

StaticAssertDecl(offsetof(SiftExtentListPageData, ext) == 24, "SiftExtentListPageData header is 24 bytes");

#define SIFT_EXTLIST_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftExtentListPageData, ext)) / sizeof(SiftExtent))

/* Writes the n >= 1 extents to a chain of pages taken from the metadata slabs; the first block of it */
extern BlockNumber sift_extlist_write(Relation rel, uint64 part_gen, uint32 part_id, const SiftExtent *ext, int n);

/* Reads the n extents of the chain at head (ERROR when the chain holds another number); the blocks of its pages are appended to pages when it is not NULL */
extern SiftExtent *sift_extlist_read(Relation rel, BlockNumber head, uint64 part_gen, uint32 part_id, int n, BlockNumber **pages, int *npages);

#endif							/* SIFT_EXTENT_H */
