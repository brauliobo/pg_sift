/*
 * sift_live.c - liveness pages of immutable segments (sift_live.h).
 */
#include "sift_host.h"

#include "sift_live.h"

#define corrupt(l, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" segment %llu is corrupted", \
					RelationGetRelationName((l)->index), (unsigned long long) (l)->gen), \
			 errdetail(__VA_ARGS__)))

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
