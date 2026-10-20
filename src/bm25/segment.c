/*
 * bm25/segment.c - immutable segment writer and reader (layout in
 * bm25/segment.h and sift_page.h).
 */
#include "sift_host.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif
#include "bm25/kernels.h"
#include "sift_live.h"
#include "sift_popcount.h"
#include "bm25/segment.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
#define ULL(x)			((unsigned long long) (x))
#define MAXOFF			SIFT_MAX_OFFSET
#define PAGE_RANK_WORDS	((MAXOFF + 63) / 64)	/* words of rank bits of a page of docs */

#define corrupt(seg, ...) SIFT_SEG_CORRUPT((seg)->index, (seg)->entry.gen, __VA_ARGS__)

/* The liveness pages of a segment, as the shared module addresses them */
static SiftLive
seg_live(const SiftSegment *seg)
{
	return (SiftLive) {seg->index, seg->entry.gen, seg->hdr->live_first, seg->hdr->live_npages,
		seg->hdr->sb_hi - seg->hdr->sb_lo + 1, seg->hdr->ndocs};
}

#ifdef SIFT_TESTING
int			sift_test_segfmt = 0;
#endif

/*
 * The dl rows of a DOCMAP record are 16 bits per slot, or in format 3 8 or 4 bits where the largest dl of the SB allows. The width is
 * kept in bits 30-31 of the record's document count (SIFT_DL_CODE_SHIFT), so a record of format 2 and one of format 3 read alike.
 */
#define SIFT_DL_CODE_SHIFT	30
#define SIFT_DL_CODE_BITS(code)	(16 >> (code))
#define SIFT_DL_ROW_BYTES(code, n)	(((n) * SIFT_DL_CODE_BITS(code) + 7) / 8)

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
	uint64		spool_min;		/* postings from which a term is staged in files (sift_segw_spool), 0 = never */
	/* SB being accumulated */
	uint32		sb;
	uint32		sb_ndocs;
	uint16		sb_maxdl;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint16		maxoff[SIFT_SB_PAGES];
	uint64	   *offmask;		/* [page][SIFT_OFFMASK_WORDS] */
	uint16	   *dl;				/* [page][MAXOFF] */
	SiftDocSBRef *sbdir;
	uint32		nsb;
	uint32		maxsb;
	/* format 2: the live rows of every SB, spooled, and where each region starts (by SB from sb_lo) */
	int			fmt;
	BufFile    *liverows;
	uint64	   *reg;
	uint32		nreg;
	uint32		maxreg;
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
	uint64		reclen;			/* bytes of the loaded SB's record */
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	int			np;				/* present pages */
	int			nwmax;			/* words of offsets of the fullest page */
	uint64	   *om;				/* canonical offset masks of present pages */
	const uint8 *row0;			/* the dl rows of the pages, after the chunk */
	int			dlcode;			/* width of the dl rows (SIFT_DL_CODE_BITS) */
	bool		indexed;		/* before, maskidx, rows and rowlen are set */
	uint32		before[SIFT_SB_PAGES];	/* docs on earlier pages (present pages) */
	uint8		maskidx[SIFT_SB_PAGES]; /* present-page index (present pages) */
	const uint8 *rows[SIFT_SB_PAGES];	/* the stored dl row of each present page, NULL for the others */
	uint16		rowlen[SIFT_SB_PAGES];	/* slots of the row: its page's highest present offset */
	MemoryContext mcxt;
	uint16	   *xdl;			/* narrow rows expanded to 16 bits, one after the other, for the pages in xdone */
	Size		xcap;
	uint32		xoff[SIFT_SB_PAGES];	/* where the row of the page starts in xdl */
	uint64		xdone[SIFT_PAGEMASK_WORDS];
	bool		live_ok;		/* live_bits holds the bits of SB live_sb */
	uint32		live_sb;
	uint8	   *live_bits;		/* the SB's docs, bit i for rank rank_base + i, from bit rank_base % 8 */
	Size		live_cap;
	bool		live_om_ok;		/* live_om holds the live offsets of the loaded SB's pages */
	uint64	   *live_om;		/* nwmax words per present page, in the order of om */
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
	w->fmt = SIFT_WRITE_SEGFMT;
#ifdef SIFT_TESTING
	if (sift_test_segfmt > 0)
		w->fmt = Min(sift_test_segfmt, SIFT_WRITE_SEGFMT);
#endif
	w->mcxt = mcxt;
	w->ref = *ref;
	w->positions = positions;
	w->sw = sift_sw_begin(wt, gen);
	w->offmask = palloc0(SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64));
	w->dl = palloc0(SIFT_SB_PAGES * MAXOFF * sizeof(uint16));
	w->maxsb = 64;
	w->sbdir = palloc(w->maxsb * sizeof(SiftDocSBRef));
	if (w->fmt >= SIFT_SEGFMT_LIVEROWS)
	{
		w->liverows = BufFileCreateTemp(false);
		w->maxreg = 64;
		w->reg = palloc(w->maxreg * sizeof(uint64));
	}
	w->termlen = -1;
	MemoryContextSwitchTo(old);
	return w;
}

void
sift_segw_spool(SiftSegmentWriter *w, uint64 min_postings)
{
	w->spool_min = min_postings;
}

/*
 * The region of the SB being flushed in the LIVE rows: the live offsets of its present pages, all live for now, as wide as the
 * widest page needs (np rows of om)
 */
static void
write_region(SiftSegmentWriter *w, const uint64 *om, int np)
{
	SiftLive2RegionData r = {.np = np};
	uint32		idx = w->sb - w->hdr.sb_lo;
	int			nw = 1,
				k = 0;

	for (int p = 0; p < SIFT_SB_PAGES; p++)
		nw = Max(nw, (w->maxoff[p] + 63) / 64);
	r.w = nw * sizeof(uint64);
	memcpy(r.pages, w->pagemask, sizeof(r.pages));
	while (w->nreg <= idx + 1)
	{
		if (w->nreg == w->maxreg)
		{
			w->maxreg *= 2;
			w->reg = repalloc_huge(w->reg, w->maxreg * sizeof(uint64));
		}
		w->reg[w->nreg] = w->nreg == 0 ? 0 : w->reg[w->nreg - 1];
		w->nreg++;
	}
	/* entries up to idx are the end of the region before; idx + 1 is the end of this one */
	w->reg[idx + 1] = w->reg[idx] + sizeof(r) + (uint64) np * r.w;
	BufFileWrite(w->liverows, &r, sizeof(r));
	for (; k < np; k++)
		BufFileWrite(w->liverows, om + SIFT_OFFMASK_WORDS * k, r.w);
}

static int
dl_code(const SiftSegmentWriter *w)
{
	if (w->fmt < SIFT_SEGFMT_PACKED || w->sb_maxdl > 255)
		return 0;
	return w->sb_maxdl > 15 ? 1 : 2;
}

/* The row of n slots in the width of code, little endian, a byte at least */
static void
write_dl_row(SiftStreamWriter *sw, const uint16 *dl, int n, int code)
{
	uint8		buf[SIFT_MAX_OFFSET];
	int			bits = SIFT_DL_CODE_BITS(code);
	int			len = SIFT_DL_ROW_BYTES(code, n);

	if (code == 0)
	{
		sift_sw_write(sw, dl, n * sizeof(uint16));
		return;
	}
	memset(buf, 0, len);
	for (int i = 0; i < n; i++)
		buf[i * bits / 8] |= dl[i] << (i * bits % 8);
	sift_sw_write(sw, buf, len);
}

static void
flush_sb(SiftSegmentWriter *w)
{
	uint64	   *om = palloc(SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64));
	uint8	   *chunk;
	uint32		hdr[2];
	int			np = 0,
				code = dl_code(w);

	if (w->sb_ndocs == 0)
		return;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
		if (w->pagemask[p / 64] & (1ULL << (p % 64)))
			memcpy(om + SIFT_OFFMASK_WORDS * np++, w->offmask + SIFT_OFFMASK_WORDS * p,
				   SIFT_OFFMASK_WORDS * sizeof(uint64));
	if (w->liverows)
		write_region(w, om, np);
	hdr[0] = w->sb_ndocs | (uint32) code << SIFT_DL_CODE_SHIFT;
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
		write_dl_row(w->sw, w->dl + p * MAXOFF, w->maxoff[p], code);
		memset(w->dl + p * MAXOFF, 0, w->maxoff[p] * sizeof(uint16));
		memset(w->offmask + p * SIFT_OFFMASK_WORDS, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		w->maxoff[p] = 0;
	}
	memset(w->pagemask, 0, sizeof(w->pagemask));
	w->sb_ndocs = 0;
	w->sb_maxdl = 0;
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
		w->sb_maxdl = Max(w->sb_maxdl, dl[i]);
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
	if (w->spool_min)
		sift_postw_spool(w->pw, w->spool_min);
	if (w->fmt >= SIFT_SEGFMT_PACKED)
		sift_postw_pack(w->pw);
	w->dw = sift_dict_writer_begin(w->sw, w->fmt >= SIFT_SEGFMT_PACKED);
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
	if (w->liverows)
	{
		sift_live2_write(w->wt, w->gen, w->hdr.sb_hi - w->hdr.sb_lo + 1, sift_dict_writer_nslots(w->dw), w->reg, w->liverows,
						 &w->hdr.live_first, &w->hdr.live_npages);
		BufFileClose(w->liverows);
		pfree(w->reg);
		w->liverows = NULL;
	}
	else
		sift_live_write(w->wt, w->gen, w->hdr.sb_hi - w->hdr.sb_lo + 1, w->ndocs, &w->hdr.live_first, &w->hdr.live_npages);
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
	entry->format = w->fmt;
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

static DocMap *
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
static void
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

static void
load_sb(SiftSegment *seg, DocMap *dm, int64 i)
{
	const SiftDocSBRef *r = &dm->dir[i];
	uint64		len = (i + 1 < dm->nsb ? dm->dir[i + 1].off : dm->dir_off) - r->off;
	uint64		all[SIFT_PAGEMASK_WORDS] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
	uint32	   *h;

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

bool
sift_seg_sb_dirty(SiftSegment *seg, uint32 sb)
{
	load_live_header(seg);
	return sb >= seg->hdr->sb_lo && sb <= seg->hdr->sb_hi && sb_bit_dirty(seg, sb);
}

static DocMap *
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

static DocMap *
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
static pg_noinline void
expand_row(DocMap *dm, int page)
{
	expand_dl(dm->xdl + dm->xoff[page], dm->rows[page], dm->rowlen[page], SIFT_DL_CODE_BITS(dm->dlcode));
	dm->xdone[page / 64] |= 1ULL << (page % 64);
}

/* The dl row of a present page of the indexed SB as 16 bits per slot: narrow rows are expanded on the first use */
static inline const uint16 *
dl_row(DocMap *dm, int page)
{
	if (dm->dlcode == 0)
		return (const uint16 *) dm->rows[page];
	if (!(dm->xdone[page / 64] >> (page % 64) & 1))
		expand_row(dm, page);
	return dm->xdl + dm->xoff[page];
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

/* ---------- liveness, format 2 ---------- */

/* The header of the LIVE run, and the region of one SB (its live offsets as a group of bitmaps) read last */
typedef struct Live2
{
	SiftLive2HeaderData h;
	bool		rok;			/* grp holds the region of SB rsb */
	uint32		rsb;
	uint64		rstart;			/* byte of the run where that region starts */
	Size		rcap;
	uint8	   *rbuf;			/* the region from byte 24, so that its rows start at 64 */
	sift_grp	grp;
} Live2;

static Live2 *
live2(SiftSegment *seg)
{
	Live2	   *l2 = seg->l2;
	SiftLive	l;

	if (l2)
		return l2;
	l = seg_live(seg);
	l2 = MemoryContextAllocZero(seg->mcxt, sizeof(Live2));
	sift_live2_header(&l, &l2->h);
	seg->l2 = l2;
	return l2;
}

/* The row of the page in a group of bitmaps, NULL when the page is not in it */
static const uint8 *
grp_row(const sift_grp *g, int page)
{
	int			wd = page / 64,
				idx = 0;

	if (!(g->pages[wd] >> (page % 64) & 1))
		return NULL;
	for (int j = 0; j < wd; j++)
		idx += sift_pc64(g->pages[j]);
	idx += sift_pc64(g->pages[wd] & ((UINT64CONST(1) << (page % 64)) - 1));
	return g->off + (Size) idx * g->w;
}

/*
 * The live offsets of the SB's pages, NULL for an SB without docs. They are kept until the reader asks for another SB, like the
 * dirty-SB bitmap: a doc vacuumed meanwhile is dead to every snapshot, so the heap check refuses it all the same. The group is
 * valid until the next call on seg.
 */
static const sift_grp *
live2_rows(SiftSegment *seg, uint32 sb)
{
	Live2	   *l2 = live2(seg);

	if (!l2->rok || l2->rsb != sb)
	{
		SiftLive	l = seg_live(seg);
		SiftLive2RegionData *r;
		uint64		len;

		l2->rok = false;
		if (sb < seg->hdr->sb_lo || sb > seg->hdr->sb_hi)
			return NULL;
		l2->rstart = sift_live2_region(&l, &l2->h, sb - seg->hdr->sb_lo, &len);
		if (len == 0)
			return NULL;
		if (len < sizeof(*r) || len > sizeof(*r) + SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64))
			corrupt(seg, "Live rows of SB %u span %llu bytes.", sb, ULL(len));
		if (l2->rcap < len + 24)
		{
			l2->rcap = Max(len + 24, 4096);
			if (l2->rbuf)
				pfree(l2->rbuf);
			l2->rbuf = MemoryContextAllocAligned(seg->mcxt, l2->rcap, SIFT_SIMD_ALIGN, 0);
		}
		sift_live_copy(&l, l2->rstart, len, l2->rbuf + 24);
		r = (SiftLive2RegionData *) (l2->rbuf + 24);
		if (r->w == 0 || r->w % 8 != 0 || r->w > SIFT_OFFMASK_WORDS * 8 || r->np == 0 || r->np > SIFT_SB_PAGES ||
			len != sizeof(*r) + (uint64) r->np * r->w ||
			r->np != (uint32) (sift_pc64(r->pages[0]) + sift_pc64(r->pages[1]) + sift_pc64(r->pages[2]) + sift_pc64(r->pages[3])))
			corrupt(seg, "Live rows of SB %u: %u rows of %u bytes in %llu bytes.", sb, r->np, r->w, ULL(len));
		l2->grp = (sift_grp) {.gid = sb,.w = r->w,.off = l2->rbuf + 24 + sizeof(*r)};
		memcpy(l2->grp.pages, r->pages, sizeof(r->pages));
		l2->rsb = sb;
		l2->rok = true;
	}
	return &l2->grp;
}

static int
live2_mask(SiftSegment *seg, uint32 sb, int page, uint64 *mask)
{
	const sift_grp *g = live2_rows(seg, sb);
	const uint64 *row = g ? (const uint64 *) grp_row(g, page) : NULL;
	int			n = 0;

	for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		n += sift_pc64(mask[k] &= row && k < (int) (g->w / 8) ? row[k] : 0);
	return n;
}

static uint64
live2_count(SiftSegment *seg, const sift_grp *g, const uint64 *pages)
{
	const sift_grp *rows = live2_rows(seg, g->gid);
	const uint8 *off = g->off;
	uint64		n = 0;

	if (rows == NULL)
		return 0;
	if (memcmp(pages, g->pages, sizeof(g->pages)) == 0)
		return sift_grp_and_count(g, rows);
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			bit = pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};

			if (pages[wd] >> bit & 1)
			{
				memcpy(mask, off, g->w);
				n += live2_mask(seg, g->gid, wd * 64 + bit, mask);
			}
		}
	return n;
}

static void
live2_cleared(SiftSegment *seg, int nd, uint64 dead_dl)
{
	Live2	   *l2 = live2(seg);

	l2->rok = false;
	l2->h.ndead += nd;
	l2->h.dead_dl += dead_dl;
}

/* ---------- liveness ---------- */

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

/*
 * The liveness bits of the docs of the SB, read once for all its pages: the bit of rank rank_base + i is bit
 * rank_base % 8 + i of the result (a word array), followed by zeros for the word loads past a last page. A reader
 * keeps the bits it first read until it asks for another SB, like it keeps the dirty-SB bitmap: a doc vacuumed meanwhile
 * is dead to every snapshot, so the heap check refuses it all the same. A writer that changes the bits drops them
 * (delete_ranks), and the rows made from them (live_om).
 */
static const uint8 *
live_sb_bits(SiftSegment *seg, DocMap *dm, const SiftDocChunk *dc)
{
	if (!dm->live_ok || dm->live_sb != dc->sb)
	{
		SiftLive	l = seg_live(seg);
		uint64		first = SIFT_LIVE_START(l.nsb) + dc->rank_base / 8,
					len = (dc->rank_base % 8 + dc->ndocs + 7) / 8;
		Size		pad = (PAGE_RANK_WORDS + 1) * sizeof(uint64);

		dm->live_ok = dm->live_om_ok = false;
		if (dm->live_cap < len + pad)
		{
			dm->live_cap = Max(len + pad, 1024);
			if (dm->live_bits)
				pfree(dm->live_bits);
			dm->live_bits = MemoryContextAlloc(seg->mcxt, dm->live_cap);
		}
		sift_live_copy(&l, first, len, dm->live_bits);
		memset(dm->live_bits + len, 0, pad);
		dm->live_sb = dc->sb;
		dm->live_ok = true;
	}
	return dm->live_bits;
}

/*
 * Keeps in mask, a subset of the present offsets of a page of a dirty SB, the live ones: the bits of its docs, taken in
 * rank order, spread over the present offsets. The words past the highest present offset are empty, so they are not
 * expanded. Returns the offsets left.
 */
int
sift_seg_page_mask_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *mask)
{
	DocMap	   *dm;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_mask(seg, dc->sb, page, mask);
	dm = indexed(seg, dc);
	const uint64 *bits = (const uint64 *) live_sb_bits(seg, dm, dc);
	int			nw = dm->rows[page] ? (dm->rowlen[page] + 63) / 64 : 0,
				left = sift_kern->expand(mask, mask, dm->om + dm->maskidx[page] * SIFT_OFFMASK_WORDS, bits,
										 dc->rank_base % 8 + dm->before[page], nw);

	memset(mask + nw, 0, (SIFT_OFFMASK_WORDS - nw) * sizeof(uint64));
	return left;
}

/*
 * The live offsets of every present page of the loaded SB, nwmax words each, in the order of om: the rows are packed
 * and spread over the present offsets by one expansion, which costs less than a page's own once the group has a
 * fraction of the SB's pages. They are a group of bitmaps, so a term's group meets them in the count kernel.
 */
static const uint64 *
live_sb_offsets(SiftSegment *seg, DocMap *dm, const SiftDocChunk *dc)
{
	const uint64 *bits = (const uint64 *) live_sb_bits(seg, dm, dc);
	int			nw = dm->nwmax;
	uint64	   *rows;

	if (dm->live_om_ok)
		return dm->live_om;
	if (dm->live_om == NULL)
		dm->live_om = MemoryContextAllocAligned(seg->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
												SIFT_SIMD_ALIGN, 0);
	rows = dm->live_om;
	for (int k = 0; k < dm->np; k++)
		for (int j = 0; j < nw; j++)
			rows[k * nw + j] = dm->om[k * SIFT_OFFMASK_WORDS + j];
	sift_kern->expand(rows, rows, rows, bits, dc->rank_base % 8, dm->np * nw);
	dm->live_om_ok = true;
	return rows;
}

static uint64
count_live1(SiftSegment *seg, const SiftDocChunk *dc, const sift_grp *g, const uint64 *pages)
{
	DocMap	   *dm = loaded(seg, dc);
	const uint8 *off = g->off;
	int			sel = 0;
	uint64		n = 0;

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		sel += sift_pc64(pages[wd]);
	if (memcmp(pages, g->pages, sizeof(g->pages)) == 0 && (dm->live_om_ok || sel * 12 >= dm->np * dm->nwmax))
	{
		sift_grp	rows = {.gid = dc->sb,.w = dm->nwmax * 8,.off = (const uint8 *) live_sb_offsets(seg, dm, dc)};

		memcpy(rows.pages, dm->pagemask, sizeof(rows.pages));
		return sift_grp_and_count(g, &rows);
	}
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			bit = pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};

			if (pages[wd] >> bit & 1)
			{
				memcpy(mask, off, g->w);
				n += sift_seg_page_mask_live(seg, dc, wd * 64 + bit, mask);
			}
		}
	return n;
}

uint64
sift_seg_live_count(SiftSegment *seg, const sift_grp *g, const uint64 *pages)
{
	SiftDocChunk dc;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_count(seg, g, pages);
	return sift_seg_docs(seg, g->gid, &dc) ? count_live1(seg, &dc, g, pages) : 0;
}

int
sift_seg_live_mask(SiftSegment *seg, uint32 sb, int page, uint64 *mask)
{
	SiftDocChunk dc;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_mask(seg, sb, page, mask);
	return sift_seg_docs(seg, sb, &dc) ? sift_seg_page_mask_live(seg, &dc, page, mask) : 0;
}

/* The live offsets of a page */
void
sift_seg_page_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *live)
{
	if (sift_seg_page(seg, dc, page, live, NULL) && dc->dirty)
		sift_seg_page_mask_live(seg, dc, page, live);
}

bool
sift_seg_lookup(SiftSegment *seg, const char *term, int len, SiftTermInfo *info)
{
	if (!sift_dict_lookup(seg->dict, term, len, info))
		return false;
	if (seg->entry.flags & SIFT_SEG_DIRTY)
		sift_seg_term_stats(seg, info);
	return true;
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

void
sift_seg_live_totals(SiftSegment *seg, uint64 *ndocs, uint64 *sum_dl)
{
	*ndocs = seg->hdr->ndocs;
	*sum_dl = seg->hdr->sum_dl;
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS && (seg->entry.flags & SIFT_SEG_DIRTY))
	{
		const SiftLive2HeaderData *h = &live2(seg)->h;

		*ndocs -= h->ndead;
		*sum_dl -= h->dead_dl;
	}
}

/* Is the doc of the key live: its offset is in the rows of its SB, which are all the SB's docs while the SB is clean */
static bool
live2_key(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int			off = (int) (key & 0xFFFF) - 1;
	const sift_grp *g;
	const uint64 *row;

	if (!sift_seg_sb_dirty(seg, sift_block_sb(blk)))
		return true;
	g = live2_rows(seg, sift_block_sb(blk));
	row = g ? (const uint64 *) grp_row(g, blk & (SIFT_SB_PAGES - 1)) : NULL;
	return row && off / 64 < (int) (g->w / 8) && (row[off / 64] >> (off % 64) & 1);
}

/* The header the segment kept when it read its first dfs (live2) is the one they count: its table was complete and no doc has been cleared since */
bool
sift_seg_live_stable(SiftSegment *seg)
{
	SiftLive	l;
	SiftLive2HeaderData h;
	const SiftLive2HeaderData *kept;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	if (seg->fmt < SIFT_SEGFMT_LIVEROWS)
		return false;
	l = seg_live(seg);
	sift_live2_header(&l, &h);
	kept = &live2(seg)->h;
	return kept->tbl_ndead == kept->ndead && h.ndead == kept->ndead;
}

bool
sift_seg_key_live(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	SiftDocChunk dc;
	uint64		live[SIFT_OFFMASK_WORDS];
	int			off = (int) (key & 0xFFFF) - 1;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_key(seg, key);
	if (!sift_seg_sb_dirty(seg, sift_block_sb(blk)) || !sift_seg_docs(seg, sift_block_sb(blk), &dc))
		return true;
	sift_seg_page_live(seg, &dc, blk & (SIFT_SB_PAGES - 1), live);
	return off >= 0 && off < SIFT_OFFMASK_WORDS * 64 && (live[off / 64] >> (off % 64) & 1);
}

void
sift_seg_term_stats(SiftSegment *seg, SiftTermInfo *info)
{
	info->ldf = info->df;
	if (seg->fmt < SIFT_SEGFMT_LIVEROWS || !(seg->entry.flags & SIFT_SEG_DIRTY))
		return;
	if (info->slot >= 0)
	{
		SiftLive	l = seg_live(seg);

		info->ldf -= Min(info->df, sift_live2_dead(&l, &live2(seg)->h, info->slot));
	}
	else
		for (uint32 i = 0; i < info->df; i++)
			info->ldf -= !live2_key(seg, info->inline_keys[i]);
}

bool
sift_seg_term_live(SiftSegment *seg, const SiftTermInfo *info)
{
	MemoryContext cx;
	SiftPostingCursor *c;
	bool		live = false;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
	{
		SiftTermInfo t = *info;

		sift_seg_term_stats(seg, &t);
		return t.ldf > 0;
	}
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift term live", ALLOCSET_SMALL_SIZES);
	c = sift_post_open(&seg->sr, info, cx);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g && !live; g = sift_next(sift_post_cursor(c)))
		live = group_has_live(seg, g);
	sift_sr_release(&seg->sr);
	MemoryContextDelete(cx);
	return live;
}

/* ---------- VACUUM ---------- */

/* Deletes the docs of ranks dead[] (ascending) of one SB (format 1) */
static void
delete_ranks(SiftSegment *seg, uint32 sb, const uint64 *dead, int nd)
{
	SiftLive	l = seg_live(seg);

	sift_seg_mark_dirty(seg->index, &seg->entry);
	sift_live_clear(&l, sb - seg->hdr->sb_lo, dead, nd);
	docmap(seg)->live_ok = docmap(seg)->live_om_ok = false;
}

/* Deletes the docs with these bits of the run (format 2) in one SB, dead_dl the sum of their dl */
static void
delete_bits2(SiftSegment *seg, uint32 sb, const uint64 *bits, int nd, uint64 dead_dl)
{
	SiftLive	l = seg_live(seg);

	sift_seg_mark_dirty(seg->index, &seg->entry);
	sift_live2_clear(&l, sb - seg->hdr->sb_lo, bits, nd, dead_dl);
	live2_cleared(seg, nd, dead_dl);
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

	if (!sift_seg_docs(seg, sift_block_sb(blk), &dc) || !(dc.pagemask[page / 64] >> (page % 64) & 1))
		return -1;
	sift_seg_page(seg, &dc, page, present, NULL);
	sift_seg_page_live(seg, &dc, page, live);
	if (!(live[off / 64] >> (off % 64) & 1))
		return -1;
	rank = dc.rank_base + docmap(seg)->before[page];
	for (int w = 0; w < off / 64; w++)
		rank += pg_popcount64(present[w]);
	return rank + pg_popcount64(present[off / 64] & ((UINT64CONST(1) << (off % 64)) - 1));
}

/*
 * One doc of a segment of format 2. The table of dead postings is left behind (tbl_ndead < ndead) until a VACUUM of the index
 * counts it again: a host that deletes one doc at a time writes format 1
 */
static bool
delete_key2(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int			page = blk & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;
	SiftDocChunk dc;
	const sift_grp *rows;
	const uint64 *row;
	uint64		present[SIFT_OFFMASK_WORDS],
				bit;
	const uint16 *dl;

	if (!sift_seg_docs(seg, sift_block_sb(blk), &dc) || !(dc.pagemask[page / 64] >> (page % 64) & 1))
		return false;
	rows = live2_rows(seg, dc.sb);
	row = rows ? (const uint64 *) grp_row(rows, page) : NULL;
	if (row == NULL || off / 64 >= (int) (rows->w / 8) || !(row[off / 64] >> (off % 64) & 1))
		return false;
	sift_seg_page(seg, &dc, page, present, &dl);
	bit = (live2(seg)->rstart + sizeof(SiftLive2RegionData) + ((const uint8 *) row - rows->off)) * 8 + off;
	delete_bits2(seg, dc.sb, &bit, 1, dl[off]);
	return true;
}

bool
sift_seg_delete_key(SiftSegment *seg, SiftKey key)
{
	int64		rank;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return delete_key2(seg, key);
	rank = sift_seg_live_rank(seg, key);
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

		if (!(dc->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, dc, p, present, NULL);
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

/*
 * Asks cb about every live doc of the SB (format 2); returns the bits of the dead ones in the LIVE run in bits[] and the sum of
 * their dl in *dead_dl
 */
static int
sb_dead2(SiftSegment *seg, const SiftDocChunk *dc, IndexBulkDeleteCallback cb, void *cbstate,
		 IndexBulkDeleteResult *stats, uint64 *bits, uint64 *dead_dl)
{
	const sift_grp *rows = live2_rows(seg, dc->sb);
	uint64		first = (live2(seg)->rstart + sizeof(SiftLive2RegionData)) * 8;
	int			nd = 0,
				k = 0;

	if (rows == NULL || memcmp(rows->pages, dc->pagemask, sizeof(dc->pagemask)) != 0)
		corrupt(seg, "The live rows of SB %u are not those of its pages.", dc->sb);
	*dead_dl = 0;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS];
		const uint64 *live = (const uint64 *) (rows->off + (Size) k * rows->w);
		const uint16 *dl;

		if (!(dc->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, dc, p, present, &dl);
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			for (uint64 m = present[wd] & (wd < (int) (rows->w / 8) ? live[wd] : 0); m; m &= m - 1)
			{
				ItemPointerData tid;
				int			o = wd * 64 + __builtin_ctzll(m);

				ItemPointerSet(&tid, dc->sb * SIFT_SB_PAGES + p, o + 1);
				if (cb(&tid, cbstate))
				{
					bits[nd++] = first + (uint64) k * rows->w * 8 + o;
					*dead_dl += dl[o];
				}
				else
					stats->num_index_tuples++;
			}
		k++;
	}
	return nd;
}

/* ---------- the dead postings of each term (format 2) ---------- */

typedef struct DeadScan
{
	SiftSegment *seg;
	MemoryContext tcxt;
	sift_grp  **grp;			/* by SB from sb_lo: the dead docs as rows, NULL when the SB has none */
	uint32	   *dead;			/* by slot */
	uint32		nslots;
} DeadScan;

/* The dead docs of the SB at position i of the SB directory (present and not live), as a group of the rows' width */
static sift_grp *
dead_rows(SiftSegment *seg, DocMap *dm, int64 i, MemoryContext cx)
{
	uint32		sb = dm->dir[i].sb;
	const sift_grp *rows;
	sift_grp   *g;
	uint64	   *out;
	bool		any = false;

	load_sb(seg, dm, i);
	rows = live2_rows(seg, sb);
	if (rows == NULL || memcmp(rows->pages, dm->pagemask, sizeof(dm->pagemask)) != 0)
		corrupt(seg, "The live rows of SB %u are not those of its pages.", sb);
	g = MemoryContextAlloc(cx, sizeof(sift_grp));
	out = MemoryContextAllocAligned(cx, (Size) dm->np * rows->w, SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	for (int k = 0; k < dm->np; k++)
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		{
			uint64		om = dm->om[k * SIFT_OFFMASK_WORDS + wd];

			if (wd >= (int) (rows->w / 8))
			{
				if (om)
					corrupt(seg, "The live rows of SB %u are narrower than its docs.", sb);
				continue;
			}
			out[(Size) k * (rows->w / 8) + wd] = om & ~((const uint64 *) rows->off)[(Size) k * (rows->w / 8) + wd];
			any |= out[(Size) k * (rows->w / 8) + wd] != 0;
		}
	*g = (sift_grp) {.gid = sb,.w = rows->w,.off = (const uint8 *) out};
	memcpy(g->pages, rows->pages, sizeof(g->pages));
	return any ? g : NULL;
}

static bool
dead_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DeadScan   *ds = arg;
	SiftSegment *seg = ds->seg;
	SiftPostingCursor *c;
	sift_cur   *cur;
	uint64		n = 0;

	if (info->slot < 0)
		return true;
	if (info->slot >= ds->nslots)
		corrupt(seg, "A term has slot %d in a table of %u.", info->slot, ds->nslots);
	c = sift_post_open(&seg->sr, info, ds->tcxt);
	sift_sr_unlock(&seg->sr);
	cur = sift_post_cursor(c);
	for (const sift_grp *g = sift_next(cur); g; g = sift_next(cur))
	{
		const sift_grp *d;

		if (g->gid < seg->hdr->sb_lo || g->gid > seg->hdr->sb_hi)
			corrupt(seg, "A posting lies in SB %u, outside the segment's %u to %u.", g->gid, seg->hdr->sb_lo, seg->hdr->sb_hi);
		d = ds->grp[g->gid - seg->hdr->sb_lo];
		if (d)
			n += sift_grp_and_count(g, d);
	}
	ds->dead[info->slot] = n;
	MemoryContextReset(ds->tcxt);
	CHECK_FOR_INTERRUPTS();
	return true;
}

/* Counts again, for every term with a slot, the postings of the docs that are dead now: the whole table, so that a repeat is harmless */
static void
count_dead(SiftSegment *seg, const SiftLive2HeaderData *h, uint32 *dead)
{
	SiftLive	l = seg_live(seg);
	DocMap	   *dm = docmap(seg);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift dead postings", ALLOCSET_DEFAULT_SIZES);
	DeadScan	ds = {.seg = seg,.dead = dead,.nslots = h->nslots};
	uint8	   *dirty = palloc((h->nsb + 7) / 8);

	ds.tcxt = AllocSetContextCreate(cx, "sift dead postings term", ALLOCSET_DEFAULT_SIZES);
	ds.grp = MemoryContextAllocZero(cx, (Size) h->nsb * sizeof(sift_grp *));
	memset(dead, 0, (Size) h->nslots * sizeof(uint32));
	sift_live2_dirty(&l, h, dirty);
	live2(seg)->rok = false;
	for (int64 i = 0; i < dm->nsb; i++)
	{
		uint32		idx = dm->dir[i].sb - seg->hdr->sb_lo;

		if (dirty[idx / 8] >> (idx % 8) & 1)
			ds.grp[idx] = dead_rows(seg, dm, i, cx);
		CHECK_FOR_INTERRUPTS();
	}
	live2(seg)->rok = false;
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, dead_term, &ds);
	sift_sr_release(&seg->sr);
	pfree(dirty);
	MemoryContextDelete(cx);
}

/* When the docs were cleared since the table was counted (VACUUM, or a crash that left it short): counts the table again and stores it */
void
sift_seg_refresh_dead(SiftSegment *seg)
{
	SiftLive	l = seg_live(seg);
	SiftLive2HeaderData h;
	uint32	   *dead;

	if (seg->fmt < SIFT_SEGFMT_LIVEROWS)
		return;
	sift_live2_header(&l, &h);
	if (h.tbl_ndead == h.ndead)
		return;
	dead = MemoryContextAllocHuge(CurrentMemoryContext, Max(h.nslots, 1) * sizeof(uint32));
	count_dead(seg, &h, dead);
	sift_live2_set_dead(&l, &h, dead);
	pfree(dead);
	sift_sr_release(&seg->sr);
}

int64
sift_seg_bulkdelete(SiftSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	SiftDocChunk dc;
	uint64	   *dead = palloc(SIFT_SB_PAGES * MAXOFF * sizeof(uint64));
	int64		removed = 0;
	bool		rows = seg->fmt >= SIFT_SEGFMT_LIVEROWS;

	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &dc); sb = dc.sb + 1)
	{
		uint64		dl = 0;
		int			nd = rows ? sb_dead2(seg, &dc, cb, cbstate, stats, dead, &dl) : sb_dead(seg, &dc, cb, cbstate, stats, dead);

		if (nd == 0)
			continue;
		if (rows)
			delete_bits2(seg, dc.sb, dead, nd, dl);
		else
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
	SiftLive	l = seg_live(seg);

	return seg->entry.flags & SIFT_SEG_DIRTY ? seg->hdr->ndocs - sift_live_ndead(&l) : seg->hdr->ndocs;
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
	uint32		tcap;
	sift_tid   *tids;			/* SIFT_GRP_MAXTIDS */
	uint8	   *gtf;
	char		prev[SIFT_MAX_TERM_BYTES];
	int			prevlen;
	bool		stopped;
	uint32		nslots;			/* terms with a slot so far (format 2) */
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
		index_sb(seg, dm);
		w->st->parts.docmap_presence += dm->reclen;
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			const uint64 *om = dm->om + dm->maskidx[p] * SIFT_OFFMASK_WORDS;
			BlockNumber blk = dm->dir[i].sb * SIFT_SB_PAGES + p;
			const uint16 *row = dm->rows[p] ? dl_row(dm, p) : NULL;

			if (row)
			{
				w->st->parts.docmap_dl += SIFT_DL_ROW_BYTES(dm->dlcode, dm->rowlen[p]);
				w->st->parts.docmap_presence -= SIFT_DL_ROW_BYTES(dm->dlcode, dm->rowlen[p]);
			}
			for (int o = 0; row && o < dm->rowlen[p]; o++)
			{
				uint16		v = row[o];

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
	SiftLive	l = seg_live(seg);
	uint32		nsb = l.nsb;
	uint64		start = SIFT_LIVE_START(nsb),
				live = 0;
	Buffer		buf = InvalidBuffer;
	SiftLiveHeaderData lh;
	bool		dirty = false;

	for (int i = 0; i < (int) sizeof(lh); i++)
		((uint8 *) &lh)[i] = sift_live_byte(&l, i, &buf);
	if (lh.nsb != nsb || (start + (seg->hdr->ndocs + 7) / 8 + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD != seg->hdr->live_npages)
		corrupt(seg, "Liveness header lists %u SBs in %u pages, expected %u.", lh.nsb, seg->hdr->live_npages, nsb);
	for (uint32 i = 0; i < (nsb + 7) / 8; i++)
		dirty |= sift_live_byte(&l, sizeof(lh) + i, &buf) != 0;
	for (uint64 r = 0; r < seg->hdr->ndocs; r += 8)
	{
		uint8		v = sift_live_byte(&l, start + r / 8, &buf);

		if (seg->hdr->ndocs - r < 8 && (v >> (seg->hdr->ndocs - r)) != 0)
			corrupt(seg, "Liveness bits are set past the last doc.");
		live += sift_pc64(v);
	}
	UnlockReleaseBuffer(buf);
	if (live + lh.ndead != seg->hdr->ndocs || ((seg->entry.flags & SIFT_SEG_DIRTY) == 0 && (dirty || lh.ndead)))
		corrupt(seg, "%llu live and %u dead of %llu docs (dirty flag %d).",
				ULL(live), lh.ndead, ULL(seg->hdr->ndocs), (seg->entry.flags & SIFT_SEG_DIRTY) != 0);
}

/* Format 2: the live rows are the docmap's offsets less the vacuumed docs, the header counts them, and the table counts their postings */
static void
verify_live2(Walk *w)
{
	SiftSegment *seg = w->seg;
	SiftLive	l = seg_live(seg);
	SiftLive2HeaderData h;
	DocMap	   *dm = docmap(seg);
	uint8	   *dirty;
	uint64		dead = 0,
				dead_dl = 0,
				rowbytes = 0;
	bool		anydirty = false;

	sift_live2_header(&l, &h);
	dirty = palloc((h.nsb + 7) / 8);
	sift_live2_dirty(&l, &h, dirty);
	for (uint32 i = 0; i < (h.nsb + 7) / 8; i++)
		anydirty |= dirty[i] != 0;
	live2(seg)->rok = false;
	for (int64 i = 0; i < dm->nsb; i++)
	{
		uint32		sb = dm->dir[i].sb,
					idx = sb - seg->hdr->sb_lo;
		const sift_grp *rows;
		uint64		sbdead = 0;
		int			k = 0;

		load_sb(seg, dm, i);
		index_sb(seg, dm);
		live2(seg)->rok = false;
		rows = live2_rows(seg, sb);
		if (rows == NULL || memcmp(rows->pages, dm->pagemask, sizeof(dm->pagemask)) != 0)
			corrupt(seg, "The live rows of SB %u are not those of its pages.", sb);
		rowbytes += sizeof(SiftLive2RegionData) + (uint64) dm->np * rows->w;
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			const uint64 *om = dm->om + dm->maskidx[p] * SIFT_OFFMASK_WORDS,
					   *live = (const uint64 *) (rows->off + (Size) k * rows->w);
			const uint16 *dl;

			if (!dm->rows[p])
				continue;
			dl = dl_row(dm, p);
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			{
				uint64		lv = wd < (int) (rows->w / 8) ? live[wd] : 0;

				if ((lv & ~om[wd]) || (wd >= (int) (rows->w / 8) && om[wd]))
					corrupt(seg, "The live row of page %d of SB %u holds offsets the docmap does not (word %d).", p, sb, wd);
				for (uint64 m = om[wd] & ~lv; m; m &= m - 1, sbdead++)
					dead_dl += dl[wd * 64 + __builtin_ctzll(m)];
			}
			k++;
		}
		if (sbdead > 0 && !(dirty[idx / 8] >> (idx % 8) & 1))
			corrupt(seg, "SB %u has %llu dead docs and no dirty bit.", sb, ULL(sbdead));
		dead += sbdead;
	}
	if (dead != h.ndead || dead_dl != h.dead_dl || rowbytes != h.tbl_off - h.rows_off ||
		((seg->entry.flags & SIFT_SEG_DIRTY) == 0 && (anydirty || h.ndead)))
		corrupt(seg, "%llu dead docs (%llu tokens) in %llu bytes of rows; the header %u (%llu), %llu bytes (dirty flag %d).",
				ULL(dead), ULL(dead_dl), ULL(rowbytes), h.ndead, ULL(h.dead_dl), ULL(h.tbl_off - h.rows_off),
				(seg->entry.flags & SIFT_SEG_DIRTY) != 0);
	pfree(dirty);
}

/* The table of dead postings against the postings themselves; after the walk of the terms has checked them */
static void
verify_dead2(Walk *w)
{
	SiftSegment *seg = w->seg;
	SiftLive	l = seg_live(seg);
	SiftLive2HeaderData h;

	sift_live2_header(&l, &h);
	if (h.tbl_ndead == h.ndead)
	{
		uint32	   *have = palloc(Max(h.nslots, 1) * sizeof(uint32)),
				   *want = palloc(Max(h.nslots, 1) * sizeof(uint32));

		if (w->nslots != h.nslots)
			corrupt(seg, "The dictionary has %u terms with a slot, the table %u.", w->nslots, h.nslots);
		sift_live2_dead_all(&l, &h, have);
		count_dead(seg, &h, want);
		for (uint32 i = 0; i < h.nslots; i++)
			if (have[i] != want[i])
				corrupt(seg, "The table counts %u dead postings for slot %u, the postings %u.", have[i], i, want[i]);
		pfree(have);
		pfree(want);
	}
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

static int
varint_len(uint64 x)
{
	int			n = 1;

	for (; x >= 0x80; x >>= 7)
		n++;
	return n;
}

/* The bytes of one term in the parts of SiftPartStats, as the writers lay them out (bm25/dict.h, bm25/posting.h) */
static void
account_term(SiftPartStats *ps, bool packed, int shared, int len, const SiftTermInfo *info, const SiftPostingCursor *c)
{
	uint64		df = info->df;
	bool		pos = (info->flags & SIFT_TERM_POS) != 0;
	bool		tf1 = (info->flags & SIFT_TERM_ALL_TF1) != 0;

	if (packed)
	{
		ps->dict_terms += 1 + (shared >= 15) + (len - shared - 1 >= 15) + len - shared;
		ps->dict_entries += 2 + (df >= 15 ? varint_len(df - 15) : 0);
	}
	else
	{
		ps->dict_terms += 2 + len - shared;
		ps->dict_entries += varint_len(df) + 2;
	}
	if (info->flags & SIFT_TERM_INLINE)
	{
		ps->inline_terms++;
		ps->dict_inline_keys += (packed ? varint_len(sift_dict_pack_key(info->inline_keys[0])) : varint_len(info->inline_keys[0])) +
			(df > 1 ? (packed ? varint_len(sift_dict_pack_key(info->inline_keys[1]) - sift_dict_pack_key(info->inline_keys[0])) :
					   varint_len(info->inline_keys[1] - info->inline_keys[0])) : 0) + (tf1 ? 0 : df);
		if (pos)
			ps->dict_inline_pos += varint_len(info->pos_len) + info->pos_len;
	}
	else
	{
		uint64		side = info->post_len - info->codec_len - (pos ? info->pos_len : 0);
		uint32		ng,
					nrows;

		ps->blob_terms++;
		ps->dict_entries += varint_len(info->post_len) + varint_len(info->post_len - info->codec_len) +
			(tf1 ? 0 : varint_len(info->nnib)) + (pos ? varint_len(info->pos_len) : 0);
		sift_post_group_stats(c, &ng, &nrows);
		if (info->flags & SIFT_TERM_SPARSE)
			ps->list_codec += info->codec_len;
		else
		{
			ps->grouped_dir += 20 + 12 * (uint64) ng;
			ps->grouped_payload += info->codec_len - 20 - 12 * (uint64) ng;
		}
		if (side > 0)
		{
			ps->align_pad += ALIGN4(info->codec_len) - info->codec_len;
			side -= ALIGN4(info->codec_len) - info->codec_len;
			ps->gscore += (uint64) ng * sizeof(SiftGroupScore);
			ps->blkbound += (uint64) nrows * SIFT_BLKS_PER_SB;
			side -= (uint64) ng * sizeof(SiftGroupScore) + (uint64) nrows * SIFT_BLKS_PER_SB;
			if (!tf1)
			{
				ps->tf_l1 += (df + 7) / 8;
				ps->tf_nib += ((uint64) info->nnib + 1) / 2;
				ps->tf_esc += side - (df + 7) / 8 - ((uint64) info->nnib + 1) / 2;
			}
		}
		if (pos)
		{
			ps->pos_ckpt += (df + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY * sizeof(uint32);
			ps->pos_records += info->pos_len - (df + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY * sizeof(uint32);
		}
	}
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
				bi = 0,
				shared = 0;
	uint32		n = 0;
	uint64		pos = 0;
	SiftDfClassStats *cls = &w->st->cls[df_class(Max(info->df, 1))];

	if (w->verify && w->prevlen >= 0 &&
		(memcmp(w->prev, term, Min(len, w->prevlen)) > 0 ||
		 (memcmp(w->prev, term, Min(len, w->prevlen)) == 0 && w->prevlen >= len)))
		corrupt(seg, "Dictionary term \"%.*s\" follows \"%.*s\".", len, term, w->prevlen, w->prev);
	/* a block's first term is not front coded */
	if (w->st->nterms % SIFT_DICT_BLOCK != 0)
		while (shared < Min(len, w->prevlen) && term[shared] == w->prev[shared])
			shared++;
	memcpy(w->prev, term, len);
	w->prevlen = len;
	if (info->df > w->ndocs)
		corrupt(seg, "Term \"%.*s\" has df %u in a segment of %llu docs.", len, term, info->df, ULL(w->ndocs));
	if (w->verify && seg->fmt >= SIFT_SEGFMT_LIVEROWS &&
		info->slot != ((info->flags & SIFT_TERM_INLINE) ? -1 : (int32) w->nslots++))
		corrupt(seg, "Term \"%.*s\" has slot %d, expected %d.", len, term, info->slot,
				(info->flags & SIFT_TERM_INLINE) ? -1 : (int) w->nslots - 1);
	if (info->df > w->tcap)
	{
		w->tcap = Max(info->df, w->tcap * 2);
		w->tkeys = w->tkeys ? repalloc_huge(w->tkeys, w->tcap * sizeof(SiftKey)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(SiftKey));
		w->ttf = w->ttf ? repalloc_huge(w->ttf, w->tcap) : MemoryContextAllocHuge(seg->mcxt, w->tcap);
		w->tdl = w->tdl ? repalloc_huge(w->tdl, w->tcap * sizeof(uint16)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint16));
		w->tpoff = w->tpoff ? repalloc_huge(w->tpoff, w->tcap * sizeof(uint32)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint32));
		w->tplen = w->tplen ? repalloc_huge(w->tplen, w->tcap * sizeof(uint32)) :
			MemoryContextAllocHuge(seg->mcxt, w->tcap * sizeof(uint32));
	}
	c = w->verify ? sift_post_open_verified(&seg->sr, info, w->tcxt) : sift_post_open(&seg->sr, info, w->tcxt);
	if (w->verify)
		sift_post_check(c);
	b = sift_post_bounds(c, &nb);
	account_term(&w->st->parts, seg->fmt >= SIFT_SEGFMT_PACKED, shared, len, info, c);
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
	w->stopped = w->visit && !w->visit(w->arg, term, len, info, w->tkeys, w->ttf, w->tdl, w->tprec, w->tplen);
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
	if (verify && seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		verify_live2(&w);
	else if (verify)
		verify_live(&w);
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, walk_term, &w);
	if (!w.stopped && st->nterms != seg->hdr->nterms)
		corrupt(seg, "Dictionary holds %llu terms, the header %llu.", ULL(st->nterms), ULL(seg->hdr->nterms));
	st->parts.docmap_dir = (uint64) docmap(seg)->nsb * sizeof(SiftDocSBRef) + sizeof(uint32);
	st->parts.dictidx = s[SIFT_SEC_DICTIDX].len + seg->hdr->dictroot_n * sizeof(uint64);
	st->parts.dict_blocks = s[SIFT_SEC_DICT].len - st->parts.dict_terms - st->parts.dict_entries - st->parts.dict_inline_keys -
		st->parts.dict_inline_pos;
	if (verify && !w.stopped && seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		verify_dead2(&w);
	MemoryContextDelete(w.tcxt);
}

/* ---------- the live part of a segment, streamed (the input of a merge) ---------- */

/*
 * The dl of every doc of an SB, in the rows of its DOCMAP record, and which of them were vacuumed: what the postings of the SB
 * look up. Terms come in dictionary order and each one runs through the SBs, so the SBs a term touches are kept (the most
 * recently used within a budget) instead of the dl of the whole segment.
 */
typedef struct SBDocs
{
	struct SBDocs *newer;
	struct SBDocs *older;
	int64		at;				/* in the SB directory */
	Size		size;
	uint16	   *dl;				/* the rows of its pages one after the other */
	uint8	   *dead;			/* a bit for each doc slot of dl: vacuumed (NULL in a clean SB) */
	uint32		start[SIFT_SB_PAGES];	/* of the row of each page in dl */
	uint16		len[SIFT_SB_PAGES];		/* 0: no docs on the page */
} SBDocs;

typedef struct DocCache
{
	SiftSegment *seg;
	MemoryContext mcxt;
	SBDocs	  **sb;				/* by position in the SB directory */
	SBDocs	   *newest;
	SBDocs	   *oldest;
	Size		used;
	Size		budget;
	uint16	   *rows;			/* scratch of a build: SIFT_SB_PAGES * MAXOFF slots and their dead bits */
	uint8	   *dead;
} DocCache;

static void
lru_unlink(DocCache *dc, SBDocs *e)
{
	*(e->newer ? &e->newer->older : &dc->newest) = e->older;
	*(e->older ? &e->older->newer : &dc->oldest) = e->newer;
}

static void
lru_push(DocCache *dc, SBDocs *e)
{
	e->newer = NULL;
	e->older = dc->newest;
	*(dc->newest ? &dc->newest->newer : &dc->oldest) = e;
	dc->newest = e;
}

/*
 * The dirty bit of an SB is cached when the segment is first asked for docs, and goes stale when docs are deleted through the
 * same SiftSegment later (the MariaDB merge drops its dead documents first): a segment with a vacuumed doc is dirty in every SB
 */
static void
dirty_everywhere(const SiftSegment *seg, SiftDocChunk *ch)
{
	ch->dirty = (seg->entry.flags & SIFT_SEG_DIRTY) != 0;
}

/* The docs of the SB at position at of the directory */
static SBDocs *
sbdocs_build(DocCache *dc, int64 at)
{
	SiftSegment *seg = dc->seg;
	SiftDocChunk ch;
	SBDocs	   *e = MemoryContextAllocZero(dc->mcxt, sizeof(SBDocs));
	uint32		total = 0;
	bool		anydead = false;

	sift_seg_docs(seg, docmap(seg)->dir[at].sb, &ch);
	dirty_everywhere(seg, &ch);
	memset(dc->dead, 0, (SIFT_SB_PAGES * MAXOFF + 7) / 8);
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS],
					live[SIFT_OFFMASK_WORDS];
		const uint16 *dl;
		int			len = 0;

		if (!(ch.pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, &ch, p, present, &dl);
		for (int wd = SIFT_OFFMASK_WORDS - 1; wd >= 0 && len == 0; wd--)
			if (present[wd])
				len = wd * 64 + 64 - __builtin_clzll(present[wd]);
		memcpy(dc->rows + total, dl, len * sizeof(uint16));
		if (ch.dirty)
		{
			sift_seg_page_live(seg, &ch, p, live);
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
				for (uint64 m = present[wd] & ~live[wd]; m; m &= m - 1, anydead = true)
				{
					uint32		slot = total + wd * 64 + __builtin_ctzll(m);

					dc->dead[slot / 8] |= 1 << (slot % 8);
				}
		}
		e->start[p] = total;
		e->len[p] = len;
		total += len;
	}
	e->at = at;
	e->dl = MemoryContextAlloc(dc->mcxt, total * sizeof(uint16));
	memcpy(e->dl, dc->rows, total * sizeof(uint16));
	e->size = sizeof(SBDocs) + total * sizeof(uint16);
	if (anydead)
	{
		e->dead = MemoryContextAlloc(dc->mcxt, (total + 7) / 8);
		memcpy(e->dead, dc->dead, (total + 7) / 8);
		e->size += (total + 7) / 8;
	}
	return e;
}

static SBDocs *
sbdocs_get(DocCache *dc, uint32 sb)
{
	DocMap	   *dm = docmap(dc->seg);
	int64		at = sb_search(dm, sb);
	SBDocs	   *e;

	if (at == dm->nsb || dm->dir[at].sb != sb)
		corrupt(dc->seg, "A posting lies in SB %u, which has no docs.", sb);
	if ((e = dc->sb[at]) != NULL)
	{
		if (e != dc->newest)
		{
			lru_unlink(dc, e);
			lru_push(dc, e);
		}
		return e;
	}
	e = sbdocs_build(dc, at);
	while (dc->oldest && dc->used + e->size > dc->budget)
	{
		SBDocs	   *old = dc->oldest;

		lru_unlink(dc, old);
		dc->sb[old->at] = NULL;
		dc->used -= old->size;
		pfree(old->dl);
		if (old->dead)
			pfree(old->dead);
		pfree(old);
	}
	dc->sb[at] = e;
	dc->used += e->size;
	lru_push(dc, e);
	return e;
}

/* The dl of the doc with this key, 0 when it was vacuumed; a posting of a doc the SB does not have is corruption */
static uint16
sbdocs_dl(DocCache *dc, const SBDocs *e, SiftKey key)
{
	int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;
	uint32		slot;

	if (off < 0 || off >= e->len[page] || e->dl[slot = e->start[page] + off] == 0)
		corrupt(dc->seg, "Posting (%u,%u) is not a doc of the segment.", sift_key_block(key), (uint32) (key & 0xFFFF));
	return e->dead && (e->dead[slot / 8] >> (slot % 8) & 1) ? 0 : e->dl[slot];
}

/* A term can have this many postings (a group, the most the evaluator decodes at once) */
#define STREAM_POSTINGS		SIFT_GRP_MAXTIDS

typedef struct Stream
{
	SiftSegment *seg;
	const SiftSegSink *sink;
	void	   *arg;
	DocCache	dc;
	MemoryContext tcxt;
	sift_tid   *tids;
	uint8	   *gtf;
	bool		dirty;			/* some doc was vacuumed */
	/* the live postings not yet delivered */
	uint32		n;
	SiftKey    *keys;
	uint8	   *tf;
	uint16	   *dl;
	uint32	   *plen;
	uint8	   *pos;			/* their records, back to back (positions only) */
	Size		poslen;
	Size		poscap;
} Stream;

static void
stream_flush(Stream *s, bool haspos)
{
	if (s->n > 0)
		s->sink->postings(s->arg, s->keys, s->tf, s->dl, haspos ? s->pos : NULL, haspos ? s->plen : NULL, s->n);
	s->n = 0;
	s->poslen = 0;
}

/* Adds the live postings of the group to the pending ones; pr: the records of the term (read in step with the postings) */
static void
stream_group(Stream *s, SiftPostingCursor *c, const sift_grp *g, SiftPosReader *pr)
{
	SBDocs	   *e = sbdocs_get(&s->dc, g->gid);
	uint32		k = sift_grp_tids(g, NULL, s->tids);

	sift_post_group_tf(c, s->gtf);
	for (uint32 j = 0; j < k; j++)
	{
		uint16		dl = sbdocs_dl(&s->dc, e, s->tids[j]);
		uint32		len = 0;
		const uint8 *rec = pr ? sift_post_pos_next(pr, &len) : NULL;

		if (dl == 0)
			continue;
		if (rec)
		{
			if (s->poslen + len > s->poscap)
			{
				s->poscap = Max(s->poslen + len, Max(8192, s->poscap * 2));
				s->pos = s->pos ? repalloc_huge(s->pos, s->poscap) : MemoryContextAllocHuge(s->dc.mcxt, s->poscap);
			}
			memcpy(s->pos + s->poslen, rec, len);
			s->poslen += len;
			s->plen[s->n] = len;
		}
		s->keys[s->n] = s->tids[j];
		s->tf[s->n] = s->gtf[j];
		s->dl[s->n++] = dl;
	}
}

/* Live postings of the term, counted without delivering them */
static uint64
stream_count(Stream *s, SiftPostingCursor *c)
{
	uint64		live = 0;

	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
	{
		SBDocs	   *e = sbdocs_get(&s->dc, g->gid);
		uint32		k = sift_grp_tids(g, NULL, s->tids);

		for (uint32 j = 0; j < k; j++)
			live += sbdocs_dl(&s->dc, e, s->tids[j]) != 0;
	}
	sift_post_rewind(c);
	return live;
}

static bool
stream_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Stream	   *s = arg;
	SiftPostingCursor *c = sift_post_open(&s->seg->sr, info, s->tcxt);
	bool		haspos = (info->flags & SIFT_TERM_POS) != 0,
				buffered = s->dirty && info->df <= STREAM_POSTINGS;
	SiftPosReader *pr = haspos ? sift_post_pos_reader(c, s->tcxt) : NULL;
	SiftTermInfo live = *info;

	/* the df of a term goes first: known, counted, or (a small term of a vacuumed segment) that of its buffered postings */
	if (s->dirty && !buffered)
		live.df = stream_count(s, c);
	if (!buffered && live.df > 0)
		s->sink->term(s->arg, term, len, &live);
	if (buffered || live.df > 0)
		for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
		{
			stream_group(s, c, g, pr);
			if (!buffered)
				stream_flush(s, haspos);
		}
	if (buffered && s->n > 0)
	{
		live.df = s->n;
		s->sink->term(s->arg, term, len, &live);
		stream_flush(s, haspos);
	}
	MemoryContextReset(s->tcxt);
	CHECK_FOR_INTERRUPTS();
	return true;
}

/* The live docs of the segment in key order, a batch at a time */
static void
stream_docs(Stream *s)
{
	SiftSegment *seg = s->seg;
	SiftDocChunk ch;
	SiftKey		keys[1024];
	uint16		dl[1024];
	int			n = 0;

	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &ch); sb = ch.sb + 1)
	{
		dirty_everywhere(seg, &ch);
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint64		live[SIFT_OFFMASK_WORDS];
			const uint16 *row;

			if (!(ch.pagemask[p / 64] & (1ULL << (p % 64))))
				continue;
			sift_seg_page(seg, &ch, p, live, &row);
			if (ch.dirty)
				sift_seg_page_live(seg, &ch, p, live);
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
				for (uint64 m = live[wd]; m; m &= m - 1)
				{
					int			o = wd * 64 + __builtin_ctzll(m);

					keys[n] = ((SiftKey) (ch.sb * SIFT_SB_PAGES + p) << 16) | (o + 1);
					dl[n++] = row[o];
					if (n == lengthof(keys))
					{
						s->sink->docs(s->arg, keys, dl, n);
						n = 0;
					}
				}
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (n > 0)
		s->sink->docs(s->arg, keys, dl, n);
}

void
sift_seg_stream(SiftSegment *seg, Size budget, const SiftSegSink *sink, void *arg)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift stream", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	Stream		s = {.seg = seg,.sink = sink,.arg = arg,.dirty = (seg->entry.flags & SIFT_SEG_DIRTY) != 0};

	s.dc = (DocCache) {.seg = seg,.mcxt = cxt,.budget = budget};
	s.dc.sb = MemoryContextAllocHuge(cxt, docmap(seg)->nsb * sizeof(SBDocs *));
	memset(s.dc.sb, 0, docmap(seg)->nsb * sizeof(SBDocs *));
	s.dc.rows = palloc(SIFT_SB_PAGES * MAXOFF * sizeof(uint16));
	s.dc.dead = palloc((SIFT_SB_PAGES * MAXOFF + 7) / 8);
	s.tcxt = AllocSetContextCreate(cxt, "sift stream term", ALLOCSET_DEFAULT_SIZES);
	s.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	s.gtf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	s.keys = palloc(STREAM_POSTINGS * sizeof(SiftKey));
	s.tf = palloc(STREAM_POSTINGS);
	s.dl = palloc(STREAM_POSTINGS * sizeof(uint16));
	s.plen = palloc(STREAM_POSTINGS * sizeof(uint32));
	stream_docs(&s);
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, stream_term, &s);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* sift_seg_dump: the postings of a term gathered, for the visitor of whole terms */
typedef struct DumpTerm
{
	MemoryContext mcxt;
	SiftDocVisit docs;
	SiftTermVisit visit;
	void	   *arg;
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;
	SiftTermInfo info;
	uint32		n;
	uint32		cap;
	SiftKey    *keys;
	uint8	   *tf;
	uint16	   *dl;
	uint32	   *plen;
	uint8	   *pos;
	Size		poslen;
	Size		poscap;
} DumpTerm;

static void
collect_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	DumpTerm   *d = arg;

	d->docs(d->arg, keys, dl, n);
}

static void
collect_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DumpTerm   *d = arg;

	memcpy(d->term, term, len);
	d->len = len;
	d->info = *info;
	d->n = d->poslen = 0;
	if (info->df > d->cap)
	{
		d->cap = Max(info->df, d->cap * 2);
		d->keys = d->keys ? repalloc_huge(d->keys, d->cap * sizeof(SiftKey)) : MemoryContextAllocHuge(d->mcxt, d->cap * sizeof(SiftKey));
		d->tf = d->tf ? repalloc_huge(d->tf, d->cap) : MemoryContextAllocHuge(d->mcxt, d->cap);
		d->dl = d->dl ? repalloc_huge(d->dl, d->cap * sizeof(uint16)) : MemoryContextAllocHuge(d->mcxt, d->cap * sizeof(uint16));
		d->plen = d->plen ? repalloc_huge(d->plen, d->cap * sizeof(uint32)) : MemoryContextAllocHuge(d->mcxt, d->cap * sizeof(uint32));
	}
}

static void
collect_postings(void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen,
			  int n)
{
	DumpTerm   *d = arg;

	memcpy(d->keys + d->n, keys, n * sizeof(SiftKey));
	memcpy(d->tf + d->n, tf, n);
	memcpy(d->dl + d->n, dl, n * sizeof(uint16));
	if (pos)
	{
		Size		len = 0;

		for (int i = 0; i < n; i++)
			len += poslen[i];
		if (d->poslen + len > d->poscap)
		{
			d->poscap = Max(d->poslen + len, d->poscap * 2);
			d->pos = d->pos ? repalloc_huge(d->pos, d->poscap) : MemoryContextAllocHuge(d->mcxt, d->poscap);
		}
		memcpy(d->pos + d->poslen, pos, len);
		memcpy(d->plen + d->n, poslen, n * sizeof(uint32));
		d->poslen += len;
	}
	d->n += n;
	if (d->n == d->info.df)
		d->visit(d->arg, d->term, d->len, &d->info, d->keys, d->tf, d->dl, pos ? d->pos : NULL, d->plen);
}

void
sift_seg_dump(SiftSegment *seg, const SiftScoreRef *ref, SiftDocVisit docs, SiftTermVisit terms, void *arg)
{
	DumpTerm	d = {.mcxt = CurrentMemoryContext,.docs = docs,.visit = terms,.arg = arg};

	sift_seg_stream(seg, (Size) maintenance_work_mem * 1024 / 2, &(SiftSegSink) {collect_docs, collect_term, collect_postings}, &d);
	if (d.keys)
	{
		pfree(d.keys);
		pfree(d.tf);
		pfree(d.dl);
		pfree(d.plen);
	}
	if (d.pos)
		pfree(d.pos);
}
