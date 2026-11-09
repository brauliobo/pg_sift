/*
 * table/tomb.c - the write buffer page tables of a sift_table: the page of a virtual block
 */
#include "postgres.h"

#include "storage/bufmgr.h"

#include "core/lane.h"
#include "table/table.h"

/* Brings the page table of seg up to the header's now */
void
sift_tbl_seg_load(Relation rel, SiftTblSeg *seg)
{
	seg->npages = 0;
	(void) sift_tbl_wbuf_page(rel, seg, 0);
}

/*
 * Copies the write buffer page of vblock into copy (BLCKSZ bytes); false when none of the nsegs write buffers of segs holds it (*held: one covers it,
 * and the page is not there). *gen: its segment's generation. A scan reads the list it began with: the write buffer a VACUUM promotes meanwhile
 * is a column group in the directory the scan does not have
 */
bool
sift_tbl_load_vblock_in(Relation rel, SiftTblSeg *segs, int nsegs, BlockNumber vblock, Page copy, uint64 *gen, bool *held)
{
	*held = false;
	for (int i = 0; i < nsegs; i++)
	{
		SiftTblSeg *seg = &segs[i];
		BlockNumber blk;
		Buffer		buf;

		if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
			continue;
		*held = true;
		blk = sift_tbl_wbuf_page(rel, seg, vblock - seg->vbase);
		if (!BlockNumberIsValid(blk))
			return false;
		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
		sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
		memcpy(copy, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
		*gen = seg->gen;
		return true;
	}
	return false;
}

Buffer
sift_tbl_lock_page(Relation rel, const SiftTblSeg *seg, uint32 idx)
{
	Buffer		buf = sift_lane_read_locked(rel, seg->page[idx], BUFFER_LOCK_EXCLUSIVE);

	sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
	return buf;
}
