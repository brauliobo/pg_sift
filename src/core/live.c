/*
 * core/live.c - liveness pages of immutable segments (core/live.h).
 */
#include "core/host.h"

#include "core/live.h"

#define corrupt(l, ...) SIFT_SEG_CORRUPT((l)->index, (l)->gen, __VA_ARGS__)

SiftSegHeaderData *
sift_seg_read_header(Relation index, const SiftSegEntry *entry)
{
	Buffer		buf = ReadBuffer(index, entry->header_blk);
	Page		page;
	SiftSegHeaderData *h,
			   *copy;
	uint32		used;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(index, buf, SIFT_PT_SEGHEADER, entry->gen);
	page = BufferGetPage(buf);
	h = (SiftSegHeaderData *) SiftPageGetData(page);
	used = SiftPageGetUsed(page);
	if (used < offsetof(SiftSegHeaderData, extents) || h->nextents > SIFT_MAX_EXTENTS ||
		used < offsetof(SiftSegHeaderData, extents) + h->nextents * sizeof(SiftExtent))
	{
		UnlockReleaseBuffer(buf);
		SIFT_SEG_CORRUPT(index, entry->gen, "Header block %u holds %u bytes.", entry->header_blk, used);
	}
	copy = palloc(used);
	memcpy(copy, h, used);
	UnlockReleaseBuffer(buf);
	return copy;
}

static void
set_dirty(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == *(uint64 *) arg)
			dir->segs[i].flags |= SIFT_SEG_DIRTY;
}

/* The directory flag goes first: bits cleared under a clean flag would be ignored by readers */
void
sift_seg_mark_dirty(Relation index, SiftSegEntry *entry)
{
	if (entry->flags & SIFT_SEG_DIRTY)
		return;
	sift_dir_update(index, &(SiftDirChange) {.fn = set_dirty,.arg = &entry->gen,.extra = InvalidBuffer});
	entry->flags |= SIFT_SEG_DIRTY;
}

void
sift_live_write(SiftWriteTarget *wt, uint64 gen, uint32 nsb, uint64 nbits, BlockNumber *first, uint32 *npages)
{
	uint64		start = SIFT_LIVE_START(nsb),
				total = start + (nbits + 7) / 8;
	PGIOAlignedBlock page;

	*npages = (total + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD;
	*first = sift_wt_reserve(wt, *npages);
	for (uint32 i = 0; i < *npages; i++)
	{
		uint64		lo = (uint64) i * SIFT_PAGE_PAYLOAD,
					hi = Min(lo + SIFT_PAGE_PAYLOAD, total);
		char	   *data = SiftPageGetData(page.data);

		sift_page_init(page.data, SIFT_PT_LIVE, gen);
		if (i == 0)
			*(SiftLiveHeaderData *) data = (SiftLiveHeaderData) {nsb, 0};
		for (uint64 b = Max(lo, start); b < hi; b++)
		{
			uint64		r = (b - start) * 8;

			data[b - lo] = nbits - r >= 8 ? 0xFF : (1 << (nbits - r)) - 1;
		}
		SiftPageSetUsed(page.data, hi - lo);
		sift_wt_put_page(wt, *first + i, page.data);
	}
}

void
sift_live_check(const SiftLive *l)
{
	uint64		total = SIFT_LIVE_START(l->nsb) + (l->nbits + 7) / 8;

	if (l->npages == 0 || total > (uint64) l->npages * SIFT_PAGE_PAYLOAD)
		corrupt(l, "%u LIVE pages cannot hold %u dirty bits and %llu liveness bits.", l->npages, l->nsb,
				(unsigned long long) l->nbits);
}

uint8
sift_live_byte(const SiftLive *l, uint64 b, Buffer *buf)
{
	BlockNumber blk = l->first + b / SIFT_PAGE_PAYLOAD;
	Page		page;

	if (b / SIFT_PAGE_PAYLOAD >= l->npages)
		corrupt(l, "Liveness byte %llu lies past %u LIVE pages.", (unsigned long long) b, l->npages);
	if (!BufferIsValid(*buf) || BufferGetBlockNumber(*buf) != blk)
	{
		if (BufferIsValid(*buf))
			UnlockReleaseBuffer(*buf);
		*buf = ReadBuffer(l->index, blk);
		LockBuffer(*buf, BUFFER_LOCK_SHARE);
		sift_store_check_page(l->index, *buf, SIFT_PT_LIVE, l->gen);
	}
	page = BufferGetPage(*buf);
	if (b % SIFT_PAGE_PAYLOAD >= SiftPageGetUsed(page))
		corrupt(l, "Liveness byte %llu lies past the written bytes of block %u.", (unsigned long long) b, blk);
	return SiftPageGetData(page)[b % SIFT_PAGE_PAYLOAD];
}

void
sift_live_copy(const SiftLive *l, uint64 b, uint64 len, uint8 *out)
{
	while (len > 0)
	{
		uint64		off = b % SIFT_PAGE_PAYLOAD,
					n = Min(len, SIFT_PAGE_PAYLOAD - off);
		Buffer		buf = InvalidBuffer;
		Page		page;

		sift_live_byte(l, b, &buf);
		page = BufferGetPage(buf);
		if (off + n > SiftPageGetUsed(page))
			corrupt(l, "Liveness bytes %llu..%llu lie past the written bytes of block %u.",
					(unsigned long long) b, (unsigned long long) (b + n), BufferGetBlockNumber(buf));
		memcpy(out, SiftPageGetData(page) + off, n);
		UnlockReleaseBuffer(buf);
		b += n;
		out += n;
		len -= n;
	}
}

void
sift_live_dirty(const SiftLive *l, uint8 *out)
{
	sift_live_copy(l, sizeof(SiftLiveHeaderData), (l->nsb + 7) / 8, out);
}

uint32
sift_live_ndead(const SiftLive *l)
{
	SiftLiveHeaderData lh;

	sift_live_copy(l, 0, sizeof(lh), (uint8 *) &lh);
	if (lh.ndead > l->nbits)
		corrupt(l, "Liveness header counts %u dead of %llu rows.", lh.ndead, (unsigned long long) l->nbits);
	return lh.ndead;
}

void
sift_live_bits(const SiftLive *l, uint64 rank, uint32 n, uint64 *out, uint32 nwords)
{
	uint64		start = SIFT_LIVE_START(l->nsb);
	Buffer		buf = InvalidBuffer;

	memset(out, 0, nwords * sizeof(uint64));
	for (uint32 i = 0; i < n;)
	{
		uint64		r = rank + i;
		uint32		sh = r % 8,
					take = Min(8 - sh, n - i);
		uint64		byte = (sift_live_byte(l, start + r / 8, &buf) >> sh) & ((1u << take) - 1);

		out[i / 64] |= byte << (i % 64);
		if (i % 64 + take > 64)
			out[i / 64 + 1] |= byte >> (64 - i % 64);
		i += take;
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

void
sift_live_clear(const SiftLive *l, uint32 sbidx, const uint64 *dead, int nd)
{
	uint64		start = SIFT_LIVE_START(l->nsb);
	uint32		pg[3];
	Buffer		buf[3];
	Page		page[3];
	int			n = 1;
	GenericXLogState *state;
	SiftLiveHeaderData *lh;

	pg[0] = 0;
	for (int i = 0; i < nd; i++)
	{
		uint32		p = (start + dead[i] / 8) / SIFT_PAGE_PAYLOAD;

		if (pg[n - 1] != p)
			pg[n++] = p;
	}
	Assert(n <= 3);
	for (int i = 0; i < n; i++)
	{
		buf[i] = ReadBuffer(l->index, l->first + pg[i]);
		LockBuffer(buf[i], BUFFER_LOCK_EXCLUSIVE);
		sift_store_check_page(l->index, buf[i], SIFT_PT_LIVE, l->gen);
	}
	state = GenericXLogStart(l->index);
	for (int i = 0; i < n; i++)
		page[i] = GenericXLogRegisterBuffer(state, buf[i], 0);
	for (int i = 0; i < nd; i++)
	{
		uint64		b = start + dead[i] / 8;
		int			k = 0;

		while (pg[k] != b / SIFT_PAGE_PAYLOAD)
			k++;
		SiftPageGetData(page[k])[b % SIFT_PAGE_PAYLOAD] &= ~(1 << (dead[i] % 8));
	}
	lh = (SiftLiveHeaderData *) SiftPageGetData(page[0]);
	lh->ndead += nd;
	SiftPageGetData(page[0])[sizeof(SiftLiveHeaderData) + sbidx / 8] |= 1 << (sbidx % 8);
	GenericXLogFinish(state);
	for (int i = 0; i < n; i++)
		UnlockReleaseBuffer(buf[i]);
}

#ifndef MAX_GENERIC_XLOG_PAGES
#define MAX_GENERIC_XLOG_PAGES	4
#endif

#define LIVE2_BYTES(h)	((uint64) (h)->tbl_off + (uint64) (h)->nslots * sizeof(uint32))

/* Pages of the run in the making: bytes go in, full pages go out */
typedef struct LiveEmit
{
	SiftWriteTarget *wt;
	uint64		gen;
	BlockNumber first;
	uint32		next;
	uint32		used;
	PGIOAlignedBlock page;
} LiveEmit;

static void
emit_flush(LiveEmit *e)
{
	SiftPageSetUsed(e->page.data, e->used);
	sift_wt_put_page(e->wt, e->first + e->next++, e->page.data);
	e->used = 0;
}

static void
emit(LiveEmit *e, const void *data, uint64 len)
{
	while (len > 0)
	{
		uint64		n = Min(len, SIFT_PAGE_PAYLOAD - e->used);

		if (e->used == 0)
			sift_page_init(e->page.data, SIFT_PT_LIVE, e->gen);
		memcpy(SiftPageGetData(e->page.data) + e->used, data, n);
		e->used += n;
		data = (const char *) data + n;
		len -= n;
		if (e->used == SIFT_PAGE_PAYLOAD)
			emit_flush(e);
	}
}

static void
emit_zeros(LiveEmit *e, uint64 len)
{
	static const char zero[256];

	for (uint64 n; len > 0; len -= n)
	{
		n = Min(len, sizeof(zero));
		emit(e, zero, n);
	}
}

void
sift_live2_write(SiftWriteTarget *wt, uint64 gen, uint32 nsb, uint32 nslots, const uint64 *reg, BufFile *rows,
				 BlockNumber *first, uint32 *npages)
{
	SiftLive2HeaderData h = {.nsb = nsb,.nslots = nslots};
	uint64		dirty_len = TYPEALIGN(8, (nsb + 7) / 8);
	LiveEmit	e = {.wt = wt,.gen = gen};
	char	   *buf = palloc(SIFT_PAGE_PAYLOAD);

	h.reg_off = sizeof(h) + dirty_len;
	h.rows_off = h.reg_off + (uint64) (nsb + 1) * sizeof(uint64);
	h.tbl_off = h.rows_off + reg[nsb];
	*npages = (LIVE2_BYTES(&h) + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD;
	e.first = *first = sift_wt_reserve(wt, *npages);
	emit(&e, &h, sizeof(h));
	emit_zeros(&e, dirty_len);
	emit(&e, reg, (uint64) (nsb + 1) * sizeof(uint64));
	SIFT_INVARIANT(BufFileSeek(rows, 0, 0, SEEK_SET) == 0, "sift: could not rewind a spool file");
	for (uint64 left = reg[nsb], n; left > 0; left -= n)
	{
		n = Min(left, SIFT_PAGE_PAYLOAD);
		BufFileReadExact(rows, buf, n);
		emit(&e, buf, n);
	}
	emit_zeros(&e, (uint64) nslots * sizeof(uint32));
	if (e.used > 0)
		emit_flush(&e);
	SIFT_INVARIANT(e.next == *npages, "sift: LIVE run of %u pages written as %u", *npages, e.next);
	pfree(buf);
}

void
sift_live2_header(const SiftLive *l, SiftLive2HeaderData *h)
{
	uint64		dirty_len;

	sift_live_copy(l, 0, sizeof(*h), (uint8 *) h);
	dirty_len = TYPEALIGN(8, (l->nsb + 7) / 8);
	if (h->nsb != l->nsb || h->reg_off != sizeof(*h) + dirty_len ||
		h->rows_off != h->reg_off + (uint64) (l->nsb + 1) * sizeof(uint64) || h->tbl_off < h->rows_off ||
		h->tbl_off % sizeof(uint32) != 0 || LIVE2_BYTES(h) > (uint64) l->npages * SIFT_PAGE_PAYLOAD ||
		h->ndead > l->nbits || h->tbl_ndead > h->ndead)
		corrupt(l, "Liveness header of %u SBs, %u dead, %u slots, offsets %llu/%llu/%llu does not fit %u LIVE pages and %llu docs.",
				h->nsb, h->ndead, h->nslots, (unsigned long long) h->reg_off, (unsigned long long) h->rows_off,
				(unsigned long long) h->tbl_off, l->npages, (unsigned long long) l->nbits);
}

void
sift_live2_dirty(const SiftLive *l, const SiftLive2HeaderData *h, uint8 *out)
{
	sift_live_copy(l, sizeof(*h), (h->nsb + 7) / 8, out);
}

uint64
sift_live2_region(const SiftLive *l, const SiftLive2HeaderData *h, uint32 sbidx, uint64 *len)
{
	uint64		reg[2];

	sift_live_copy(l, h->reg_off + (uint64) sbidx * sizeof(uint64), sizeof(reg), (uint8 *) reg);
	if (reg[1] < reg[0] || h->rows_off + reg[1] > h->tbl_off)
		corrupt(l, "Region of SB %u spans %llu to %llu of %llu bytes.", sbidx, (unsigned long long) reg[0],
				(unsigned long long) reg[1], (unsigned long long) (h->tbl_off - h->rows_off));
	*len = reg[1] - reg[0];
	return h->rows_off + reg[0];
}

void
sift_live2_clear(const SiftLive *l, uint32 sbidx, const uint64 *bits, int nd, uint64 dead_dl)
{
	uint32		pg[MAX_GENERIC_XLOG_PAGES];
	Buffer		buf[MAX_GENERIC_XLOG_PAGES];
	Page		page[MAX_GENERIC_XLOG_PAGES];
	uint64		dirty = sizeof(SiftLive2HeaderData) + sbidx / 8;
	int			n = 1;
	GenericXLogState *state;
	SiftLive2HeaderData *h;

	pg[0] = 0;
	for (int i = -1; i < nd; i++)
	{
		uint32		p = (i < 0 ? dirty : bits[i] / 8) / SIFT_PAGE_PAYLOAD;
		int			k = 0;

		while (k < n && pg[k] != p)
			k++;
		if (k == n)
		{
			if (n == MAX_GENERIC_XLOG_PAGES)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("sift: the docs of one superblock span more than %d LIVE pages", MAX_GENERIC_XLOG_PAGES - 1),
						 errhint("Run sift.merge() on the index to rewrite the segment without its dead documents.")));
			pg[n++] = p;
		}
	}
	for (int i = 0; i < n; i++)
	{
		buf[i] = ReadBuffer(l->index, l->first + pg[i]);
		LockBuffer(buf[i], BUFFER_LOCK_EXCLUSIVE);
		sift_store_check_page(l->index, buf[i], SIFT_PT_LIVE, l->gen);
	}
	state = GenericXLogStart(l->index);
	for (int i = 0; i < n; i++)
		page[i] = GenericXLogRegisterBuffer(state, buf[i], 0);
	for (int i = -1; i < nd; i++)
	{
		uint64		b = i < 0 ? dirty : bits[i] / 8;
		uint8		m = i < 0 ? 1 << (sbidx % 8) : 1 << (bits[i] % 8);
		uint8	   *at;
		int			k = 0;

		while (pg[k] != b / SIFT_PAGE_PAYLOAD)
			k++;
		at = (uint8 *) SiftPageGetData(page[k]) + b % SIFT_PAGE_PAYLOAD;
		*at = i < 0 ? *at | m : *at & ~m;
	}
	h = (SiftLive2HeaderData *) SiftPageGetData(page[0]);
	h->ndead += nd;
	h->dead_dl += dead_dl;
	GenericXLogFinish(state);
	for (int i = 0; i < n; i++)
		UnlockReleaseBuffer(buf[i]);
}

uint32
sift_live2_dead(const SiftLive *l, const SiftLive2HeaderData *h, uint32 slot)
{
	uint32		v;

	if (slot >= h->nslots)
		corrupt(l, "Slot %u of a table of %u.", slot, h->nslots);
	sift_live_copy(l, h->tbl_off + (uint64) slot * sizeof(uint32), sizeof(v), (uint8 *) &v);
	return v;
}

void
sift_live2_dead_all(const SiftLive *l, const SiftLive2HeaderData *h, uint32 *out)
{
	sift_live_copy(l, h->tbl_off, (uint64) h->nslots * sizeof(uint32), (uint8 *) out);
}

void
sift_live2_set_dead(const SiftLive *l, const SiftLive2HeaderData *h, const uint32 *dead)
{
	uint64		lo = h->tbl_off,
				hi = LIVE2_BYTES(h);
	const uint8 *src = (const uint8 *) dead;
	SiftLive2HeaderData *cur;
	Buffer		buf;
	Page		page;
	GenericXLogState *state;

	for (uint64 b = lo; b < hi;)
	{
		uint64		off = b % SIFT_PAGE_PAYLOAD,
					n = Min(hi - b, SIFT_PAGE_PAYLOAD - off);

		buf = ReadBuffer(l->index, l->first + b / SIFT_PAGE_PAYLOAD);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		sift_store_check_page(l->index, buf, SIFT_PT_LIVE, l->gen);
		if (memcmp(SiftPageGetData(BufferGetPage(buf)) + off, src + (b - lo), n) != 0)
		{
			state = GenericXLogStart(l->index);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			memcpy(SiftPageGetData(page) + off, src + (b - lo), n);
			GenericXLogFinish(state);
		}
		UnlockReleaseBuffer(buf);
		b += n;
	}
	buf = ReadBuffer(l->index, l->first);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	sift_store_check_page(l->index, buf, SIFT_PT_LIVE, l->gen);
	state = GenericXLogStart(l->index);
	page = GenericXLogRegisterBuffer(state, buf, 0);
	cur = (SiftLive2HeaderData *) SiftPageGetData(page);
	cur->tbl_ndead = cur->ndead;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
}
