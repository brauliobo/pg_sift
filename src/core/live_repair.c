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
	uint64		lo = SIFT_PAYLOAD_BYTES(i),
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

	return o->type == SIFT_PT_LIVE && o->flags == 0 && o->part_id == 0 && o->gen == gen;
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
	SIFT_DAMAGE_INDEX(l->index,
					  errmsg("sift index \"%s\" segment %llu is corrupted", RelationGetRelationName(l->index), (unsigned long long) l->gen),
					  errdetail("Liveness block %u has neither the header nor the special space of a liveness page.", blk),
					  errhint("The page cannot be restored in place: REINDEX the index."));
}

/* The first logical byte of the liveness bits, and the bytes they are */
#define BITS_START(l)	SIFT_LIVE_START((l)->nsb)
#define BITS_BYTES(l)	(((l)->nbits + 7) / 8)

/*
 * The audit goes over the pages one at a time. head holds what comes before the bits (the header and the dirty bitmap, at most 16 kB for 131,000 superblocks), which is
 * what a repair rewrites; the bits themselves are counted and tested as their page goes by, and the unit that spans two pages carries its answer to the next.
 */
typedef struct LiveWalk
{
	const SiftLive *l;
	const SiftLiveUnit *units;
	uint32		nunits;
	uint32		u;				/* the unit in hand */
	bool		u_found;		/* a cleared bit in the part of it that went by */
	uint8	   *head;
	uint64		live;			/* the bits set so far */
	SiftLiveAudit *a;
} LiveWalk;

/* Is a bit of rank first to end - 1 cleared? buf holds the bits from rank buf_first (a multiple of 8) on */
static bool
any_cleared(const uint8 *buf, uint64 buf_first, uint64 first, uint64 end)
{
	for (uint64 r = first; r < end; r++)
	{
		uint64		at = r - buf_first;

		if (r % 8 == 0 && r + 8 <= end && buf[at / 8] == 0xFF)
			r += 7;
		else if (!(buf[at / 8] >> (r % 8) & 1))
			return true;
	}
	return false;
}

static void
walk_unit_done(LiveWalk *w)
{
	const SiftLiveUnit *u = &w->units[w->u++];
	uint8	   *dirty = w->head + sizeof(SiftLiveHeaderData);

	if (!(dirty[u->sbidx / 8] >> (u->sbidx % 8) & 1) && w->u_found)
	{
		dirty[u->sbidx / 8] |= 1 << (u->sbidx % 8);
		w->a->units_lost++;
	}
	w->u_found = false;
}

/* The bits of page i (its payload is image): counted, and tested for the units that lie over them */
static void
walk_bits(LiveWalk *w, uint32 i, const uint8 *image, uint32 nbytes)
{
	const SiftLive *l = w->l;
	uint64		p0 = SIFT_PAYLOAD_BYTES(i),
				s = BITS_START(l),
				b0 = Max(p0, s) - s,
				b1 = Min(p0 + nbytes, s + BITS_BYTES(l)) - s,
				whole = l->nbits / 8;
	const uint8 *bits;
	uint64		first;

	if (p0 + nbytes <= s || b1 <= b0)
		return;
	bits = image + (s + b0 - p0);
	first = b0 * 8;
	w->live += pg_popcount((const char *) bits, Min(b1, whole) > b0 ? Min(b1, whole) - b0 : 0);
	if (b1 > whole && l->nbits % 8)
		w->live += pg_popcount32(bits[whole - b0] & ((1 << (l->nbits % 8)) - 1));
	while (w->u < w->nunits)
	{
		const SiftLiveUnit *u = &w->units[w->u];
		uint64		ub1 = u->rank_base + u->nrows;

		if (u->rank_base >= b1 * 8)
			break;
		w->u_found |= any_cleared(bits, first, Max(u->rank_base, first), Min(ub1, Min(b1 * 8, l->nbits)));
		if (ub1 > b1 * 8)
			break;
		walk_unit_done(w);
	}
}

static void
walk_page(LiveWalk *w, uint32 i, const uint8 *image, uint32 nbytes)
{
	uint64		p0 = SIFT_PAYLOAD_BYTES(i),
				s = BITS_START(w->l);

	if (p0 < s)
		memcpy(w->head + p0, image, Min(nbytes, s - p0));
	walk_bits(w, i, image, nbytes);
}

/* The pages of the run, one at a time, into w: the page that is not as a LIVE page of the segment is counted. Left in w->head: the image of what comes before the bits that a repair writes */
static void
live_walk(const SiftLive *l, const SiftLiveUnit *units, uint32 nunits, SiftLiveAudit *a, LiveWalk *w)
{
	uint8		image[SIFT_PAGE_PAYLOAD];
	uint64		prev_end = 0;

	*a = (SiftLiveAudit) {.first_damaged = InvalidBlockNumber};
	*w = (LiveWalk) {.l = l, .units = units, .nunits = nunits, .head = palloc0(BITS_START(l)), .a = a};
	for (uint32 i = 0; i < nunits; i++)
	{
		const SiftLiveUnit *u = &units[i];

		if (u->sbidx >= l->nsb || u->rank_base + u->nrows > l->nbits)
			SIFT_SEG_CORRUPT(l->index, l->gen, "Unit %u of the segment (rows %llu to %llu of %llu, superblock %u of %u) lies outside its liveness run.", i,
							 (unsigned long long) u->rank_base, (unsigned long long) (u->rank_base + u->nrows), (unsigned long long) l->nbits, u->sbidx, l->nsb);
		if (u->rank_base < prev_end)
			SIFT_SEG_CORRUPT(l->index, l->gen, "Unit %u of the segment starts at row %llu, inside the unit before it.", i, (unsigned long long) u->rank_base);
		prev_end = u->rank_base + u->nrows;
	}
	for (uint32 i = 0; i < l->npages; i++)
	{
		BlockNumber blk = l->first + i;
		Buffer		buf = ReadBuffer(l->index, blk);
		Page		page;
		uint32		nbytes = page_bytes(l, i);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!header_ok(l, i, page) && !special_ok(page, l->gen))
			page_lost(l, blk);
		if (!header_ok(l, i, page) || !special_ok(page, l->gen))
		{
			if (a->pages_damaged++ == 0)
				a->first_damaged = blk;
		}
		memcpy(image, SiftPageGetData(page), nbytes);
		UnlockReleaseBuffer(buf);
		if (i == 0)
			a->ndead_header = ((const SiftLiveHeaderData *) image)->ndead;
		walk_page(w, i, image, nbytes);
	}
	while (w->u < w->nunits)
		walk_unit_done(w);
	a->cleared = l->nbits - w->live;
	((SiftLiveHeaderData *) w->head)->ndead = a->cleared;
}

void
sift_live_audit(const SiftLive *l, const SiftLiveUnit *units, uint32 nunits, SiftLiveAudit *a)
{
	LiveWalk	w;

	live_walk(l, units, nunits, a, &w);
	pfree(w.head);
}

/* A page that differs is written whole, laid out as sift_live_write lays it out: nothing of a header or a special space that is damaged is kept, and a replica replays the bytes and not a change of its own copy */
static bool
write_page(const SiftLive *l, uint32 i, const uint8 *head)
{
	Buffer		buf = ReadBuffer(l->index, l->first + i);
	Page		page;
	SiftXlog	x;
	uint8		image[SIFT_PAGE_PAYLOAD];
	uint32		nbytes = page_bytes(l, i);
	uint64		p0 = SIFT_PAYLOAD_BYTES(i),
				s = BITS_START(l);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	memcpy(image, SiftPageGetData(page), nbytes);
	if (p0 < s)
		memcpy(image, head + p0, Min(nbytes, s - p0));
	if (header_ok(l, i, page) && special_ok(page, l->gen) && memcmp(SiftPageGetData(page), image, nbytes) == 0)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	x = sift_xlog_start(l->index);
	page = sift_xlog_register(&x, buf, GENERIC_XLOG_FULL_IMAGE);
	sift_page_init(page, SIFT_PT_LIVE, l->gen);
	memcpy(SiftPageGetData(page), image, nbytes);
	SiftPageSetUsed(page, nbytes);
	sift_xlog_finish(&x);
	UnlockReleaseBuffer(buf);
	return true;
}

void
sift_live_repair(const SiftLive *l, const SiftLiveUnit *units, uint32 nunits, SiftLiveAudit *a)
{
	LiveWalk	w;

	live_walk(l, units, nunits, a, &w);
	for (uint32 i = 0; i < l->npages; i++)
		a->pages_written += write_page(l, i, w.head);
	pfree(w.head);
}
