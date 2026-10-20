/*
 * bm25/writer.c - the segment writer: docmap, terms, header and directory entry (bm25/segment.h).
 */
#include "sift_host.h"

#include "bm25/kernels.h"
#include "sift_live.h"
#include "sift_popcount.h"
#include "bm25/segment.h"

#include "bm25/segment_int.h"

#ifdef SIFT_TESTING
int			sift_test_segfmt = 0;
#endif

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
