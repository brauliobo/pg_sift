/*
 * bm25/segment.c - immutable segment reader: open, docmap, doc pages (layout in
 * bm25/segment.h and core/page.h).
 */
#include "core/host.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif
#include "bm25/kernels.h"
#include "core/live.h"
#include "core/popcount.h"
#include "bm25/segment.h"

#include "bm25/segment_int.h"
#include "core/cmp.h"

/* ---------- reader ---------- */

SiftSegment *
sift_seg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftSegment *seg = palloc0(sizeof(SiftSegment));
	SiftSegHeaderData *h;

	seg->index = index;
	seg->entry = *entry;
	seg->mcxt = mcxt;
	seg->fmt = Max(entry->format, SIFT_SEGFMT_RANKBITS);
	if (entry->kind != SIFT_SEG_IMMUTABLE)
		elog(ERROR, "sift: segment %llu is not immutable", ULL(entry->gen));
	if (entry->format > SIFT_FORMAT_MINOR)
		elog(ERROR, "sift: segment %llu has format %u, this build reads up to %u", ULL(entry->gen), entry->format,
			 SIFT_FORMAT_MINOR);
	seg->hdr = sift_seg_read_header(index, entry);
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
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS && (entry->flags & SIFT_SEG_DIRTY))
		sift_dict_track_slots(seg->dict);
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

/* Reads and checks the SB directory (24 bytes per SB, sums the ranks) */
static void
docmap_read(SiftSegment *seg, DocMap *dm, void *handle)
{
	const SiftSectionRef *s = &seg->hdr->sections[SIFT_SEC_DOCMAP];
	uint64		rank = 0;

	seg_read(seg, dm->dir_off, dm->nsb * sizeof(SiftDocSBRef), dm->dir);
#ifdef SIFT_TESTING
	sift_docdir_stats.bytes_read += dm->nsb * sizeof(SiftDocSBRef);
#endif
	for (uint32 i = 0; i < dm->nsb; i++)
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
	dm->rank_base[dm->nsb] = rank;
	if (rank != seg->hdr->ndocs)
		corrupt(seg, "DOCMAP lists %llu docs, the header %llu.", ULL(rank), ULL(seg->hdr->ndocs));
	sift_docdir_ready(handle);
}

DocMap *
docmap(SiftSegment *seg)
{
	DocMap	   *dm = seg->docmap;
	const SiftSectionRef *s = &seg->hdr->sections[SIFT_SEC_DOCMAP];
	void	   *handle;
	uint32		nsb;

	if (dm)
		return dm;
	dm = MemoryContextAllocZero(seg->mcxt, sizeof(DocMap));
	dm->mcxt = seg->mcxt;
	seg_read(seg, s->off + s->len - sizeof(uint32), sizeof(uint32), &nsb);
	if ((uint64) nsb * sizeof(SiftDocSBRef) + sizeof(uint32) > s->len || nsb == 0 ||
		nsb > seg->hdr->sb_hi - seg->hdr->sb_lo + 1)
		corrupt(seg, "DOCMAP of %llu bytes lists %u SBs.", ULL(s->len), nsb);
	dm->nsb = nsb;
	dm->dir_off = s->off + s->len - sizeof(uint32) - nsb * sizeof(SiftDocSBRef);
	if (!sift_docdir_get(seg, nsb, &dm->dir, &dm->rank_base, &handle))
		docmap_read(seg, dm, handle);
	dm->cur = -1;
	dm->om = MemoryContextAllocAligned(seg->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
									   SIFT_SIMD_ALIGN, 0);
	seg->docmap = dm;
	return dm;
}

/*
 * The page index of the loaded SB: the docs before each present page, its row of offsets in om, its dl row and the
 * words it needs. Only what reads a page's rows or ranks needs it, not the counts of whole groups.
 */
void
index_sb(SiftSegment *seg, DocMap *dm)
{
	const uint8 *row = dm->row0;
	uint32		sb = dm->dir[dm->cur].sb,
				k = 0,
				ndocs = 0,
				nslots = 0;

	if (dm->indexed)
		return;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		const uint64 *om = dm->om + k * SIFT_OFFMASK_WORDS;
		int			maxoff = 0,
					wd;

		dm->rows[p] = NULL;
		if (!(dm->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		for (wd = SIFT_OFFMASK_WORDS - 1; wd >= 0 && om[wd] == 0; wd--)
			;
		if (wd >= 0)
			maxoff = wd * 64 + 64 - __builtin_clzll(om[wd]);
		if (maxoff == 0 || maxoff > MAXOFF)
			corrupt(seg, "DOCMAP record of SB %u has an empty or oversized page %d.", sb, p);
		dm->before[p] = ndocs;
		dm->maskidx[p] = k++;
		dm->rows[p] = row;
		dm->rowlen[p] = maxoff;
		dm->xoff[p] = nslots;
		nslots += (maxoff + 15) & ~15;
		row += SIFT_DL_ROW_BYTES(dm->dlcode, maxoff);
		for (int j = 0; j <= wd; j++)
			ndocs += sift_pc64(om[j]);
	}
	if (row > dm->rec + dm->reclen || ALIGN4(row - dm->rec) != dm->reclen)
		corrupt(seg, "DOCMAP record of SB %u spans %llu bytes, its rows %llu.", sb, ULL(dm->reclen), ULL(row - dm->rec));
	if (dm->dlcode != 0 && nslots > dm->xcap)
	{
		if (dm->xdl)
			pfree(dm->xdl);
		dm->xcap = Max(nslots, 4096);
		dm->xdl = MemoryContextAlloc(dm->mcxt, dm->xcap * sizeof(uint16));
	}
	memset(dm->xdone, 0, sizeof(dm->xdone));
	dm->indexed = true;
}

void
load_sb(SiftSegment *seg, DocMap *dm, int64 i)
{
	const SiftDocSBRef *r = &dm->dir[i];
	uint64		len = (i + 1 < dm->nsb ? dm->dir[i + 1].off : dm->dir_off) - r->off;
	uint64		all[SIFT_PAGEMASK_WORDS] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
	uint32	   *h;
	const char *bad;

	if (dm->cur == i)
		return;
	dm->cur = -1;
	dm->indexed = false;
	if (len > dm->reccap)
	{
		dm->reccap = Max(len, 4096);
		if (dm->rec)
			pfree(dm->rec);
		dm->rec = MemoryContextAllocAligned(seg->mcxt, dm->reccap + 16, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	}
	seg_read(seg, r->off, len, dm->rec);
	h = (uint32 *) dm->rec;
	dm->dlcode = h[0] >> SIFT_DL_CODE_SHIFT;
	h[0] &= (1U << SIFT_DL_CODE_SHIFT) - 1;
	if (h[0] != r->ndocs || 8 + (uint64) h[1] > len || h[1] < 4 || sift_chunk_count(dm->rec + 8) != h[0])
		corrupt(seg, "DOCMAP record of SB %u has %u docs and a chunk of %u bytes.", r->sb, h[0], h[1]);
	if (dm->dlcode > 2)
		corrupt(seg, "DOCMAP record of SB %u has dl rows of width code %d.", r->sb, dm->dlcode);
	if ((bad = sift_chunk_check(dm->rec + 8, h[1])) != NULL)
		corrupt(seg, "DOCMAP record of SB %u has a page chunk of %u bytes that is %s.", r->sb, h[1], bad);
	sift_chunk_pagemask(dm->rec + 8, dm->pagemask);
	dm->np = 0;
	for (int j = 0; j < SIFT_PAGEMASK_WORDS; j++)
		dm->np += sift_pc64(dm->pagemask[j]);
	if (sift_chunk_offsets(dm->rec + 8, all, dm->om) != dm->np)
		corrupt(seg, "DOCMAP record of SB %u has offsets for another number of pages than the %d it lists.", r->sb, dm->np);
	dm->nwmax = (sift_chunk_width(dm->rec + 8) + 7) / 8;
	dm->row0 = dm->rec + ALIGN4(8 + h[1]);
	dm->reclen = len;
	dm->cur = i;
}

/* Is the SB's bit of the dirty-SB bitmap set */
static bool
sb_bit_dirty(const SiftSegment *seg, uint32 sb)
{
	return seg->live && (((uint8 *) seg->live)[(sb - seg->hdr->sb_lo) / 8] >> ((sb - seg->hdr->sb_lo) % 8)) & 1;
}

static void
fill_chunk(SiftSegment *seg, DocMap *dm, int64 i, SiftDocChunk *dc)
{
	load_sb(seg, dm, i);
	dc->sb = dm->dir[i].sb;
	dc->ndocs = dm->dir[i].ndocs;
	memcpy(dc->pagemask, dm->pagemask, sizeof(dc->pagemask));
	dc->rank_base = dm->rank_base[i];
	dc->dirty = sb_bit_dirty(seg, dc->sb);
}

/* Index of the first SB entry >= sb */
int64
sb_search(DocMap *dm, uint32 sb)
{
	int64		lo = 0,
				hi = dm->nsb;

	SIFT_BISECT(int64, mid, lo, hi, dm->dir[mid].sb < sb);
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

bool
sift_seg_sb_dirty(SiftSegment *seg, uint32 sb)
{
	load_live_header(seg);
	return sb >= seg->hdr->sb_lo && sb <= seg->hdr->sb_hi && sb_bit_dirty(seg, sb);
}

static void
load_live_header(SiftSegment *seg)
{
	SiftLive	l = seg_live(seg);

	if (seg->live || !(seg->entry.flags & SIFT_SEG_DIRTY))
		return;
	seg->live = MemoryContextAlloc(seg->mcxt, (l.nsb + 7) / 8);
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		sift_live2_dirty(&l, &live2(seg)->h, seg->live);
	else
		sift_live_dirty(&l, seg->live);
}

DocMap *
loaded(SiftSegment *seg, const SiftDocChunk *dc)
{
	DocMap	   *dm = seg->docmap ? seg->docmap : docmap(seg);

	if (dm->cur < 0 || dm->dir[dm->cur].sb != dc->sb)
	{
		int64		i = sb_search(dm, dc->sb);

		if (i == dm->nsb || dm->dir[i].sb != dc->sb)
			elog(ERROR, "sift: SB %u has no docs in segment %llu", dc->sb, ULL(seg->entry.gen));
		load_sb(seg, dm, i);
	}
	return dm;
}

DocMap *
indexed(SiftSegment *seg, const SiftDocChunk *dc)
{
	DocMap	   *dm = loaded(seg, dc);

	index_sb(seg, dm);
	return dm;
}

/*
 * A row of n slots of 8 or 4 bits (low nibble first) as 16 bits per slot, 16 slots at a time: it reads up to 15 bytes and writes
 * up to 15 slots past the row (the record is allocated with that slack, and the rows of dm->xdl are 16 slots apart).
 */
static void
expand_dl(uint16 *dst, const uint8 *src, int n, int bits)
{
#ifdef __SSE2__
	const __m128i zero = _mm_setzero_si128(),
				nib = _mm_set1_epi8(15);

	for (int i = 0; i < n; i += 16)
	{
		__m128i		v;

		if (bits == 8)
			v = _mm_loadu_si128((const __m128i *) (src + i));
		else
		{
			__m128i		b = _mm_loadl_epi64((const __m128i *) (src + i / 2));

			v = _mm_unpacklo_epi8(_mm_and_si128(b, nib), _mm_and_si128(_mm_srli_epi16(b, 4), nib));
		}
		_mm_storeu_si128((__m128i *) (dst + i), _mm_unpacklo_epi8(v, zero));
		_mm_storeu_si128((__m128i *) (dst + i + 8), _mm_unpackhi_epi8(v, zero));
	}
#else
	for (int i = 0; i < n; i++)
		dst[i] = bits == 8 ? src[i] : src[i / 2] >> (i % 2 * 4) & 15;
#endif
}

/* Expands the dl row of a present page of the indexed SB (narrow records only) */
pg_noinline void
expand_row(DocMap *dm, int page)
{
	expand_dl(dm->xdl + dm->xoff[page], dm->rows[page], dm->rowlen[page], SIFT_DL_CODE_BITS(dm->dlcode));
	dm->xdone[page / 64] |= 1ULL << (page % 64);
}

bool
sift_seg_page(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *present, const uint16 **dl)
{
	DocMap	   *dm = indexed(seg, dc);

	if (!(dm->pagemask[page / 64] & (1ULL << (page % 64))))
	{
		if (present)
			memset(present, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		if (dl)
			*dl = NULL;
		return false;
	}
	if (present)
		memcpy(present, dm->om + dm->maskidx[page] * SIFT_OFFMASK_WORDS, SIFT_OFFMASK_WORDS * sizeof(uint64));
	if (dl)
		*dl = dl_row(dm, page);
	return true;
}

/* The dl rows of the pages of a block (SIFT_BLK_PAGES pages) that are set in want and have docs, NULL for the others */
void
sift_seg_block_dl(SiftSegment *seg, const SiftDocChunk *dc, int block, uint32 want, const uint16 **rows)
{
	DocMap	   *dm = indexed(seg, dc);

	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
	{
		int			page = block * SIFT_BLK_PAGES + pg;

		rows[pg] = (want >> pg & 1) && (dm->pagemask[page / 64] & (1ULL << (page % 64))) ? dl_row(dm, page) : NULL;
	}
}
