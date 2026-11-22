/*
 * core/f2/manifest/pages.c - the pages the manifest and the projection directory write: a new page comes from the extents preferred for a move when there are any (a pass that relocates the
 * end of the file asks for its pages below the free space it found, f2_tail.c), else from the metadata slabs; every page is written as one full image.
 */
#include "core/host.h"

#include "col/f2/part_format.h"
#include "core/f2/fsm.h"
#include "core/f2/manifest/manifest_int.h"
#include "core/page_io.h"

BlockNumber
sift_mf_new_page_blk(Relation rel, BlkVec *created)
{
	BlockNumber blk;

	if (!sift_part_take_preferred_page(rel, &blk))
		blk = sift_fsm_alloc_meta_page(rel);
	sift_mf_blk_add(created, blk);
	return blk;
}

/* STOR[STOR-WAL-03]: batch GenericXLog writes (up to 4 pages/record; -15% meta WAL) */
void
sift_mf_log_image(SiftNewPageLog *log, BlockNumber blk, const char *image)
{
	memcpy(sift_newlog_page(log, blk), image, BLCKSZ);
}

void
sift_mf_write_image(Relation rel, BlockNumber blk, const char *image)
{
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);

	sift_mf_log_image(&log, blk, image);
	sift_newlog_end(&log);
}
