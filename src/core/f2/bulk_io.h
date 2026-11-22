/*
 * core/bulk_io.h - the pages of a part written in batches: up to MAX_GENERIC_XLOG_PAGES full images in one record of generic WAL, and the file extended once for the whole run
 * of blocks instead of one block at a time.
 *
 * Every page of a batch is built whole (a full image), so none depends on what its block held before: the blocks come from the allocator, which hands out free or new extents
 * only (core/fsm.h, P3), and may be cached in shared buffers with the bytes of an older part; the images replace them through the buffer, which a write that went around shared buffers would not.
 */
#ifndef SIFT_BULK_IO_H
#define SIFT_BULK_IO_H

#include "core/host.h"

#include "core/xlog.h"

typedef struct SiftPartBulkWriter
{
	Relation	rel;
	SiftXlog	x;
	bool		open;
} SiftPartBulkWriter;

/* Makes sure the file holds every block of the extents (one extension for the run that is past its end) */
extern void sift_bulk_extend(Relation rel, const SiftExtent *ext, int next);

extern void sift_bulk_begin(SiftPartBulkWriter *w, Relation rel);

/* The working copy of block blk to build as a full image; the batch is logged when it holds MAX_GENERIC_XLOG_PAGES pages */
extern Page sift_bulk_page(SiftPartBulkWriter *w, BlockNumber blk);

/* Logs what the batch holds */
extern void sift_bulk_end(SiftPartBulkWriter *w);

#endif							/* SIFT_BULK_IO_H */
