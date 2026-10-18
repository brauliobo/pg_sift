/*
 * sift_segment.c - immutable segment writer and reader (layout in
 * sift_segment.h and sift_page.h).
 */
#include "sift_host.h"

#include "sift_kernels.h"
#include "sift_segment.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
#define ULL(x)			((unsigned long long) (x))
#define MAXOFF			SIFT_MAX_OFFSET
#define PAGE_RANK_WORDS	((MAXOFF + 63) / 64)	/* words of rank bits of a page of docs */
#define LIVE_START(nsb)	TYPEALIGN(8, sizeof(SiftLiveHeaderData) + ((nsb) + 7) / 8)

#define corrupt(seg, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" segment %llu is corrupted", \
					RelationGetRelationName((seg)->index), ULL((seg)->entry.gen)), \
			 errdetail(__VA_ARGS__)))

const uint32 sift_df_class_lo[SIFT_DF_CLASSES] = {1, 2, 3, 16, 128, 1024, 8192, 65536, 524288};
const char *const sift_df_class_name[SIFT_DF_CLASSES] = {
	"1", "2", "3-15", "16-127", "128-1K", "1K-8K", "8K-64K", "64K-512K", "512K+"
};

struct SiftSegmentWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	MemoryContext mcxt;
	SiftStreamWriter *sw;
	SiftScoreRef ref;
	SiftSegHeaderData hdr;
	uint64		ndocs;
	uint64		sum_dl;
	SiftKey		last;
	bool		positions;
	/* SB being accumulated */
	uint32		sb;
	uint32		sb_ndocs;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint16		maxoff[SIFT_SB_PAGES];
	uint64	   *offmask;		/* [page][SIFT_OFFMASK_WORDS] */
	uint16	   *dl;				/* [page][MAXOFF] */
	SiftDocSBRef *sbdir;
	uint32		nsb;
	uint32		maxsb;
	/* terms */
	SiftPostingWriter *pw;
	SiftDictWriter *dw;
	char		term[SIFT_MAX_TERM_BYTES];
	int			termlen;
};

/* Reader docmap state */
typedef struct DocMap
{
	SiftDocSBRef *dir;
	uint64	   *rank_base;
	uint32		nsb;
	uint64		dir_off;
	int64		cur;			/* loaded SB (index into dir), -1 none */
	uint8	   *rec;
	Size		reccap;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint64	   *om;				/* canonical offset masks of present pages */
	uint32		before[SIFT_SB_PAGES];	/* docs on earlier pages (present pages) */
	uint8		maskidx[SIFT_SB_PAGES]; /* present-page index (present pages) */
	const uint16 *rows[SIFT_SB_PAGES];
	uint16		rowlen[SIFT_SB_PAGES];
	bool		live_ok;		/* live_bits holds the bits of SB live_sb */
	uint32		live_sb;
	uint8	   *live_bits;		/* the SB's docs, bit i for rank rank_base + i, from bit rank_base % 8 */
	Size		live_cap;
} DocMap;

static void
sw_pad4(SiftStreamWriter *sw)
{
	sift_sw_write(sw, "\0\0\0", ALIGN4(sift_sw_tell(sw)) - sift_sw_tell(sw));
}

/* ---------- writer ---------- */

SiftSegmentWriter *
sift_segw_begin(SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref, MemoryContext mcxt, bool positions)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftSegmentWriter *w = palloc0(sizeof(SiftSegmentWriter));

	w->wt = wt;
	w->gen = gen;
	w->mcxt = mcxt;
	w->ref = *ref;
	w->positions = positions;
	w->sw = sift_sw_begin(wt, gen);
	w->offmask = palloc0(SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64));
	w->dl = palloc0(SIFT_SB_PAGES * MAXOFF * sizeof(uint16));
	w->maxsb = 64;
	w->sbdir = palloc(w->maxsb * sizeof(SiftDocSBRef));
	w->termlen = -1;
	MemoryContextSwitchTo(old);
	return w;
}

static void
flush_sb(SiftSegmentWriter *w)
{
	uint64	   *om = palloc(SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64));
	uint8	   *chunk;
	uint32		hdr[2];
	int			np = 0;

	if (w->sb_ndocs == 0)
		return;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
		if (w->pagemask[p / 64] & (1ULL << (p % 64)))
			memcpy(om + SIFT_OFFMASK_WORDS * np++, w->offmask + SIFT_OFFMASK_WORDS * p,
				   SIFT_OFFMASK_WORDS * sizeof(uint64));
	hdr[0] = w->sb_ndocs;
	hdr[1] = sift_chunk_encode(w->pagemask, om, SIFT_CT_AUTO, NULL);
	chunk = palloc(hdr[1]);
	sift_chunk_encode(w->pagemask, om, SIFT_CT_AUTO, chunk);
	if (w->nsb == w->maxsb)
	{
		w->maxsb *= 2;
		w->sbdir = repalloc_huge(w->sbdir, w->maxsb * sizeof(SiftDocSBRef));
	}
	sw_pad4(w->sw);
	w->sbdir[w->nsb++] = (SiftDocSBRef) {w->sb, w->sb_ndocs, sift_sw_tell(w->sw)};
	sift_sw_write(w->sw, hdr, sizeof(hdr));
	sift_sw_write(w->sw, chunk, hdr[1]);
	sw_pad4(w->sw);
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		if (!(w->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_sw_write(w->sw, w->dl + p * MAXOFF, w->maxoff[p] * sizeof(uint16));
		memset(w->dl + p * MAXOFF, 0, w->maxoff[p] * sizeof(uint16));
		memset(w->offmask + p * SIFT_OFFMASK_WORDS, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		w->maxoff[p] = 0;
	}
	memset(w->pagemask, 0, sizeof(w->pagemask));
	w->sb_ndocs = 0;
	pfree(om);
	pfree(chunk);
}

void
sift_segw_add_docs(SiftSegmentWriter *w, const SiftKey *keys, const uint16 *dl, int n)
{
	if (w->pw)
		elog(ERROR, "sift: segment docs must be added before terms");
	for (int i = 0; i < n; i++)
	{
		BlockNumber blk = sift_key_block(keys[i]);
		uint32		off = keys[i] & 0xFFFF,
					page = blk & (SIFT_SB_PAGES - 1);

		if ((w->ndocs > 0 && keys[i] <= w->last) || off < 1 || off > MAXOFF)
			elog(ERROR, "sift: segment doc (%u,%u) dl %u out of order or invalid", blk, off, dl[i]);
		if (sift_block_sb(blk) != w->sb)
		{
			flush_sb(w);
			w->sb = sift_block_sb(blk);
		}
		if (w->ndocs == 0)
			w->hdr.sb_lo = w->sb;
		w->pagemask[page / 64] |= 1ULL << (page % 64);
		w->offmask[page * SIFT_OFFMASK_WORDS + (off - 1) / 64] |= 1ULL << ((off - 1) % 64);
		w->dl[page * MAXOFF + off - 1] = dl[i];
		w->maxoff[page] = Max(w->maxoff[page], off);
		w->sb_ndocs++;
		w->ndocs++;
		w->sum_dl += dl[i];
		w->last = keys[i];
	}
}

/* Closes DOCMAP and opens POSTINGS on the first term */
static void
start_terms(SiftSegmentWriter *w)
{
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	if (w->ndocs == 0)
		elog(ERROR, "sift: segment terms without docs");
	flush_sb(w);
	w->hdr.sb_hi = w->sb;
	sw_pad4(w->sw);
	sift_sw_write(w->sw, w->sbdir, w->nsb * sizeof(SiftDocSBRef));
	sift_sw_write(w->sw, &w->nsb, sizeof(uint32));
	w->hdr.sections[SIFT_SEC_DOCMAP].len = sift_sw_tell(w->sw);
	w->hdr.sections[SIFT_SEC_POSTINGS].off = sift_sw_tell(w->sw);
	w->ref.avgdl = (float4) ((double) w->sum_dl / w->ndocs);
	w->pw = sift_postw_begin(w->sw, &w->ref, w->mcxt);
	w->dw = sift_dict_writer_begin(w->sw);
	pfree(w->offmask);
	pfree(w->dl);
	MemoryContextSwitchTo(old);
}

SiftPostingWriter *
sift_segw_begin_term(SiftSegmentWriter *w, const char *term, int len)
{
	if (w->pw == NULL)
		start_terms(w);
	if (len < 1 || len > SIFT_MAX_TERM_BYTES)
		elog(ERROR, "sift: term of %d bytes", len);
	memcpy(w->term, term, len);
	w->termlen = len;
	return w->pw;
}

void
sift_segw_end_term(SiftSegmentWriter *w)
{
	SiftTermInfo info;

	sift_postw_end_term(w->pw, &info);
	sift_dict_add(w->dw, w->term, w->termlen, &info);
}

/* All-live LIVE pages: header, clean dirty-SB bitmap, ndocs set bits */
static void
write_live(SiftSegmentWriter *w)
{
	uint32		nsb = w->hdr.sb_hi - w->hdr.sb_lo + 1;
	uint64		start = LIVE_START(nsb),
				total = start + (w->ndocs + 7) / 8;
	uint32		npages = (total + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD;
	PGIOAlignedBlock page;

	w->hdr.live_npages = npages;
	w->hdr.live_first = sift_wt_reserve(w->wt, npages);
	for (uint32 i = 0; i < npages; i++)
	{
		uint64		lo = (uint64) i * SIFT_PAGE_PAYLOAD,
					hi = Min(lo + SIFT_PAGE_PAYLOAD, total);
		char	   *data = SiftPageGetData(page.data);

		sift_page_init(page.data, SIFT_PT_LIVE, w->gen);
		if (i == 0)
			*(SiftLiveHeaderData *) data = (SiftLiveHeaderData) {nsb, 0};
		for (uint64 b = Max(lo, start); b < hi; b++)
		{
			uint64		r = (b - start) * 8;

			data[b - lo] = w->ndocs - r >= 8 ? 0xFF : (1 << (w->ndocs - r)) - 1;
		}
		SiftPageSetUsed(page.data, hi - lo);
		sift_wt_put_page(w->wt, w->hdr.live_first + i, page.data);
	}
}

void
sift_segw_finish(SiftSegmentWriter *w, SiftSegEntry *entry)
{
	PGIOAlignedBlock page;
	SiftSegHeaderData *hdr = (SiftSegHeaderData *) SiftPageGetData(page.data);
	SiftExtent *ext;
	int			n;
	uint64		blocks;

	if (w->pw == NULL)
		start_terms(w);
	w->hdr.sections[SIFT_SEC_POSTINGS].len = sift_sw_tell(w->sw) - w->hdr.sections[SIFT_SEC_POSTINGS].off;
	sift_dict_writer_finish(w->dw, &w->hdr);
	ext = sift_sw_finish(w->sw, &n);
	write_live(w);
	w->hdr.gen = w->gen;
	w->hdr.ndocs = w->ndocs;
	w->hdr.sum_dl = w->sum_dl;
	w->hdr.avgdl_ref = w->ref.avgdl;
	w->hdr.nextents = n;
	sift_page_init(page.data, SIFT_PT_SEGHEADER, w->gen);
	memcpy(hdr, &w->hdr, offsetof(SiftSegHeaderData, extents));
	memcpy(hdr->extents, ext, n * sizeof(SiftExtent));
	SiftPageSetUsed(page.data, offsetof(SiftSegHeaderData, extents) + n * sizeof(SiftExtent));
	blocks = 1 + w->hdr.live_npages;
	for (int i = 0; i < n; i++)
		blocks += ext[i].nblocks;
	memset(entry, 0, sizeof(SiftSegEntry));
	entry->gen = w->gen;
	entry->kind = SIFT_SEG_IMMUTABLE;
	entry->format = SIFT_FORMAT_MINOR;
	entry->flags = w->positions ? SIFT_SEG_POSITIONS : 0;
	entry->header_blk = sift_wt_write_page(w->wt, page.data);
	entry->sb_lo = w->hdr.sb_lo;
	entry->sb_hi = w->hdr.sb_hi;
	entry->ndocs = w->ndocs;
	entry->sum_dl = w->sum_dl;
	entry->bytes = blocks * BLCKSZ;
}

/* ---------- reader ---------- */

SiftSegment *
sift_seg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftSegment *seg = palloc0(sizeof(SiftSegment));
	Buffer		buf;
	Page		page;
	SiftSegHeaderData *h;
	uint32		used;

	seg->index = index;
	seg->entry = *entry;
	seg->mcxt = mcxt;
	if (entry->kind != SIFT_SEG_IMMUTABLE)
		elog(ERROR, "sift: segment %llu is not immutable", ULL(entry->gen));
	buf = ReadBuffer(index, entry->header_blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(index, buf, SIFT_PT_SEGHEADER, entry->gen);
	page = BufferGetPage(buf);
	h = (SiftSegHeaderData *) SiftPageGetData(page);
	used = SiftPageGetUsed(page);
	if (used < offsetof(SiftSegHeaderData, extents) || h->nextents > SIFT_MAX_EXTENTS ||
		used < offsetof(SiftSegHeaderData, extents) + h->nextents * sizeof(SiftExtent))
	{
		UnlockReleaseBuffer(buf);
		corrupt(seg, "Header block %u holds %u bytes.", entry->header_blk, used);
	}
	seg->hdr = palloc(used);
	memcpy(seg->hdr, h, used);
	UnlockReleaseBuffer(buf);
	h = seg->hdr;
	if (h->gen != entry->gen || h->ndocs != entry->ndocs || h->sum_dl != entry->sum_dl ||
		h->ndocs == 0 || h->sb_lo != entry->sb_lo || h->sb_hi != entry->sb_hi || h->sb_lo > h->sb_hi ||
		!(h->avgdl_ref > 0 || (h->avgdl_ref == 0 && h->sum_dl == 0)) || h->live_npages == 0)
		corrupt(seg, "Header (gen %llu, %llu docs, SBs %u-%u, avgdl %g) does not match its directory entry.",
				ULL(h->gen), ULL(h->ndocs), h->sb_lo, h->sb_hi, h->avgdl_ref);
	sift_sr_init(&seg->sr, index, h->extents, h->nextents, entry->gen);
	{
		const SiftSectionRef *s = h->sections;
		uint64		size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;

		if (s[SIFT_SEC_DOCMAP].off != 0 || s[SIFT_SEC_DOCMAP].len < sizeof(uint32) ||
			s[SIFT_SEC_POSTINGS].off != s[SIFT_SEC_DOCMAP].len ||
			s[SIFT_SEC_DICT].off != s[SIFT_SEC_POSTINGS].off + s[SIFT_SEC_POSTINGS].len ||
			h->dictroot_off + (uint64) h->dictroot_n * sizeof(uint64) > size)
			corrupt(seg, "Sections do not tile the %llu-byte stream.", ULL(size));
	}
	seg->dict = sift_dict_open(&seg->sr, h, mcxt);
	MemoryContextSwitchTo(old);
	return seg;
}

void
sift_seg_close(SiftSegment *seg)
{
	sift_sr_release(&seg->sr);
}

static const void *
seg_read(SiftSegment *seg, uint64 off, size_t len, void *scratch)
{
	const void *p = sift_sr_get(&seg->sr, off, len, scratch);

	if (p != scratch)
		memcpy(scratch, p, len);
	return scratch;
}

static DocMap *
docmap(SiftSegment *seg)
{
	DocMap	   *dm = seg->docmap;
	const SiftSectionRef *s = &seg->hdr->sections[SIFT_SEC_DOCMAP];
	uint64		rank = 0;
	uint32		nsb;

	if (dm)
		return dm;
	dm = MemoryContextAllocZero(seg->mcxt, sizeof(DocMap));
	seg_read(seg, s->off + s->len - sizeof(uint32), sizeof(uint32), &nsb);
	if ((uint64) nsb * sizeof(SiftDocSBRef) + sizeof(uint32) > s->len || nsb == 0 ||
		nsb > seg->hdr->sb_hi - seg->hdr->sb_lo + 1)
		corrupt(seg, "DOCMAP of %llu bytes lists %u SBs.", ULL(s->len), nsb);
	dm->nsb = nsb;
	dm->dir_off = s->off + s->len - sizeof(uint32) - nsb * sizeof(SiftDocSBRef);
	dm->dir = MemoryContextAllocHuge(seg->mcxt, nsb * sizeof(SiftDocSBRef));
	dm->rank_base = MemoryContextAllocHuge(seg->mcxt, (nsb + 1) * sizeof(uint64));
	seg_read(seg, dm->dir_off, nsb * sizeof(SiftDocSBRef), dm->dir);
	for (uint32 i = 0; i < nsb; i++)
	{
		const SiftDocSBRef *r = &dm->dir[i];

		if (r->sb < seg->hdr->sb_lo || r->sb > seg->hdr->sb_hi || (i > 0 && r->sb <= dm->dir[i - 1].sb) ||
			r->off % 4 != 0 || r->off < (i > 0 ? dm->dir[i - 1].off + 8 : s->off) ||
			r->off + 8 > dm->dir_off || r->ndocs == 0)
			corrupt(seg, "DOCMAP entry %u (SB %u at %llu, %u docs) is out of order.",
					i, r->sb, ULL(r->off), r->ndocs);
		dm->rank_base[i] = rank;
		rank += r->ndocs;
	}
	dm->rank_base[nsb] = rank;
	if (rank != seg->hdr->ndocs)
		corrupt(seg, "DOCMAP lists %llu docs, the header %llu.", ULL(rank), ULL(seg->hdr->ndocs));
	dm->cur = -1;
	dm->om = MemoryContextAllocAligned(seg->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
									   SIFT_SIMD_ALIGN, 0);
	seg->docmap = dm;
	return dm;
}

static void
load_sb(SiftSegment *seg, DocMap *dm, int64 i)
{
	const SiftDocSBRef *r = &dm->dir[i];
	uint64		len = (i + 1 < dm->nsb ? dm->dir[i + 1].off : dm->dir_off) - r->off;
	uint64		all[SIFT_PAGEMASK_WORDS] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
	uint32	   *h;
	const uint16 *row;
	uint32		np,
				k = 0,
				ndocs = 0;

	if (dm->cur == i)
		return;
	dm->cur = -1;
	if (len > dm->reccap)
	{
		dm->reccap = Max(len, 4096);
		if (dm->rec)
			pfree(dm->rec);
		dm->rec = MemoryContextAllocAligned(seg->mcxt, dm->reccap, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	}
	seg_read(seg, r->off, len, dm->rec);
	h = (uint32 *) dm->rec;
	if (h[0] != r->ndocs || 8 + (uint64) h[1] > len || h[1] < 4 || sift_chunk_count(dm->rec + 8) != h[0])
		corrupt(seg, "DOCMAP record of SB %u has %u docs and a chunk of %u bytes.", r->sb, h[0], h[1]);
	sift_chunk_pagemask(dm->rec + 8, dm->pagemask);
	np = sift_chunk_offsets(dm->rec + 8, all, dm->om);
	row = (const uint16 *) (dm->rec + ALIGN4(8 + h[1]));
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		const uint64 *om = dm->om + k * SIFT_OFFMASK_WORDS;
		int			maxoff = 0;

		dm->rows[p] = NULL;
		if (!(dm->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		for (int wd = SIFT_OFFMASK_WORDS - 1; wd >= 0 && maxoff == 0; wd--)
			if (om[wd])
				maxoff = wd * 64 + 64 - __builtin_clzll(om[wd]);
		if (k >= np || maxoff == 0 || maxoff > MAXOFF)
			corrupt(seg, "DOCMAP record of SB %u has an empty or oversized page %d.", r->sb, p);
		dm->before[p] = ndocs;
		dm->maskidx[p] = k++;
		dm->rows[p] = row;
		dm->rowlen[p] = maxoff;
		row += maxoff;
		ndocs += sift_offmask_popcount(om, 1);
	}
	if ((const uint8 *) row > dm->rec + len || ALIGN4((const uint8 *) row - dm->rec) != len)
		corrupt(seg, "DOCMAP record of SB %u spans %llu bytes, its rows %llu.",
				r->sb, ULL(len), ULL((const uint8 *) row - dm->rec));
	dm->cur = i;
}

static void
fill_chunk(SiftSegment *seg, DocMap *dm, int64 i, SiftDocChunk *dc)
{
	load_sb(seg, dm, i);
	dc->sb = dm->dir[i].sb;
	dc->ndocs = dm->dir[i].ndocs;
	memcpy(dc->pagemask, dm->pagemask, sizeof(dc->pagemask));
	dc->rank_base = dm->rank_base[i];
	dc->dirty = seg->live && (((uint8 *) seg->live)[(dc->sb - seg->hdr->sb_lo) / 8] >> ((dc->sb - seg->hdr->sb_lo) % 8)) & 1;
}

/* Index of the first SB entry >= sb */
static int64
sb_search(DocMap *dm, uint32 sb)
{
	int64		lo = 0,
				hi = dm->nsb;

	while (lo < hi)
	{
		int64		mid = (lo + hi) / 2;

		if (dm->dir[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void load_live_header(SiftSegment *seg);

bool
sift_seg_docs_next(SiftSegment *seg, uint32 sb, SiftDocChunk *dc)
{
	DocMap	   *dm = docmap(seg);
	int64		i = sb_search(dm, sb);

	if (i == dm->nsb)
		return false;
	load_live_header(seg);
	fill_chunk(seg, dm, i, dc);
	return true;
}

bool
sift_seg_docs(SiftSegment *seg, uint32 sb, SiftDocChunk *dc)
{
	return sift_seg_docs_next(seg, sb, dc) && dc->sb == sb;
}

static DocMap *
loaded(SiftSegment *seg, const SiftDocChunk *dc)
{
	DocMap	   *dm = docmap(seg);

	if (dm->cur < 0 || dm->dir[dm->cur].sb != dc->sb)
	{
		int64		i = sb_search(dm, dc->sb);

		if (i == dm->nsb || dm->dir[i].sb != dc->sb)
			elog(ERROR, "sift: SB %u has no docs in segment %llu", dc->sb, ULL(seg->entry.gen));
		load_sb(seg, dm, i);
	}
	return dm;
}

void
sift_seg_page(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *present, const uint16 **dl)
{
	DocMap	   *dm = loaded(seg, dc);

	if (!(dm->pagemask[page / 64] & (1ULL << (page % 64))))
	{
		memset(present, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		*dl = NULL;
		return;
	}
	memcpy(present, dm->om + dm->maskidx[page] * SIFT_OFFMASK_WORDS, SIFT_OFFMASK_WORDS * sizeof(uint64));
	*dl = dm->rows[page];
}

/* ---------- liveness ---------- */

/* Byte b of the LIVE run (logical offset over page payloads) */
static uint8
live_byte(SiftSegment *seg, uint64 b, Buffer *buf)
{
	BlockNumber blk = seg->hdr->live_first + b / SIFT_PAGE_PAYLOAD;
	Page		page;

	if (b / SIFT_PAGE_PAYLOAD >= seg->hdr->live_npages)
		corrupt(seg, "Liveness byte %llu lies past %u LIVE pages.", ULL(b), seg->hdr->live_npages);
	if (!BufferIsValid(*buf) || BufferGetBlockNumber(*buf) != blk)
	{
		if (BufferIsValid(*buf))
			UnlockReleaseBuffer(*buf);
		*buf = ReadBuffer(seg->index, blk);
		LockBuffer(*buf, BUFFER_LOCK_SHARE);
		sift_store_check_page(seg->index, *buf, SIFT_PT_LIVE, seg->entry.gen);
	}
	page = BufferGetPage(*buf);
	if (b % SIFT_PAGE_PAYLOAD >= SiftPageGetUsed(page))
		corrupt(seg, "Liveness byte %llu lies past the written bytes of block %u.", ULL(b), blk);
	return SiftPageGetData(page)[b % SIFT_PAGE_PAYLOAD];
}

/* Copies len bytes of the LIVE run from logical byte b, a page at a time */
static void
live_copy(SiftSegment *seg, uint64 b, uint64 len, uint8 *out)
{
	while (len > 0)
	{
		uint64		off = b % SIFT_PAGE_PAYLOAD,
					n = Min(len, SIFT_PAGE_PAYLOAD - off);
		Buffer		buf = InvalidBuffer;
		Page		page;

		live_byte(seg, b, &buf);
		page = BufferGetPage(buf);
		if (off + n > SiftPageGetUsed(page))
			corrupt(seg, "Liveness bytes %llu..%llu lie past the written bytes of block %u.",
					ULL(b), ULL(b + n), BufferGetBlockNumber(buf));
		memcpy(out, SiftPageGetData(page) + off, n);
		UnlockReleaseBuffer(buf);
		b += n;
		out += n;
		len -= n;
	}
}

static void
load_live_header(SiftSegment *seg)
{
	uint32		nsb = seg->hdr->sb_hi - seg->hdr->sb_lo + 1;
	uint8	   *bits;

	if (seg->live || !(seg->entry.flags & SIFT_SEG_DIRTY))
		return;
	bits = MemoryContextAlloc(seg->mcxt, (nsb + 7) / 8);
	live_copy(seg, sizeof(SiftLiveHeaderData), (nsb + 7) / 8, bits);
	seg->live = bits;
}

/*
 * The liveness bits of the docs of the SB, read once for all its pages: the bit of rank rank_base + i is bit
 * rank_base % 8 + i of the result, followed by zeros for the word loads past a last page. A reader keeps the bits it
 * first read until it asks for another SB, like it keeps the dirty-SB bitmap: a doc vacuumed meanwhile is dead to
 * every snapshot, so the heap check refuses it all the same. A writer that changes the bits drops them (clear_live).
 */
static const uint8 *
live_sb_bits(SiftSegment *seg, DocMap *dm, const SiftDocChunk *dc)
{
	if (!dm->live_ok || dm->live_sb != dc->sb)
	{
		uint64		first = LIVE_START(seg->hdr->sb_hi - seg->hdr->sb_lo + 1) + dc->rank_base / 8,
					len = (dc->rank_base % 8 + dc->ndocs + 7) / 8;
		Size		pad = (PAGE_RANK_WORDS + 1) * sizeof(uint64);

		dm->live_ok = false;
		if (dm->live_cap < len + pad)
		{
			dm->live_cap = Max(len + pad, 1024);
			if (dm->live_bits)
				pfree(dm->live_bits);
			dm->live_bits = MemoryContextAlloc(seg->mcxt, dm->live_cap);
		}
		live_copy(seg, first, len, dm->live_bits);
		memset(dm->live_bits + len, 0, pad);
		dm->live_sb = dc->sb;
		dm->live_ok = true;
	}
	return dm->live_bits;
}

/* The 64 bits of a bit string from bit pos */
static uint64
bits_at(const uint8 *p, uint64 pos)
{
	uint64		v;
	int			s = pos & 7;

	memcpy(&v, p + pos / 8, sizeof(v));
	return s ? v >> s | (uint64) p[pos / 8 + 8] << (64 - s) : v;
}

/* The live offsets of a page: the bits of its docs, taken in rank order, spread over the present offsets */
void
sift_seg_page_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *live)
{
	DocMap	   *dm = loaded(seg, dc);
	const uint16 *dl;
	uint64		rank[PAGE_RANK_WORDS + 1] = {0};
	const uint8 *bits;
	uint64		pos;

	sift_seg_page(seg, dc, page, live, &dl);
	if (!dc->dirty || dl == NULL)
		return;
	bits = live_sb_bits(seg, dm, dc);
	pos = dc->rank_base % 8 + dm->before[page];
	for (int j = 0; j < PAGE_RANK_WORDS; j++)
		rank[j] = bits_at(bits, pos + j * 64);
	sift_kern->expand(live, live, rank, PAGE_RANK_WORDS);
}

bool
sift_seg_lookup(SiftSegment *seg, const char *term, int len, SiftTermInfo *info)
{
	return sift_dict_lookup(seg->dict, term, len, info);
}

/* Does the group hold a posting of a live doc */
static bool
group_has_live(SiftSegment *seg, const sift_grp *g)
{
	SiftDocChunk dc;
	const uint8 *off = g->off;

	if (!sift_seg_docs(seg, g->gid, &dc) || !dc.dirty)
		return true;
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			uint64		mask[SIFT_OFFMASK_WORDS] = {0},
						live[SIFT_OFFMASK_WORDS];

			memcpy(mask, off, g->w);
			sift_seg_page_live(seg, &dc, wd * 64 + pg_rightmost_one_pos64(m), live);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				if (mask[k] & live[k])
					return true;
		}
	return false;
}

bool
sift_seg_term_live(SiftSegment *seg, const SiftTermInfo *info)
{
	MemoryContext cx;
	SiftPostingCursor *c;
	bool		live = false;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift term live", ALLOCSET_SMALL_SIZES);
	c = sift_post_open(&seg->sr, info, cx);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g && !live; g = sift_next(sift_post_cursor(c)))
		live = group_has_live(seg, g);
	sift_sr_release(&seg->sr);
	MemoryContextDelete(cx);
	return live;
}

/* ---------- VACUUM ---------- */

static void
set_dirty(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == *(uint64 *) arg)
			dir->segs[i].flags |= SIFT_SEG_DIRTY;
}

/*
 * Clears the liveness bits of the ranks (ascending) of one SB, sets its
 * dirty bit and counts the dead, in one WAL record: the header page plus the
 * (at most two, an SB has <= 74496 docs) pages holding the bits.
 */
static void
clear_live(SiftSegment *seg, uint32 sb, const uint64 *dead, int nd)
{
	uint32		nsb = seg->hdr->sb_hi - seg->hdr->sb_lo + 1;
	uint64		start = LIVE_START(nsb);
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
		buf[i] = ReadBuffer(seg->index, seg->hdr->live_first + pg[i]);
		LockBuffer(buf[i], BUFFER_LOCK_EXCLUSIVE);
		sift_store_check_page(seg->index, buf[i], SIFT_PT_LIVE, seg->entry.gen);
	}
	state = GenericXLogStart(seg->index);
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
	SiftPageGetData(page[0])[sizeof(SiftLiveHeaderData) + (sb - seg->hdr->sb_lo) / 8] |=
		1 << ((sb - seg->hdr->sb_lo) % 8);
	GenericXLogFinish(state);
	docmap(seg)->live_ok = false;
	for (int i = 0; i < n; i++)
		UnlockReleaseBuffer(buf[i]);
}

/* Deletes the docs of ranks dead[] (ascending) of one SB. The flag first: bits cleared under a clean flag would be ignored by readers */
static void
delete_ranks(SiftSegment *seg, uint32 sb, const uint64 *dead, int nd)
{
	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
	{
		sift_dir_update(seg->index, &(SiftDirChange) {.fn = set_dirty,.arg = &seg->entry.gen,.extra = InvalidBuffer});
		seg->entry.flags |= SIFT_SEG_DIRTY;
	}
	clear_live(seg, sb, dead, nd);
}

int64
sift_seg_live_rank(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int			page = blk & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;
	SiftDocChunk dc;
	uint64		present[SIFT_OFFMASK_WORDS],
				live[SIFT_OFFMASK_WORDS],
				rank;
	const uint16 *dl;

	if (!sift_seg_docs(seg, sift_block_sb(blk), &dc) || !(dc.pagemask[page / 64] >> (page % 64) & 1))
		return -1;
	sift_seg_page(seg, &dc, page, present, &dl);
	sift_seg_page_live(seg, &dc, page, live);
	if (!(live[off / 64] >> (off % 64) & 1))
		return -1;
	rank = dc.rank_base + docmap(seg)->before[page];
	for (int w = 0; w < off / 64; w++)
		rank += pg_popcount64(present[w]);
	return rank + pg_popcount64(present[off / 64] & ((UINT64CONST(1) << (off % 64)) - 1));
}

bool
sift_seg_delete_key(SiftSegment *seg, SiftKey key)
{
	int64		rank = sift_seg_live_rank(seg, key);

	if (rank < 0)
		return false;
	delete_ranks(seg, sift_block_sb(sift_key_block(key)), (uint64 *) &rank, 1);
	return true;
}

/* Asks cb about every live doc of the SB; returns the dead ranks in dead[] */
static int
sb_dead(SiftSegment *seg, const SiftDocChunk *dc, IndexBulkDeleteCallback cb, void *cbstate,
		IndexBulkDeleteResult *stats, uint64 *dead)
{
	DocMap	   *dm = docmap(seg);
	int			nd = 0;

	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS],
					live[SIFT_OFFMASK_WORDS],
					r;
		const uint16 *dl;

		if (!(dc->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, dc, p, present, &dl);
		sift_seg_page_live(seg, dc, p, live);
		r = dc->rank_base + dm->before[p];
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			for (uint64 m = present[wd]; m; m &= m - 1, r++)
			{
				ItemPointerData tid;

				if (!(live[wd] & (m & -m)))
					continue;
				ItemPointerSet(&tid, dc->sb * SIFT_SB_PAGES + p, wd * 64 + __builtin_ctzll(m) + 1);
				if (cb(&tid, cbstate))
					dead[nd++] = r;
				else
					stats->num_index_tuples++;
			}
	}
	return nd;
}

int64
sift_seg_bulkdelete(SiftSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	SiftDocChunk dc;
	uint64	   *dead = palloc(SIFT_SB_PAGES * MAXOFF * sizeof(uint64));
	int64		removed = 0;

	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &dc); sb = dc.sb + 1)
	{
		int			nd = sb_dead(seg, &dc, cb, cbstate, stats, dead);

		if (nd == 0)
			continue;
		delete_ranks(seg, dc.sb, dead, nd);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	pfree(dead);
	stats->tuples_removed += removed;
	return removed;
}

uint64
sift_seg_live_docs(SiftSegment *seg)
{
	SiftLiveHeaderData lh;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return seg->hdr->ndocs;
	live_copy(seg, 0, sizeof(lh), (uint8 *) &lh);
	if (lh.ndead > seg->hdr->ndocs)
		corrupt(seg, "Liveness header counts %u dead of %llu docs.", lh.ndead, ULL(seg->hdr->ndocs));
	return seg->hdr->ndocs - lh.ndead;
}

/* ---------- inspection ---------- */

typedef struct Walk
{
	SiftSegment *seg;
	SiftScoreRef ref;
	bool		verify;
	SiftSegStats *st;
	SiftTermVisit visit;
	void	   *arg;
	SiftKey    *keys;			/* every doc, ascending */
	uint16	   *dl;
	uint64		ndocs;
	/* current term */
	MemoryContext tcxt;
	SiftKey    *tkeys;
	uint8	   *ttf;
	uint16	   *tdl;
	uint32	   *tpoff;			/* positions: offset and length of each posting's record in the stream */
	uint32	   *tplen;
	const uint8 *tprec;			/* the term's records, back to back */
	uint8	   *tpbuf;			/* live_only: the live postings' records */
	Size		tpcap;
	uint64	   *trank;			/* live_only: doc rank of each posting */
	uint32		tcap;
	sift_tid   *tids;			/* SIFT_GRP_MAXTIDS */
	uint8	   *gtf;
	char		prev[SIFT_MAX_TERM_BYTES];
	int			prevlen;
	bool		stopped;
	bool		live_only;		/* dump: drop the postings and docs vacuumed away */
	SiftDocVisit docs;
	Buffer		lbuf;
} Walk;

/* Every doc of the segment with its dl; checks the docmap on the way */
static void
walk_docs(Walk *w)
{
	SiftSegment *seg = w->seg;
	DocMap	   *dm = docmap(seg);
	uint64		sum = 0;

	w->keys = MemoryContextAllocHuge(CurrentMemoryContext, seg->hdr->ndocs * sizeof(SiftKey));
	w->dl = MemoryContextAllocHuge(CurrentMemoryContext, seg->hdr->ndocs * sizeof(uint16));
	for (int64 i = 0; i < dm->nsb; i++)
	{
		load_sb(seg, dm, i);
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			const uint64 *om = dm->om + dm->maskidx[p] * SIFT_OFFMASK_WORDS;
			BlockNumber blk = dm->dir[i].sb * SIFT_SB_PAGES + p;

			for (int o = 0; dm->rows[p] && o < dm->rowlen[p]; o++)
			{
				uint16		v = dm->rows[p][o];

				if (!((om[o / 64] >> (o % 64)) & 1))
				{
					if (w->verify && v != 0)
						corrupt(seg, "Absent slot (%u,%d) has dl %u.", blk, o + 1, v);
					continue;
				}
				w->keys[w->ndocs] = ((SiftKey) blk << 16) | (o + 1);
				w->dl[w->ndocs++] = v;
				sum += v;
			}
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (w->ndocs != seg->hdr->ndocs || sum != seg->hdr->sum_dl)
		corrupt(seg, "DOCMAP holds %llu docs and %llu tokens, the header %llu and %llu.",
				ULL(w->ndocs), ULL(sum), ULL(seg->hdr->ndocs), ULL(seg->hdr->sum_dl));
}

static void
verify_live(Walk *w)
{
	SiftSegment *seg = w->seg;
	uint32		nsb = seg->hdr->sb_hi - seg->hdr->sb_lo + 1;
	uint64		start = LIVE_START(nsb),
				live = 0;
	Buffer		buf = InvalidBuffer;
	SiftLiveHeaderData lh;
	bool		dirty = false;

	for (int i = 0; i < (int) sizeof(lh); i++)
		((uint8 *) &lh)[i] = live_byte(seg, i, &buf);
	if (lh.nsb != nsb || (start + (seg->hdr->ndocs + 7) / 8 + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD != seg->hdr->live_npages)
		corrupt(seg, "Liveness header lists %u SBs in %u pages, expected %u.", lh.nsb, seg->hdr->live_npages, nsb);
	for (uint32 i = 0; i < (nsb + 7) / 8; i++)
		dirty |= live_byte(seg, sizeof(lh) + i, &buf) != 0;
	for (uint64 r = 0; r < seg->hdr->ndocs; r += 8)
	{
		uint8		v = live_byte(seg, start + r / 8, &buf);

		if (seg->hdr->ndocs - r < 8 && (v >> (seg->hdr->ndocs - r)) != 0)
			corrupt(seg, "Liveness bits are set past the last doc.");
		live += sift_pc64(v);
	}
	UnlockReleaseBuffer(buf);
	if (live + lh.ndead != seg->hdr->ndocs || ((seg->entry.flags & SIFT_SEG_DIRTY) == 0 && (dirty || lh.ndead)))
		corrupt(seg, "%llu live and %u dead of %llu docs (dirty flag %d).",
				ULL(live), lh.ndead, ULL(seg->hdr->ndocs), (seg->entry.flags & SIFT_SEG_DIRTY) != 0);
}

static int
df_class(uint32 df)
{
	int			c = SIFT_DF_CLASSES - 1;

	while (df < sift_df_class_lo[c])
		c--;
	return c;
}

/* dl of key, searching forward from *pos (keys of a term ascend) */
static uint16
doc_dl(Walk *w, SiftKey key, uint64 *pos)
{
	uint64		lo = *pos,
				hi = w->ndocs,
				step = 1;

	while (lo + step < hi && w->keys[lo + step] <= key)
		step *= 2;
	hi = Min(hi, lo + step + 1);
	while (lo < hi)
	{
		uint64		mid = (lo + hi) / 2;

		if (w->keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == w->ndocs || w->keys[lo] != key)
		corrupt(w->seg, "Posting (%u,%u) is not a doc of the segment.", sift_key_block(key), (uint32) (key & 0xFFFF));
	*pos = lo;
	return w->dl[lo];
}

/* Liveness bit of a doc rank; the LIVE page stays locked in w->lbuf until released */
static bool
rank_live(Walk *w, uint64 r)
{
	uint64		start = LIVE_START(w->seg->hdr->sb_hi - w->seg->hdr->sb_lo + 1);

	return (live_byte(w->seg, start + r / 8, &w->lbuf) >> (r % 8)) & 1;
}

static void
release_live(Walk *w)
{
	if (BufferIsValid(w->lbuf))
		UnlockReleaseBuffer(w->lbuf);
	w->lbuf = InvalidBuffer;
}

/* Compacts the term's decoded postings to the live ones; returns their number */
static uint32
live_postings(Walk *w, uint32 n)
{
	uint32		m = 0;

	if (!(w->seg->entry.flags & SIFT_SEG_DIRTY))
		return n;
	for (uint32 j = 0; j < n; j++)
		if (rank_live(w, w->trank[j]))
		{
			w->tkeys[m] = w->tkeys[j];
			w->ttf[m] = w->ttf[j];
			w->tpoff[m] = w->tpoff[j];
			w->tplen[m] = w->tplen[j];
			w->tdl[m++] = w->tdl[j];
		}
	release_live(w);
	return m;
}

/*
 * The position records of the term in rank order: offsets and lengths of each, and with verify the
 * counts against tf, the occurrences against dl and the checkpoints
 */
static void
term_positions(Walk *w, SiftPostingCursor *c, const char *term, int len, const SiftTermInfo *info)
{
	SiftSegment *seg = w->seg;
	const uint32 *ck;
	const uint8 *rec,
			   *p,
			   *end;
	uint32		reclen;

	sift_post_pos_stream(c, &ck, &rec, &reclen);
	p = rec;
	end = rec + reclen;
	for (uint32 j = 0; j < info->df; j++)
	{
		uint32		cnt,
					last;
		const uint8 *q = sift_pos_skip(p, end, &cnt, &last);

		if (w->verify)
		{
			uint16		dl = w->tdl[j];

			if ((ck && j % SIFT_POS_EVERY == 0 && ck[j / SIFT_POS_EVERY] != (uint32) (p - rec)) ||
				(w->ttf[j] < SIFT_MAX_TF ? cnt != w->ttf[j] : cnt < SIFT_MAX_TF) || (dl < SIFT_MAX_DL && last >= dl))
				corrupt(seg, "Posting (%u,%u) of \"%.*s\" has %u positions ending at %u for tf %u, dl %u.",
						sift_key_block(w->tkeys[j]), (uint32) (w->tkeys[j] & 0xFFFF), len, term, cnt, last, w->ttf[j], dl);
		}
		w->tpoff[j] = p - rec;
		w->tplen[j] = q - p;
		p = q;
	}
	if (p != end)
		corrupt(seg, "Term \"%.*s\" has %u bytes of positions after its %u postings.", len, term, (uint32) (end - p), info->df);
	w->tprec = rec;
}

/* The records of the live postings (the first n of tpoff / tplen), back to back; tplen is the new lengths */
static const uint8 *
live_records(Walk *w, uint32 n)
{
	Size		total = 0;
	uint8	   *out;

	for (uint32 j = 0; j < n; j++)
		total += w->tplen[j];
	if (total > w->tpcap)
	{
		w->tpcap = Max(total, 2 * w->tpcap);
		w->tpbuf = w->tpbuf ? repalloc_huge(w->tpbuf, w->tpcap) : MemoryContextAllocHuge(w->seg->mcxt, w->tpcap);
	}
	out = w->tpbuf;
	for (uint32 j = 0; j < n; j++)
	{
		memcpy(out, w->tprec + w->tpoff[j], w->tplen[j]);
		out += w->tplen[j];
	}
	return w->tpbuf;
}

static bool
walk_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Walk	   *w = arg;
	SiftSegment *seg = w->seg;
	MemoryContext old = MemoryContextSwitchTo(w->tcxt);
	SiftPostingCursor *c;
	const SiftGroupBounds *b;
	int			nb,
				bi = 0;
	uint32		n = 0;
	uint64		pos = 0;
	SiftDfClassStats *cls = &w->st->cls[df_class(Max(info->df, 1))];

	if (w->verify && w->prevlen >= 0 &&
		(memcmp(w->prev, term, Min(len, w->prevlen)) > 0 ||
		 (memcmp(w->prev, term, Min(len, w->prevlen)) == 0 && w->prevlen >= len)))
		corrupt(seg, "Dictionary term \"%.*s\" follows \"%.*s\".", len, term, w->prevlen, w->prev);
	memcpy(w->prev, term, len);
	w->prevlen = len;
	if (info->df > w->ndocs)
		corrupt(seg, "Term \"%.*s\" has df %u in a segment of %llu docs.", len, term, info->df, ULL(w->ndocs));
	if (info->df > w->tcap)
	{
		w->tcap = Max(info->df, w->tcap * 2);
		w->tkeys = w->tkeys ? repalloc_huge(w->tkeys, w->tcap * sizeof(SiftKey)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(SiftKey));
		w->ttf = w->ttf ? repalloc_huge(w->ttf, w->tcap) : MemoryContextAllocHuge(seg->mcxt, w->tcap);
		w->tdl = w->tdl ? repalloc_huge(w->tdl, w->tcap * sizeof(uint16)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint16));
		if (w->live_only)
			w->trank = w->trank ? repalloc_huge(w->trank, w->tcap * sizeof(uint64)) :
				MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint64));
		w->tpoff = w->tpoff ? repalloc_huge(w->tpoff, w->tcap * sizeof(uint32)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint32));
		w->tplen = w->tplen ? repalloc_huge(w->tplen, w->tcap * sizeof(uint32)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint32));
	}
	c = w->verify ? sift_post_open_verified(&seg->sr, info, w->tcxt) : sift_post_open(&seg->sr, info, w->tcxt);
	if (w->verify)
		sift_post_check(c);
	b = sift_post_bounds(c, &nb);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
	{
		uint32		k = sift_grp_tids(g, NULL, w->tids);

		if (n + k > info->df || (g->n && g->n != k) || g->base != n)
			corrupt(seg, "Term \"%.*s\" decodes past its df %u.", len, term, info->df);
		sift_post_group_tf(c, w->gtf);
		while (bi < nb && b[bi].gid < g->gid)
			bi++;
		if (bi == nb || b[bi].gid != g->gid)
			corrupt(seg, "Term \"%.*s\" has no bound for SB %u.", len, term, g->gid);
		for (uint32 j = 0; j < k; j++)
		{
			uint16		dl = doc_dl(w, w->tids[j], &pos);
			uint8		ub;

			if (n + j > 0 && w->tids[j] <= (j ? w->tids[j - 1] : w->tkeys[n - 1]))
				corrupt(seg, "Postings of \"%.*s\" are out of order.", len, term);
			w->tkeys[n + j] = w->tids[j];
			w->ttf[n + j] = w->gtf[j];
			w->tdl[n + j] = dl;
			if (w->live_only)
				w->trank[n + j] = pos;
			ub = sift_bound_u8(&w->ref, w->gtf[j], dl);
			if (w->verify && (w->gtf[j] == 0 || ub > info->max_bound || ub > b[bi].bound ||
				ub > b[bi].blk_bound[(SIFT_BLK(w->tids[j]) & (SIFT_SB_PAGES - 1)) >> SIFT_BLK_SHIFT]))
				corrupt(seg, "Posting (%u,%u) of \"%.*s\" (tf %u, dl %u, bound %u) exceeds its bounds %u/%u/%u.",
						SIFT_BLK(w->tids[j]), SIFT_OFF(w->tids[j]), len, term, w->gtf[j], dl, ub,
						info->max_bound, b[bi].bound,
						b[bi].blk_bound[(SIFT_BLK(w->tids[j]) & (SIFT_SB_PAGES - 1)) >> SIFT_BLK_SHIFT]);
		}
		n += k;
	}
	if (n != info->df)
		corrupt(seg, "Term \"%.*s\" has %u postings, df %u.", len, term, n, info->df);
	w->tprec = NULL;
	if ((info->flags & SIFT_TERM_POS) && (w->verify || w->visit))
		term_positions(w, c, term, len, info);
	cls->terms++;
	cls->postings += n;
	if (info->flags & SIFT_TERM_INLINE)
		cls->inline_terms++;
	else
	{
		cls->list_terms += (info->flags & SIFT_TERM_SPARSE) != 0;
		cls->grouped_terms += (info->flags & SIFT_TERM_SPARSE) == 0;
		cls->codec_bytes += info->codec_len;
		cls->sidecar_bytes += info->post_len - info->codec_len - ((info->flags & SIFT_TERM_POS) ? info->pos_len : 0);
	}
	w->st->nterms++;
	w->st->postings += n;
	MemoryContextSwitchTo(old);
	if (w->live_only)
		n = live_postings(w, n);
	if (n > 0 || !w->live_only)
	{
		SiftTermInfo live = *info;
		const uint8 *pos = w->tprec;

		live.df = n;
		if (pos && n < info->df)
			pos = live_records(w, n);
		w->stopped = w->visit && !w->visit(w->arg, term, len, w->live_only ? &live : info, w->tkeys, w->ttf, w->tdl,
										   pos, w->tplen);
	}
	MemoryContextReset(w->tcxt);
	return !w->stopped;
}

void
sift_seg_walk(SiftSegment *seg, const SiftScoreRef *ref, bool verify, SiftSegStats *st,
			  SiftTermVisit visit, void *arg)
{
	Walk		w = {0};
	const SiftSectionRef *s = seg->hdr->sections;

	memset(st, 0, sizeof(SiftSegStats));
	w.seg = seg;
	w.ref = *ref;
	w.ref.avgdl = seg->hdr->avgdl_ref;
	w.verify = verify;
	w.st = st;
	w.visit = visit;
	w.arg = arg;
	w.prevlen = -1;
	w.tcxt = AllocSetContextCreate(CurrentMemoryContext, "sift walk term", ALLOCSET_DEFAULT_SIZES);
	w.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	w.gtf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	st->dict_bytes = s[SIFT_SEC_DICT].len + s[SIFT_SEC_DICTIDX].len + seg->hdr->dictroot_n * sizeof(uint64);
	walk_docs(&w);
	if (verify)
		verify_live(&w);
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, walk_term, &w);
	if (!w.stopped && st->nterms != seg->hdr->nterms)
		corrupt(seg, "Dictionary holds %llu terms, the header %llu.", ULL(st->nterms), ULL(seg->hdr->nterms));
	MemoryContextDelete(w.tcxt);
}

void
sift_seg_dump(SiftSegment *seg, const SiftScoreRef *ref, SiftDocVisit docs, SiftTermVisit terms, void *arg)
{
	Walk		w = {0};
	SiftSegStats st;
	SiftKey		bk[1024];
	uint16		bdl[1024];
	int			n = 0;

	w.seg = seg;
	w.ref = *ref;
	w.ref.avgdl = seg->hdr->avgdl_ref;
	w.st = &st;
	w.visit = terms;
	w.arg = arg;
	w.prevlen = -1;
	w.live_only = true;
	w.lbuf = InvalidBuffer;
	w.tcxt = AllocSetContextCreate(CurrentMemoryContext, "sift dump term", ALLOCSET_DEFAULT_SIZES);
	w.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	w.gtf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	memset(&st, 0, sizeof(st));
	walk_docs(&w);
	for (uint64 r = 0; r < w.ndocs; r++)
	{
		if (!rank_live(&w, r))
			continue;
		bk[n] = w.keys[r];
		bdl[n++] = w.dl[r];
		if (n == lengthof(bk))
		{
			release_live(&w);
			docs(arg, bk, bdl, n);
			n = 0;
		}
	}
	release_live(&w);
	if (n > 0)
		docs(arg, bk, bdl, n);
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, walk_term, &w);
	MemoryContextDelete(w.tcxt);
}
