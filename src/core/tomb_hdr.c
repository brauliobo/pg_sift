/*
 * core/tomb_hdr.c - the header page of the tombstone log (core/tomb.h): the record of its completeness, the record of its key journal, and the format of its segment.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"

#include "core/tomb.h"

#define corrupt(rel, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_DATA_CORRUPTED), \
			 errmsg("relation \"%s\" has a corrupted tombstone log", RelationGetRelationName(rel)), \
			 errdetail(__VA_ARGS__)))

/* The completeness the record says is the same; VACUUM freezing the transaction that made the log complete says nothing new */
static bool
chan_same(const SiftTombChan *old, const SiftTombChan *new)
{
	return old->flags == new->flags && old->witness[0] == new->witness[0] && old->witness[1] == new->witness[1] &&
		(old->base_xid == new->base_xid || new->base_xid == FrozenTransactionId);
}

/*
 * The journal vouches for the horizons from the transaction that changed the completeness of the log on: what the log did not hear before is not in the journal either, so a
 * stamp older than that is not vouched (the next transaction id is above the transaction that makes the change).
 */
void
sift_tomb_set_chan(Relation rel, SiftTombLog *t, const SiftTombChan *c)
{
	const SiftSegEntry *e;
	Buffer		hbuf;
	SiftPageEdit ed;
	bool		trim;

	LockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	sift_dir_read(rel, t->dir);
	if ((e = sift_tomb_find_seg(t->dir)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	hbuf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_EXCLUSIVE);
	trim = SiftPageGetUsed(BufferGetPage(hbuf)) >= MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTombHdr) && SIFT_TOMB_HDR(BufferGetPage(hbuf))->keys.lineage != 0 &&
		!chan_same(&SIFT_TOMB_HDR(BufferGetPage(hbuf))->chan, c);
	sift_page_edit_begin(&ed, hbuf, SIFT_PAGE_DATA_OFFSET);
	sift_page_edit_extra(&ed, (char *) &SIFT_TOMB_HDR(ed.page)->chan - (char *) ed.page,
						 trim ? sizeof(SiftTombChan) + sizeof(SiftTombKeys) : sizeof(SiftTombChan));
	SIFT_TOMB_HDR(ed.page)->chan = *c;
	if (trim)
		SIFT_TOMB_HDR(ed.page)->keys.trim = ReadNextTransactionId();
	sift_page_edit_end(rel, &ed, 1);
	if (trim)
		t->keys.trim = SIFT_TOMB_HDR(BufferGetPage(hbuf))->keys.trim;
	UnlockReleaseBuffer(hbuf);
	UnlockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	t->chan = *c;
}

void
sift_tomb_set_keys(Relation rel, SiftTombLog *t, const SiftTombKeys *k)
{
	const SiftSegEntry *e;
	Buffer		hbuf;
	SiftPageEdit ed;

	LockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	sift_dir_read(rel, t->dir);
	if ((e = sift_tomb_find_seg(t->dir)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	hbuf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_EXCLUSIVE);
	if (SiftPageGetUsed(BufferGetPage(hbuf)) < MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTombHdr))
		corrupt(rel, "The tombstone header does not hold a key journal record.");
	sift_page_edit_begin(&ed, hbuf, SIFT_PAGE_DATA_OFFSET);
	sift_page_edit_extra(&ed, (char *) &SIFT_TOMB_HDR(ed.page)->keys - (char *) ed.page, sizeof(SiftTombKeys));
	SIFT_TOMB_HDR(ed.page)->keys = *k;
	sift_page_edit_end(rel, &ed, 1);
	UnlockReleaseBuffer(hbuf);
	UnlockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	t->keys = *k;
	t->keys_cover = true;
}

static void
set_tomb_format(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (sift_tomb_is_seg(&dir->segs[i]))
			dir->segs[i].format = Max(dir->segs[i].format, *(uint8 *) arg);
}

void
sift_tomb_set_format(Relation rel, uint8 format)
{
	sift_dir_update(rel, &(SiftDirChange) {.fn = set_tomb_format,.arg = &format,.extra = InvalidBuffer});
}

bool
sift_tomb_peek_keys(Relation rel, const SiftDirSnapshot *dir, SiftTombKeys *k, uint64 *nentries)
{
	const SiftSegEntry *e = sift_tomb_find_seg(dir);
	Buffer		buf;
	Page		page;
	bool		cover;

	memset(k, 0, sizeof(*k));
	*nentries = 0;
	if (e == NULL)
		return false;
	buf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_WBUFHEADER, e->gen);
	page = BufferGetPage(buf);
	cover = SiftPageGetUsed(page) >= MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTombHdr);
	*nentries = SIFT_TOMB_HDR(page)->nentries;
	if (cover)
		*k = SIFT_TOMB_HDR(page)->keys;
	UnlockReleaseBuffer(buf);
	return cover;
}
