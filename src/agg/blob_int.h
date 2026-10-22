/*
 * agg/blob_int.h - the blob a group table leaves a process as (agg/blob.c)
 */
#ifndef SIFT_AGG_BLOB_INT_H
#define SIFT_AGG_BLOB_INT_H

#include "agg/agg_fwd_int.h"

/*
 * A group table leaves a process as a blob: the key values its groups use, then per group the key ids (numbered from 1 in the
 * blob) and the states of the aggregates, in 8-byte aligned sections. blob_merge adds a blob to a table by value: the keys through
 * the table's own ids, the count(DISTINCT) sets by union, the sums in the order the blobs are merged (which the caller fixes).
 */
#define BLOB_MAGIC	0x53414731

typedef struct BlobHdr
{
	uint32		magic;
	int32		lane;
	uint32		ngroups;
	uint32		pad;
} BlobHdr;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* blob.c */
void blob_dump(AggExec *ex, int lane, StringInfo b);
void blob_dump_list(AggExec *ex, int lane, const uint32 *gl, uint32 ng, StringInfo b);
uint32 blob_group_hash(AggExec *ex, uint32 g);
void blob_merge(AggExec *g, const char *p);

#pragma GCC visibility pop

#endif
