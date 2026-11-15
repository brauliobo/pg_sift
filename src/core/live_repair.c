/*
 * core/live_repair.c - the audit and the repair of the LIVE run of a segment (core/live.h): what sift_live_clear of 4.2.1 to 4.3.5 left in a segment of more than 65,152
 * superblocks, whose dirty bit it wrote past the first page of the bitmap: the type, flags and generation of page 0 changed, the header of the next page changed, and the dirty bits that fell in them lost.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "core/live.h"
#include "core/store_int.h"
#include "core/xlog.h"

#define SPECIAL_SIZE	MAXALIGN(sizeof(SiftPageOpaqueData))
#define LIVE_BYTES(l)	(SIFT_LIVE_START((l)->nsb) + ((l)->nbits + 7) / 8)

/* The bytes page i of the run holds: all of them but on the last page */
static uint32
page_bytes(const SiftLive *l, uint32 i)
{
	uint64		lo = (uint64) i * SIFT_PAGE_PAYLOAD,
				total = LIVE_BYTES(l);

	return lo >= total ? 0 : Min(SIFT_PAGE_PAYLOAD, total - lo);
}

/* The special space where it lies on every page, whatever the header's pointer to it says */
static SiftPageOpaque
special_of(Page page)
{
	return (SiftPageOpaque) ((char *) page + BLCKSZ - SPECIAL_SIZE);
}

static bool
special_ok(Page page, uint64 gen)
{
	SiftPageOpaque o = special_of(page);

	return o->type == SIFT_PT_LIVE && o->flags == 0 && o->reserved == 0 && o->gen == gen;
}

static bool
header_ok(const SiftLive *l, uint32 i, Page page)
{
	return sift_page_layout_ok(page) && SiftPageGetUsed(page) == page_bytes(l, i);
}

/* A page that is no longer one of the run: neither its header nor its special space says what it is, and its bytes are not to be believed */
static void
page_lost(const SiftLive *l, BlockNumber blk)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift index \"%s\" segment %llu is corrupted", RelationGetRelationName(l->index), (unsigned long long) l->gen),
			 errdetail("Liveness block %u has neither the header nor the special space of a liveness page.", blk),
			 errhint("The page cannot be restored in place: REINDEX the index.")));
}

/* The bytes of the run, as its pages hold them whatever their special spaces say: the page that is not as a LIVE page of the segment is counted */
static uint8 *
live_load(const SiftLive *l, SiftLiveAudit *a)
{
	/* check-memory: allow a bit for each row and each superblock of one segment */
	uint8	   *run = MemoryContextAllocHuge(CurrentMemoryContext, Max(LIVE_BYTES(l), 1));

	*a = (SiftLiveAudit) {.first_damaged = InvalidBlockNumber};
	for (uint32 i = 0; i < l->npages; i++)
	{
		BlockNumber blk = l->first + i;
		Buffer		buf = ReadBuffer(l->index, blk);
		Page		page;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!header_ok(l, i, page) && !special_ok(page, l->gen))
			page_lost(l, blk);
		if (!header_ok(l, i, page) || !special_ok(page, l->gen))
		{
			if (a->pages_damaged++ == 0)
				a->first_damaged = blk;
		}
		memcpy(run + (uint64) i * SIFT_PAGE_PAYLOAD, SiftPageGetData(page), page_bytes(l, i));
		UnlockReleaseBuffer(buf);
	}
	return run;
}

static uint64
cleared_bits(const uint8 *bits, uint64 nbits)
{
	uint64		whole = nbits / 8,
				live = pg_popcount((const char *) bits, whole);

	if (nbits % 8)
		live += pg_popcount32(bits[whole] & ((1 << (nbits % 8)) - 1));
	return nbits - live;
}

static bool
any_cleared(const uint8 *bits, uint64 first, uint64 end)
{
	for (uint64 r = first; r < end; r++)
	{
		if (r % 8 == 0 && r + 8 <= end && bits[r / 8] == 0xFF)
			r += 7;
		else if (!(bits[r / 8] >> (r % 8) & 1))
			return true;
	}
	return false;
}

/* What the run says it is, a dirty bit set for each unit that has a cleared bit and the header's count the cleared bits: the image a repair writes */
static void
live_derive(const SiftLive *l, uint8 *run, const SiftLiveUnit *units, uint32 nunits, SiftLiveAudit *a)
{
	SiftLiveHeaderData *lh = (SiftLiveHeaderData *) run;
	uint8	   *dirty = run + sizeof(SiftLiveHeaderData),
			   *bits = run + SIFT_LIVE_START(l->nsb);

	a->ndead_header = lh->ndead;
	a->cleared = cleared_bits(bits, l->nbits);
	for (uint32 i = 0; i < nunits; i++)
	{
		const SiftLiveUnit *u = &units[i];

		if (u->sbidx >= l->nsb || u->rank_base + u->nrows > l->nbits)
			SIFT_SEG_CORRUPT(l->index, l->gen, "Unit %u of the segment (rows %llu to %llu of %llu, superblock %u of %u) lies outside its liveness run.", i,
							 (unsigned long long) u->rank_base, (unsigned long long) (u->rank_base + u->nrows), (unsigned long long) l->nbits, u->sbidx, l->nsb);
		if (!(dirty[u->sbidx / 8] >> (u->sbidx % 8) & 1) && any_cleared(bits, u->rank_base, u->rank_base + u->nrows))
		{
			dirty[u->sbidx / 8] |= 1 << (u->sbidx % 8);
			a->units_lost++;
		}
	}
	lh->ndead = a->cleared;
}

void
sift_live_audit(const SiftLive *l, const SiftLiveUnit *units, uint32 nunits, SiftLiveAudit *a)
{
	uint8	   *run = live_load(l, a);

	live_derive(l, run, units, nunits, a);
	pfree(run);
}

/* A page that differs is written whole, laid out as sift_live_write lays it out: nothing of a header or a special space that is damaged is kept, and a replica replays the bytes and not a change of its own copy */
static bool
write_page(const SiftLive *l, uint32 i, const uint8 *run)
{
	Buffer		buf = ReadBuffer(l->index, l->first + i);
	Page		page;
	SiftXlog	x;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	if (header_ok(l, i, page) && special_ok(page, l->gen) &&
		memcmp(SiftPageGetData(page), run + (uint64) i * SIFT_PAGE_PAYLOAD, page_bytes(l, i)) == 0)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	x = sift_xlog_start(l->index);
	page = sift_xlog_register(&x, buf, GENERIC_XLOG_FULL_IMAGE);
	sift_page_init(page, SIFT_PT_LIVE, l->gen);
	memcpy(SiftPageGetData(page), run + (uint64) i * SIFT_PAGE_PAYLOAD, page_bytes(l, i));
	SiftPageSetUsed(page, page_bytes(l, i));
	sift_xlog_finish(&x);
	UnlockReleaseBuffer(buf);
	return true;
}

void
sift_live_repair(const SiftLive *l, const SiftLiveUnit *units, uint32 nunits, SiftLiveAudit *a)
{
	uint8	   *run = live_load(l, a);

	live_derive(l, run, units, nunits, a);
	for (uint32 i = 0; i < l->npages; i++)
		a->pages_written += write_page(l, i, run);
	pfree(run);
}
