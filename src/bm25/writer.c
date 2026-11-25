/*
 * bm25/writer.c - the segment writer: docmap, terms, header and directory entry (bm25/segment/segment.h).
 */
#include "core/host.h"

#include "core/live/live.h"
#include "bm25/segment/segment.h"

#include "bm25/segment/int.h"

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
	SiftSpool  *liverows;
	uint64	   *reg;
	uint32		nreg;
	uint32		maxreg;
	/* the INCLUDE columns (SIFT_SEGFMT_ATTR): the values of the SB being accumulated, and the records of the SBs flushed, spooled until the docmap ends */
	SiftAttrSrc attr;
	int64	   *av;				/* [col][page][MAXOFF] */
	uint8	   *as;				/* the SiftAttrState of each */
	SiftSpool  *arecs[SIFT_ATTR_MAXCOLS];	/* the records of each column */
	uint64		arec_len[SIFT_ATTR_MAXCOLS];
	SiftAttrRef *adir;			/* [SB][col] */
	/* the length of each field of every doc (SIFT_SEGFMT_FIELDDL): the rows of the SB being accumulated, the records of the SBs flushed (SB by SB, field by field) and where they are */
	int			nf;
	uint16	   *fdl;			/* [field][page][MAXOFF] */
	SiftSpool  *frecs;
	uint64		frec_len;
	SiftFieldDlRef *fdir;		/* [SB][field] */
	SiftFieldDlTrailer ft;
	/* terms */
	SiftPostingWriter *pw;
	SiftDictWriter *dw;
	char		term[SIFT_MAX_TERM_BYTES];
	int			termlen;
	/* a writer of a range of the terms (sift_segw_begin_range): the dictionary entries go to this file instead of dw */
	BufFile    *ent;
	uint64		nterms;
};

static void
sw_pad4(SiftStreamWriter *sw)
{
	sift_sw_write(sw, "\0\0\0", ALIGN4(sift_sw_tell(sw)) - sift_sw_tell(sw));
}

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
		w->liverows = sift_spool_begin();
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
			w->reg = repalloc(w->reg, w->maxreg * sizeof(uint64));
		}
		w->reg[w->nreg] = w->nreg == 0 ? 0 : w->reg[w->nreg - 1];
		w->nreg++;
	}
	/* entries up to idx are the end of the region before; idx + 1 is the end of this one */
	w->reg[idx + 1] = w->reg[idx] + sizeof(r) + (uint64) np * r.w;
	sift_spool_write(w->liverows, &r, sizeof(r));
	for (; k < np; k++)
		sift_spool_write(w->liverows, om + SIFT_OFFMASK_WORDS * k, r.w);
}

/* STOR[STOR-BM25-07]: DOCMAP per-SB dl_code (1 doc dl>255 doubles 256-pg SB dl rows to 16b; v7 per-page) */
static int
dl_code(const SiftSegmentWriter *w, uint16 maxdl)
{
	if (w->fmt < SIFT_SEGFMT_PACKED || maxdl > 255)
		return 0;
	return maxdl > 15 ? 1 : 2;
}

/* The row of n slots in the width of code, little endian, a byte at least: dl itself for code 0, else buf (SIFT_MAX_OFFSET bytes) */
static const uint8 *
dl_row_bytes(const uint16 *dl, int n, int code, uint8 *buf, int *len)
{
	int			bits = SIFT_DL_CODE_BITS(code);

	*len = SIFT_DL_ROW_BYTES(code, n);
	if (code == 0)
		return (const uint8 *) dl;
	memset(buf, 0, *len);
	for (int i = 0; i < n; i++)
		buf[i * bits / 8] |= dl[i] << (i * bits % 8);
	return buf;
}

static void
write_dl_row(SiftStreamWriter *sw, const uint16 *dl, int n, int code)
{
	uint8		buf[SIFT_MAX_OFFSET];
	int			len;
	const uint8 *row = dl_row_bytes(dl, n, code, buf, &len);

	sift_sw_write(sw, row, len);
}

void
sift_segw_fields(SiftSegmentWriter *w, int nfields)
{
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	SIFT_INVARIANT(w->ndocs == 0 && nfields > 0 && nfields <= SIFT_MAX_FIELDS, "sift: field lengths after the first doc, or %d of them", nfields);
#ifdef SIFT_TESTING
	if (sift_test_segfmt > 0 && sift_test_segfmt < SIFT_SEGFMT_FIELDDL)
		return;					/* the test writes a format that has none */
#endif
	w->nf = nfields;
	w->fmt = Max(w->fmt, SIFT_SEGFMT_FIELDDL);
	w->fdl = palloc0((Size) nfields * SIFT_SB_PAGES * MAXOFF * sizeof(uint16));
	w->frecs = sift_spool_begin();
	w->fdir = palloc(w->maxsb * nfields * sizeof(SiftFieldDlRef));
	w->ft = (SiftFieldDlTrailer) {.magic = SIFT_FIELDDL_MAGIC,.nfields = nfields};
	MemoryContextSwitchTo(old);
}

/* The records of the field lengths of the SB being flushed, one per field: the rows of its present pages in the width its longest length needs (w->nsb is the SB's index) */
static void
flush_fields(SiftSegmentWriter *w)
{
	for (int f = 0; f < w->nf; f++)
	{
		SiftFieldDlRef ref = {.off = w->frec_len};
		uint16		maxdl = 0;

		for (int p = 0; p < SIFT_SB_PAGES; p++)
			for (int s = 0; w->pagemask[p / 64] >> (p % 64) & 1 && s < w->maxoff[p]; s++)
				maxdl = Max(maxdl, w->fdl[((Size) f * SIFT_SB_PAGES + p) * MAXOFF + s]);
		if (maxdl > 0)
			ref.dlcode = dl_code(w, maxdl);
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint16	   *row = w->fdl + ((Size) f * SIFT_SB_PAGES + p) * MAXOFF;
			uint8		buf[SIFT_MAX_OFFSET];
			int			len;
			const uint8 *bytes;

			if (!(w->pagemask[p / 64] >> (p % 64) & 1))
				continue;
			if (maxdl > 0)
			{
				bytes = dl_row_bytes(row, w->maxoff[p], ref.dlcode, buf, &len);
				sift_spool_write(w->frecs, bytes, len);
				ref.len += len;
			}
			memset(row, 0, w->maxoff[p] * sizeof(uint16));
		}
		w->frec_len += ref.len;
		w->fdir[(Size) w->nsb * w->nf + f] = ref;
	}
}

void
sift_segw_attrs(SiftSegmentWriter *w, const SiftAttrSrc *src)
{
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);
	/* MEM[MEM-BM25-09]: SiftSegWriter attrs + stack buf (SIFT_ATTR_MAXCOLS*670KB -> src->ncols; 64KB stack buf) */
	Size		n = (Size) src->ncols * SIFT_SB_PAGES * MAXOFF;

	SIFT_INVARIANT(w->ndocs == 0 && src->ncols > 0 && src->ncols <= SIFT_ATTR_MAXCOLS, "sift: INCLUDE columns after the first doc, or %d of them", src->ncols);
	w->attr = *src;
	w->fmt = Max(w->fmt, SIFT_SEGFMT_ATTR);
	w->av = palloc(n * sizeof(int64));
	w->as = palloc0(n);
	for (int c = 0; c < src->ncols; c++)
		w->arecs[c] = sift_spool_begin();
	w->adir = palloc(w->maxsb * src->ncols * sizeof(SiftAttrRef));
	MemoryContextSwitchTo(old);
}

/* Bits a code of up to max needs */
static int
code_bits(uint64 max)
{
	return max == 0 ? 1 : 64 - __builtin_clzll(max);
}

/* The records of the INCLUDE values of the SB being flushed, one per column (header, then the row of each present page), appended to the spool of the column (w->nsb is the SB's index) */
static void
flush_attr(SiftSegmentWriter *w)
{
	const Size	cols = (Size) SIFT_SB_PAGES * MAXOFF;

	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		OffsetNumber offs[MAXOFF];
		int64		vals[SIFT_ATTR_MAXCOLS * MAXOFF];
		uint8		st[SIFT_ATTR_MAXCOLS * MAXOFF];
		int			n = 0;

		if (!(w->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		{
			uint64		m = w->offmask[p * SIFT_OFFMASK_WORDS + k];

			for (; m; m &= m - 1)
				offs[n++] = k * 64 + __builtin_ctzll(m) + 1;
		}
		w->attr.fetch(w->attr.arg, (w->sb << SIFT_SB_SHIFT) + p, offs, n, vals, st);
		for (int c = 0; c < w->attr.ncols; c++)
			for (int i = 0; i < n; i++)
			{
				w->av[c * cols + p * MAXOFF + offs[i] - 1] = vals[c * n + i];
				w->as[c * cols + p * MAXOFF + offs[i] - 1] = st[c * n + i];
			}
	}
	for (int c = 0; c < w->attr.ncols; c++)
	{
		SiftAttrColHdr hdr = {0};
		Size		total = sizeof(hdr),
					pad;
		int64		lo = PG_INT64_MAX,
					hi = PG_INT64_MIN;
		bool		any = false,
					unknown = false;
		uint8	   *rec,
				   *row;

		if (!w->attr.intlike[c])
			hdr.flags = SIFT_ATTR_NOROWS;
		else
		{
			for (int p = 0; p < SIFT_SB_PAGES; p++)
				for (int s = 0; s < w->maxoff[p]; s++)
					if (!(w->offmask[p * SIFT_OFFMASK_WORDS + s / 64] >> (s % 64) & 1))
						continue;
					else if (w->as[c * cols + p * MAXOFF + s] == SIFT_AV_UNKNOWN)
						unknown = true;
					else if (w->as[c * cols + p * MAXOFF + s] == SIFT_AV_VALUE)
					{
						int64		v = w->av[c * cols + p * MAXOFF + s];

						lo = Min(lo, v);
						hi = Max(hi, v);
						any = true;
					}
			if (any && (uint64) hi - (uint64) lo >= (UINT64CONST(1) << SIFT_ATTR_MAXWIDTH) - 2)
				hdr.flags = SIFT_ATTR_NOROWS;
		}
		if (!(hdr.flags & SIFT_ATTR_NOROWS))
		{
			hdr.flags = unknown ? 0 : SIFT_ATTR_NOUNK;
			hdr.base = any ? lo : 0;
			hdr.unk = any ? (uint64) hi - (uint64) lo + 2 : 1;
			/* STOR[STOR-BM25-08]: adir INCLUDE per-SB bitpack width=bits(hi-lo+2) over 0..maxoff-1 (v7 dict/frame) */
			hdr.width = code_bits(hdr.unk);
			for (int p = 0; p < SIFT_SB_PAGES; p++)
				if (w->pagemask[p / 64] & (1ULL << (p % 64)))
					total += ATTR_ROW_BYTES(w->maxoff[p], hdr.width);
		}
		rec = palloc0(total + 16);
		memcpy(rec, &hdr, sizeof(hdr));
		row = rec + sizeof(hdr);
		for (int p = 0; p < SIFT_SB_PAGES && !(hdr.flags & SIFT_ATTR_NOROWS); p++)
		{
			if (!(w->pagemask[p / 64] & (1ULL << (p % 64))))
				continue;
			for (int s = 0; s < w->maxoff[p]; s++)
			{
				uint64		code = 0,
							x;
				uint64		bit = (uint64) s * hdr.width;

				if (w->offmask[p * SIFT_OFFMASK_WORDS + s / 64] >> (s % 64) & 1)
				{
					uint8		st = w->as[c * cols + p * MAXOFF + s];

					code = st == SIFT_AV_NULL ? 0 : st == SIFT_AV_UNKNOWN ? hdr.unk :
						(uint64) (w->av[c * cols + p * MAXOFF + s] - hdr.base) + 1;
				}
				x = code << (bit & 7);
				for (int k = 0; k < 8 && x >> (8 * k); k++)
					row[(bit >> 3) + k] |= (uint8) (x >> (8 * k));
			}
			row += ATTR_ROW_BYTES(w->maxoff[p], hdr.width);
			memset(w->as + c * cols + p * MAXOFF, 0, MAXOFF);
		}
		pad = ALIGN4(total) - total;
		w->adir[(Size) w->nsb * w->attr.ncols + c] = (SiftAttrRef) {w->arec_len[c], (uint32) total, 0};
		sift_spool_write(w->arecs[c], rec, total);
		sift_spool_write(w->arecs[c], "\0\0\0", pad);
		w->arec_len[c] += total + pad;
		pfree(rec);
	}
}

static void
flush_sb(SiftSegmentWriter *w)
{
	uint64	   *om;
	uint8	   *chunk;
	uint32		hdr[2];
	int			np = 0,
				code = dl_code(w, w->sb_maxdl);

	if (w->sb_ndocs == 0)
		return;
	om = palloc(SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64));
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
		w->sbdir = repalloc(w->sbdir, w->maxsb * sizeof(SiftDocSBRef));
		if (w->adir)
			w->adir = repalloc(w->adir, w->maxsb * w->attr.ncols * sizeof(SiftAttrRef));
		if (w->fdir)
			w->fdir = repalloc(w->fdir, w->maxsb * w->nf * sizeof(SiftFieldDlRef));
	}
	if (w->attr.ncols)
		flush_attr(w);
	if (w->nf)
		flush_fields(w);
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
sift_segw_add_docs(SiftSegmentWriter *w, const SiftKey *keys, const uint16 *dl, const uint16 *fdl, int n)
{
	SIFT_INVARIANT(!w->pw, "sift: segment docs must be added before terms");
	SIFT_INVARIANT(w->nf == 0 || fdl != NULL, "sift: no field lengths for the docs of a segment of %d fields", w->nf);
	for (int i = 0; i < n; i++)
	{
		BlockNumber blk = sift_key_block(keys[i]);
		uint32		off = keys[i] & 0xFFFF,
					page = blk & (SIFT_SB_PAGES - 1);

		SIFT_INVARIANT(!((w->ndocs > 0 && keys[i] <= w->last) || off < 1 || off > MAXOFF), "sift: segment doc (%u,%u) dl %u out of order or invalid",
					   blk, off, dl[i]);
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
		for (int f = 0; f < w->nf; f++)
		{
			w->fdl[((Size) f * SIFT_SB_PAGES + page) * MAXOFF + off - 1] = fdl[i * w->nf + f];
			w->ft.sum_dl[f] += fdl[i * w->nf + f];
			w->ft.ndocs_nz[f] += fdl[i * w->nf + f] > 0;
		}
		w->maxoff[page] = Max(w->maxoff[page], off);
		w->sb_maxdl = Max(w->sb_maxdl, dl[i]);
		w->sb_ndocs++;
		w->ndocs++;
		w->sum_dl += dl[i];
		w->last = keys[i];
	}
}

static void
postw_open(SiftSegmentWriter *w)
{
	w->pw = sift_postw_begin(w->sw, &w->ref, w->mcxt);
	if (w->spool_min)
		sift_postw_spool(w->pw, w->spool_min);
	if (w->fmt >= SIFT_SEGFMT_PACKED)
		sift_postw_pack(w->pw);
	if (w->fmt >= SIFT_SEGFMT_POSBLK)
		sift_postw_posblk(w->pw);
}

/* Appends the spooled records of one kind to the stream, where the directory entries that point into them (ref, each of stride bytes) get their offsets */
static void
copy_records(SiftSegmentWriter *w, SiftSpool *records, uint64 *start, uint64 len)
{
	sift_sw_write_spool(w->sw, records);
	*start += len;
}

/* The end of DOCMAP of format 5 and later: the directory of the SBs, then the INCLUDE and field records' directories and the field trailer, then recs_end, the INCLUDE columns and the SBs */
static void
write_trailer(SiftSegmentWriter *w)
{
	uint64		recs_end = sift_sw_tell(w->sw),
				start = recs_end;
	uint32		nc = w->attr.ncols;

	for (uint32 c = 0; c < nc; c++)
	{
		copy_records(w, w->arecs[c], &start, w->arec_len[c]);
		w->arecs[c] = NULL;
		for (uint32 i = 0; i < w->nsb; i++)
			w->adir[(Size) i * nc + c].off += start - w->arec_len[c];
	}
	if (w->nf)
	{
		copy_records(w, w->frecs, &start, w->frec_len);
		w->frecs = NULL;
		sw_pad4(w->sw);
		for (uint32 i = 0; i < w->nsb * w->nf; i++)
			w->fdir[i].off += start - w->frec_len;
	}
	sift_sw_write(w->sw, w->sbdir, w->nsb * sizeof(SiftDocSBRef));
	if (nc)
		sift_sw_write(w->sw, w->adir, (Size) w->nsb * nc * sizeof(SiftAttrRef));
	if (w->nf)
	{
		w->ft.fdir_off = sift_sw_tell(w->sw);
		sift_sw_write(w->sw, w->fdir, (Size) w->nsb * w->nf * sizeof(SiftFieldDlRef));
		sift_sw_write(w->sw, &w->ft, sizeof(w->ft));
	}
	sift_sw_write(w->sw, &recs_end, sizeof(uint64));
	sift_sw_write(w->sw, &nc, sizeof(uint32));
	sift_sw_write(w->sw, &w->nsb, sizeof(uint32));
}

/* Closes DOCMAP and opens POSTINGS on the first term */
static void
start_terms(SiftSegmentWriter *w)
{
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	SIFT_INVARIANT(w->ndocs != 0, "sift: segment terms without docs");
	flush_sb(w);
	w->hdr.sb_hi = w->sb;
	sw_pad4(w->sw);
	if (w->fmt >= SIFT_SEGFMT_ATTR)
		write_trailer(w);
	else
	{
		sift_sw_write(w->sw, w->sbdir, w->nsb * sizeof(SiftDocSBRef));
		sift_sw_write(w->sw, &w->nsb, sizeof(uint32));
	}
	w->hdr.sections[SIFT_SEC_DOCMAP].len = sift_sw_tell(w->sw);
	w->hdr.sections[SIFT_SEC_POSTINGS].off = sift_sw_tell(w->sw);
	w->ref.avgdl = (float4) ((double) w->sum_dl / w->ndocs);
	postw_open(w);
	w->dw = sift_dict_writer_begin(w->sw, w->fmt >= SIFT_SEGFMT_PACKED, w->fmt >= SIFT_SEGFMT_POSBLK);
	pfree(w->offmask);
	pfree(w->dl);
	if (w->fdl)
		pfree(w->fdl);
	MemoryContextSwitchTo(old);
}

SiftPostingWriter *
sift_segw_begin_term(SiftSegmentWriter *w, const char *term, int len)
{
	if (w->pw == NULL && w->ent)
		postw_open(w);
	else if (w->pw == NULL)
		start_terms(w);
	SIFT_INVARIANT(len >= 1 && len <= SIFT_MAX_TERM_BYTES, "sift: term of %d bytes", len);
	sift_postw_field(w->pw, SIFT_FIELD_TERM(term[0], SIFT_MAX_FIELDS));
	memcpy(w->term, term, len);
	w->termlen = len;
	return w->pw;
}

void
sift_segw_end_term(SiftSegmentWriter *w)
{
	SiftTermInfo info;

	sift_postw_end_term(w->pw, &info);
	if (w->ent)
	{
		uint8		len = w->termlen;

		BufFileWrite(w->ent, &len, 1);
		BufFileWrite(w->ent, w->term, len);
		BufFileWrite(w->ent, &info, sizeof(info));
		if ((info.flags & SIFT_TERM_INLINE) && (info.flags & SIFT_TERM_POS))
			BufFileWrite(w->ent, info.inline_pos, info.pos_len);
		w->nterms++;
		return;
	}
	sift_dict_add(w->dw, w->term, w->termlen, &info);
}

SiftSegmentWriter *
sift_segw_begin_range(SiftStreamWriter *post, BufFile *ent, const SiftScoreRef *ref, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftSegmentWriter *w = palloc0(sizeof(SiftSegmentWriter));

	w->fmt = SIFT_WRITE_SEGFMT;
#ifdef SIFT_TESTING
	if (sift_test_segfmt > 0)
		w->fmt = Min(sift_test_segfmt, SIFT_WRITE_SEGFMT);
#endif
	w->mcxt = mcxt;
	w->ref = *ref;
	w->sw = post;
	w->ent = ent;
	w->termlen = -1;
	MemoryContextSwitchTo(old);
	return w;
}

uint64
sift_segw_range_terms(const SiftSegmentWriter *w)
{
	return w->nterms;
}

void
sift_segw_adopt_range(SiftSegmentWriter *w, BufFile *post, uint64 post_len, BufFile *ent, uint64 nterms, float4 avgdl)
{
	uint64		base;
	uint8	   *pos = NULL;
	Size		poscap = 0;

	if (w->pw == NULL)
		start_terms(w);
	SIFT_INVARIANT(w->ref.avgdl == avgdl, "sift: the ranges of a segment were scored with an average length %g, its docs have %g", avgdl, w->ref.avgdl);
	if (post_len > 0)
	{
		sift_sw_write(w->sw, "\0\0\0", ALIGN4(sift_sw_tell(w->sw)) - sift_sw_tell(w->sw));
		sift_sw_write_file(w->sw, post, post_len);
	}
	base = sift_sw_tell(w->sw) - post_len;
	SIFT_INVARIANT(BufFileSeek(ent, 0, 0, SEEK_SET) == 0, "sift: could not rewind a range of terms");
	for (uint64 i = 0; i < nterms; i++)
	{
		SiftTermInfo info;
		char		term[SIFT_MAX_TERM_BYTES];
		uint8		len;

		BufFileReadExact(ent, &len, 1);
		BufFileReadExact(ent, term, len);
		BufFileReadExact(ent, &info, sizeof(info));
		if (info.flags & SIFT_TERM_INLINE)
		{
			if (info.flags & SIFT_TERM_POS)
			{
				if (info.pos_len > poscap)
				{
					poscap = Max(info.pos_len, 256);
					SIFT_REGROW(pos, poscap);
				}
				BufFileReadExact(ent, pos, info.pos_len);
			}
			info.inline_pos = pos;
		}
		else
			info.post_off += base;
		sift_dict_add(w->dw, term, len, &info);
	}
	if (pos)
		pfree(pos);
}

void
sift_segw_finish(SiftSegmentWriter *w, SiftSegEntry *entry)
{
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
	blocks = 1 + w->hdr.live_npages;
	for (int i = 0; i < n; i++)
		blocks += ext[i].nblocks;
	memset(entry, 0, sizeof(SiftSegEntry));
	entry->gen = w->gen;
	entry->kind = SIFT_SEG_IMMUTABLE;
	entry->format = w->fmt;
	entry->flags = w->positions ? SIFT_SEG_POSITIONS : 0;
	entry->header_blk = sift_wt_write_seghdr(w->wt, &w->hdr, ext, n, NULL, 0);
	entry->sb_lo = w->hdr.sb_lo;
	entry->sb_hi = w->hdr.sb_hi;
	entry->ndocs = w->ndocs;
	entry->sum_dl = w->sum_dl;
	entry->bytes = blocks * BLCKSZ;
}
