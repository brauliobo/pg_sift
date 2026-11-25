/*
 * core/live/repair_test.c - sift._test_live_repair: a LIVE run damaged the way sift_live_clear of 4.2.1 to 4.3.5 damaged it, and what the audit and the repair make of it
 */
#include "core/store/test_int.h"
#include "utils/array.h"

#include "core/live/live.h"

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_live_repair);

/*
 * The dirty bit of an SB, where sift_live_clear of 4.2.1 to 4.3.5 wrote it: the byte of the first page's image whatever the SB, so for an SB past its capacity in the special space,
 * and from SB 65,280 on in the image of the page of the dead row, which the record registered second: in its header up to SB 65,471, in its bytes after that, which is not reproduced
 */
static void
legacy_dirty_bit(const SiftLive *l, uint32 sbidx)
{
	uint64		b = sizeof(SiftLiveHeaderData) + sbidx / 8,
				image = SIFT_PAGE_DATA_OFFSET + b,
				row = SIFT_LIVE_START(l->nsb) + sbidx / 8;
	Buffer		first = ReadBuffer(l->index, l->first),
				right = ReadBuffer(l->index, l->first + SIFT_PAYLOAD_PAGE(b)),
				hit;
	uint8	   *at;

	if (image >= BLCKSZ + SIFT_PAGE_DATA_OFFSET)
	{
		ReleaseBuffer(first);
		ReleaseBuffer(right);
		return;
	}
	hit = image < BLCKSZ ? first : ReadBuffer(l->index, l->first + SIFT_PAYLOAD_PAGE(row));
	LockBuffer(right, BUFFER_LOCK_EXCLUSIVE);
	*((uint8 *) SiftPageGetData(BufferGetPage(right)) + SIFT_PAYLOAD_OFF(b)) &= ~(1 << (sbidx % 8));
	MarkBufferDirty(right);
	UnlockReleaseBuffer(right);
	LockBuffer(hit, BUFFER_LOCK_EXCLUSIVE);
	at = (uint8 *) BufferGetPage(hit) + (image < BLCKSZ ? image : image - BLCKSZ);
	*at |= 1 << (sbidx % 8);
	MarkBufferDirty(hit);
	UnlockReleaseBuffer(hit);
	if (hit != first)
		ReleaseBuffer(first);
}

static void
put_stage(ReturnSetInfo *rsi, const char *stage, const SiftLiveAudit *a, int dirty_read)
{
	Datum		v[7] = {CStringGetTextDatum(stage), Int32GetDatum(a->pages_damaged), Int32GetDatum(a->units_lost), Int64GetDatum(a->ndead_header),
						Int64GetDatum(a->cleared), Int32GetDatum(a->pages_written), Int32GetDatum(dirty_read)};
	bool		nulls[7] = {0};

	nulls[6] = dirty_read < 0;
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
}

/*
 * sift._test_live_repair(index, nsb, sbs): the LIVE run of nsb superblocks of one row each (none in the directory), the rows of the superblocks sbs killed with the dirty
 * bit where the old writer put it (a superblock listed twice is counted dead twice, as a VACUUM did when its dirty bit was lost); the audit, the repair, the audit and the repair again,
 * and how many of the listed superblocks read back dirty through the checked reader
 */
Datum
sift_test_live_repair(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint32		nsb = PG_GETARG_INT32(1);
	ArrayType  *sbs = PG_GETARG_ARRAYTYPE_P(2);
	int32	   *sb = (int32 *) ARR_DATA_PTR(sbs);
	int			nsbs = ArrayGetNItems(ARR_NDIM(sbs), ARR_DIMS(sbs));
	SiftLive	l = {.index = index, .nsb = nsb, .nbits = nsb};
	SiftLiveUnit *units = palloc(nsb * sizeof(SiftLiveUnit));
	SiftLiveAudit a;
	SiftWriteTarget *wt;
	uint8	   *dirty = palloc0((nsb + 7) / 8);
	int			nread = 0;

	InitMaterializedSRF(fcinfo, 0);
	MAINT_LOCK(index, ExclusiveLock);
	l.gen = sift_store_new_gen(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	sift_live_write(wt, l.gen, nsb, l.nbits, &l.first, &l.npages);
	sift_wt_finish(wt);
	for (uint32 i = 0; i < nsb; i++)
		units[i] = (SiftLiveUnit) {i, 1, i};
	for (int i = 0; i < nsbs; i++)
	{
		uint64		rank = sb[i];

		sift_live_clear(&l, sb[i], &rank, 1);
	}
	for (int i = 0; i < nsbs; i++)
		legacy_dirty_bit(&l, sb[i]);
	sift_live_audit(&l, units, nsb, &a);
	put_stage(rsi, "damaged", &a, -1);
	sift_live_repair(&l, units, nsb, &a);
	put_stage(rsi, "repaired", &a, -1);
	sift_live_audit(&l, units, nsb, &a);
	put_stage(rsi, "audit", &a, -1);
	sift_live_repair(&l, units, nsb, &a);
	sift_live_dirty(&l, dirty);
	for (int i = 0; i < nsbs; i++)
		nread += dirty[sb[i] / 8] >> (sb[i] % 8) & 1;
	put_stage(rsi, "again", &a, nread);
	MAINT_UNLOCK(index, ExclusiveLock);
	relation_close(index, RowExclusiveLock);
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
