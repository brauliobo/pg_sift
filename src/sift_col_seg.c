/*
 * sift_col_seg.c - immutable column segments: writer and reader over the page store.
 *
 * Stream: UNITS (the unit records, 8-byte aligned), DICTS (a directory, then the serialized dictionary of each STR leaf), ZONES (a
 * directory, then per leaf the zone arrays of all units), then SCHEMA and SBDIR. The header page is a SiftSegHeaderData whose sections
 * are, in order of the stream: POSTINGS = UNITS, DICT = DICTS, DICTIDX = ZONES, DOCMAP = SCHEMA + SBDIR (dictroot_off = the schema's bytes,
 * dictroot_n = the units).
 */
#include "sift_host.h"

#include "sift_col_seg.h"
#include "sift_live.h"

#define ULL(x)			((unsigned long long) (x))
#define ALIGN8(x)		TYPEALIGN(8, x)
#define DICTS_MAGIC		0x44444F43		/* "COLD" */
#define ZONES_MAGIC		0x5A4C4F43		/* "COLZ" */

#define corrupt(seg, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" segment %llu is corrupted", \
					RelationGetRelationName((seg)->index), ULL((seg)->entry.gen)), \
			 errdetail(__VA_ARGS__)))

/* ---------- writer ---------- */

struct SiftColSegWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	const SiftColSchema *schema;
	const SiftCcPolicy *pol;	/* [ncols] compression policy per leaf, NULL: plain chunks */
	MemoryContext mcxt;
	SiftStreamWriter *sw;
	SiftCDict **dict;			/* [ncols], STR leaves */
	uint32	  **dictmap;		/* [ncols] scratch: batch code -> segment code */
	bool	   *inl;			/* [ncols] scratch: the STR leaf is stored inline in the unit being added */
	SiftCZone  *zones;			/* [unit * ncols + col] */
	SiftColSBRef *sbdir;
	uint32		nsb;
	uint32		capsb;
	uint64		rows;
	uint32		sb_lo;
	uint32		sb_hi;
	uint32		tag;			/* SiftSegEntry.reserved */
	StringInfoData buf;
};

static void
sw_pad8(SiftStreamWriter *sw)
{
	static const char zeros[8] = {0};

	sift_sw_write(sw, zeros, ALIGN8(sift_sw_tell(sw)) - sift_sw_tell(sw));
}

SiftColSegWriter *
sift_colw_begin(SiftWriteTarget *wt, uint64 gen, const SiftColSchema *schema, const SiftCcPolicy *pol, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSegWriter *w = palloc0(sizeof(SiftColSegWriter));

	w->wt = wt;
	w->gen = gen;
	w->schema = schema;
	w->pol = pol;
	w->mcxt = mcxt;
	w->sw = sift_sw_begin(wt, gen);
	w->dict = palloc0(schema->ncols * sizeof(SiftCDict *));
	w->dictmap = palloc0(schema->ncols * sizeof(uint32 *));
	w->inl = palloc0(schema->ncols * sizeof(bool));
	for (int c = 0; c < schema->ncols; c++)
		if (schema->cols[c].store == SIFT_COLS_STR)
			w->dict[c] = sift_cdict_create(mcxt);
	w->capsb = 64;
	w->zones = palloc(w->capsb * schema->ncols * sizeof(SiftCZone));
	w->sbdir = palloc(w->capsb * sizeof(SiftColSBRef));
	initStringInfo(&w->buf);
	MemoryContextSwitchTo(old);
	return w;
}

/* Interns the batch dictionary of each STR leaf not stored inline into the segment's: dictmap[col][batch code] = segment code */
static void
map_dicts(SiftColSegWriter *w, const SiftColBatch *b, const bool *inl)
{
	for (int c = 0; c < w->schema->ncols; c++)
	{
		const SiftCDict *bd = b->dict[c];

		if (w->dict[c] == NULL || inl[c])
			continue;
		w->dictmap[c] = w->dictmap[c] ? repalloc(w->dictmap[c], Max(bd->n, 1) * sizeof(uint32)) :
			MemoryContextAlloc(w->mcxt, Max(bd->n, 1) * sizeof(uint32));
		for (uint32 i = 0; i < bd->n; i++)
			w->dictmap[c][i] = sift_cdict_intern(w->dict[c], bd->bytes + bd->off[i], bd->off[i + 1] - bd->off[i]);
	}
}

void
sift_colw_add(SiftColSegWriter *w, const SiftColBatch *b)
{
	int			ncols = w->schema->ncols;
	Size		start;
	uint64		off;

	Assert(b->nrows > 0 && b->schema->ncols == ncols);
	if (w->nsb > 0 && b->sb <= w->sbdir[w->nsb - 1].sb)
		elog(ERROR, "sift_col: superblock %u added after %u", b->sb, w->sbdir[w->nsb - 1].sb);
	if (w->nsb == w->capsb)
	{
		w->capsb *= 2;
		w->zones = repalloc_huge(w->zones, (Size) w->capsb * ncols * sizeof(SiftCZone));
		w->sbdir = repalloc_huge(w->sbdir, w->capsb * sizeof(SiftColSBRef));
	}
	for (int c = 0; c < ncols; c++)
		w->inl[c] = w->dict[c] != NULL && sift_cbatch_str_inline(b, c, w->dict[c], SIFT_COL_DICT_MAX_BYTES);
	map_dicts(w, b, w->inl);
	resetStringInfo(&w->buf);
	sift_cunit_encode(b, w->dictmap, w->inl, w->pol, &w->buf, &start, w->zones + (Size) w->nsb * ncols);
	sw_pad8(w->sw);
	off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, w->buf.data + start, w->buf.len - start);
	w->sbdir[w->nsb] = (SiftColSBRef) {b->sb, b->nrows, off, w->rows, w->buf.len - start, 0};
	if (w->nsb == 0)
		w->sb_lo = b->sb;
	w->sb_hi = b->sb;
	w->nsb++;
	w->rows += b->nrows;
}

void
sift_colw_set_tag(SiftColSegWriter *w, uint32 tag)
{
	w->tag = tag;
}

uint64
sift_colw_rows(const SiftColSegWriter *w)
{
	return w->rows;
}

uint64
sift_colw_bytes(const SiftColSegWriter *w)
{
	return sift_sw_tell(w->sw);
}

Size
sift_colw_memory(const SiftColSegWriter *w)
{
	Size		m = (Size) w->nsb * (w->schema->ncols * sizeof(SiftCZone) + sizeof(SiftColSBRef));

	for (int c = 0; c < w->schema->ncols; c++)
		if (w->dict[c])
			m += w->dict[c]->nbytes + (Size) w->dict[c]->n * 3 * sizeof(uint32);
	return m;
}

/* Appends len bytes of buf at the 8-aligned stream position; returns the offset */
static uint64
put8(SiftColSegWriter *w, const void *data, Size len)
{
	uint64		off;

	sw_pad8(w->sw);
	off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, data, len);
	return off;
}

static void
write_dicts(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64	   *offs = palloc0(sizeof(uint32) * 2 + ncols * sizeof(uint64));
	uint32	   *hdr = (uint32 *) offs;
	uint64	   *tab = (uint64 *) (hdr + 2);
	StringInfoData blob;

	sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	hdr[0] = DICTS_MAGIC;
	hdr[1] = ncols;
	/* the directory is written last to know the offsets: serialize the dictionaries first into memory */
	initStringInfo(&blob);
	for (int c = 0; c < ncols; c++)
	{
		if (w->dict[c] == NULL)
			continue;
		while (blob.len % 8)
			appendStringInfoChar(&blob, '\0');
		tab[c] = sizeof(uint32) * 2 + ncols * sizeof(uint64) + blob.len;
		sift_cdict_serialize(w->dict[c], &blob);
	}
	sift_sw_write(w->sw, offs, sizeof(uint32) * 2 + ncols * sizeof(uint64));
	sift_sw_write(w->sw, blob.data, blob.len);
	sw_pad8(w->sw);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(blob.data);
	pfree(offs);
}

/* One leaf's zone arrays: min, max, sum (u64 each), nnull, nvals, flags (u32 each, padded to 8), set (STR) */
static Size
zone_block_size(const SiftColDef *def, uint32 nsb)
{
	return ALIGN8(nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))) + (def->store == SIFT_COLS_STR ? nsb * 4 * sizeof(uint64) : 0);
}

static void
write_zones(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	Size		dirsz = sizeof(uint32) * 2 + ncols * sizeof(uint64),
				at = ALIGN8(dirsz);
	char	   *dir = palloc0(ALIGN8(dirsz));
	uint32	   *hdr = (uint32 *) dir;
	uint64	   *tab = (uint64 *) (dir + sizeof(uint32) * 2);

	hdr[0] = ZONES_MAGIC;
	hdr[1] = ncols;
	for (int c = 0; c < ncols; c++)
	{
		tab[c] = at;
		at += zone_block_size(&w->schema->cols[c], w->nsb);
	}
	sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, dir, ALIGN8(dirsz));
	for (int c = 0; c < ncols; c++)
	{
		const SiftColDef *def = &w->schema->cols[c];
		Size		sz = zone_block_size(def, w->nsb);
		char	   *blk = palloc0(sz);
		uint64	   *mn = (uint64 *) blk,
				   *mx = mn + w->nsb,
				   *sum = mx + w->nsb;
		uint32	   *nnull = (uint32 *) (sum + w->nsb),
				   *nvals = nnull + w->nsb,
				   *flags = nvals + w->nsb;
		uint64	   *set = (uint64 *) (blk + ALIGN8(w->nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))));

		for (uint32 u = 0; u < w->nsb; u++)
		{
			const SiftCZone *z = &w->zones[(Size) u * ncols + c];

			mn[u] = z->min;
			mx[u] = z->max;
			if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
				memcpy(&sum[u], &z->sum_f, sizeof(double));
			else
				sum[u] = z->sum_i;
			nnull[u] = z->nnull;
			nvals[u] = z->nvals;
			flags[u] = z->flags;
			if (def->store == SIFT_COLS_STR)
				memcpy(set + (Size) u * 4, z->set, 4 * sizeof(uint64));
		}
		sift_sw_write(w->sw, blk, sz);
		pfree(blk);
	}
	sw_pad8(w->sw);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(dir);
}

void
sift_colw_finish(SiftColSegWriter *w, SiftSegEntry *entry)
{
	PGIOAlignedBlock page;
	SiftSegHeaderData *hdr = (SiftSegHeaderData *) SiftPageGetData(page.data);
	SiftSegHeaderData h = {0};
	StringInfoData schema;
	SiftExtent *ext;
	int			n;
	uint64		blocks;

	if (w->nsb == 0)
		elog(ERROR, "sift_col: a segment needs at least one unit");
	h.sections[SIFT_SEC_POSTINGS] = (SiftSectionRef) {0, sift_sw_tell(w->sw)};
	write_dicts(w, &h.sections[SIFT_SEC_DICT]);
	write_zones(w, &h.sections[SIFT_SEC_DICTIDX]);
	initStringInfo(&schema);
	sift_cschema_serialize(w->schema, &schema);
	sw_pad8(w->sw);
	h.sections[SIFT_SEC_DOCMAP].off = sift_sw_tell(w->sw);
	h.dictroot_off = schema.len;
	sift_sw_write(w->sw, schema.data, schema.len);
	put8(w, w->sbdir, (Size) w->nsb * sizeof(SiftColSBRef));
	sw_pad8(w->sw);
	h.sections[SIFT_SEC_DOCMAP].len = sift_sw_tell(w->sw) - h.sections[SIFT_SEC_DOCMAP].off;
	ext = sift_sw_finish(w->sw, &n);
	sift_live_write(w->wt, w->gen, w->sb_hi - w->sb_lo + 1, w->rows, &h.live_first, &h.live_npages);
	h.gen = w->gen;
	h.ndocs = w->rows;
	h.nterms = w->schema->ncols;
	h.sb_lo = w->sb_lo;
	h.sb_hi = w->sb_hi;
	h.dictroot_n = w->nsb;
	h.nextents = n;
	sift_page_init(page.data, SIFT_PT_SEGHEADER, w->gen);
	memcpy(hdr, &h, offsetof(SiftSegHeaderData, extents));
	memcpy(hdr->extents, ext, n * sizeof(SiftExtent));
	SiftPageSetUsed(page.data, offsetof(SiftSegHeaderData, extents) + n * sizeof(SiftExtent));
	blocks = 1 + h.live_npages;
	for (int i = 0; i < n; i++)
		blocks += ext[i].nblocks;
	memset(entry, 0, sizeof(SiftSegEntry));
	entry->gen = w->gen;
	entry->kind = SIFT_SEG_IMMUTABLE;
	entry->format = SIFT_COL_FORMAT_MINOR;
	entry->header_blk = sift_wt_write_page(w->wt, page.data);
	entry->sb_lo = w->sb_lo;
	entry->sb_hi = w->sb_hi;
	entry->reserved = w->tag;
	entry->sum_dl = 0;
	entry->ndocs = w->rows;
	entry->bytes = blocks * BLCKSZ;
	pfree(schema.data);
}

/* ---------- reader ---------- */

typedef struct ZoneArrays
{
	const int64 *min;
	const int64 *max;
	const int64 *sum;
	const uint32 *nnull;
	const uint32 *nvals;
	const uint32 *flags;
	const uint64 (*set)[4];
	bool		any_ovf;		/* some unit flags SIFT_COLZ_OVERFLOW */
	bool		any_inl;		/* some unit flags SIFT_COLZ_INLINE: its min and max are prefix keys, not codes */
} ZoneArrays;

typedef struct SegPriv
{
	SiftLive	live;
	uint8	   *dirty;			/* dirty-SB bitmap, loaded when the entry is flagged dirty */
	SiftCDict **dict;
	ZoneArrays **zones;
	uint8	   *unitbuf;
	Size		unitcap;
} SegPriv;

#define PRIV(seg)	((SegPriv *) (seg)->priv)

static void
seg_read(SiftColSegment *seg, uint64 off, Size len, void *dst)
{
	char	   *p = dst;

	/* in pieces of a page's payload: sift_sr_get copies across pages into scratch only when asked for a contiguous range */
	while (len > 0)
	{
		Size		n = Min(len, SIFT_PAGE_PAYLOAD * 8);
		const void *src = sift_sr_get(&seg->sr, off, n, p);

		if (src != p)
			memcpy(p, src, n);
		p += n;
		off += n;
		len -= n;
	}
	/* a segment that stays open between calls keeps its page pinned, never locked: a lock holds off interrupts and parallel workers */
	sift_sr_unlock(&seg->sr);
}

static const SiftSectionRef *
sec(const SiftColSegment *seg, SiftSection s)
{
	return &seg->hdr->sections[s];
}

SiftColSegment *
sift_cseg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSegment *seg = palloc0(sizeof(SiftColSegment));
	SegPriv    *pv = palloc0(sizeof(SegPriv));
	Buffer		buf;
	Page		page;
	SiftSegHeaderData *h;
	uint32		used;
	uint64		size,
				rank = 0;
	char	   *schema;
	const SiftSectionRef *s;

	seg->index = index;
	seg->entry = *entry;
	seg->mcxt = mcxt;
	seg->priv = pv;
	if (entry->kind != SIFT_SEG_IMMUTABLE)
		elog(ERROR, "sift_col: segment %llu is not immutable", ULL(entry->gen));
	if (entry->format != SIFT_COL_FORMAT_MINOR)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" has a segment of column format 1.%d, this build reads 1.%d",
						RelationGetRelationName(index), entry->format, SIFT_COL_FORMAT_MINOR),
				 errhint(entry->format > SIFT_COL_FORMAT_MINOR ? "Use a newer pg_sift, or REINDEX." : "REINDEX; segments of a pre-release column format are not read.")));
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
	if (h->gen != entry->gen || h->ndocs != entry->ndocs || h->ndocs == 0 || h->sb_lo != entry->sb_lo ||
		h->sb_hi != entry->sb_hi || h->sb_lo > h->sb_hi || h->live_npages == 0 || h->dictroot_n == 0 ||
		h->dictroot_n > h->sb_hi - h->sb_lo + 1 || h->nterms == 0 || h->nterms > SIFT_COL_MAX_COLS)
		corrupt(seg, "Header (gen %llu, %llu rows, SBs %u-%u, %u units) does not match its directory entry.",
				ULL(h->gen), ULL(h->ndocs), h->sb_lo, h->sb_hi, h->dictroot_n);
	sift_sr_init(&seg->sr, index, h->extents, h->nextents, entry->gen);
	size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;
	s = h->sections;
	if (s[SIFT_SEC_POSTINGS].off != 0 || s[SIFT_SEC_DICT].off != s[SIFT_SEC_POSTINGS].len ||
		s[SIFT_SEC_DICTIDX].off != s[SIFT_SEC_DICT].off + s[SIFT_SEC_DICT].len ||
		s[SIFT_SEC_DOCMAP].off != s[SIFT_SEC_DICTIDX].off + s[SIFT_SEC_DICTIDX].len ||
		s[SIFT_SEC_DOCMAP].off + s[SIFT_SEC_DOCMAP].len > size ||
		h->dictroot_off + (uint64) h->dictroot_n * sizeof(SiftColSBRef) > s[SIFT_SEC_DOCMAP].len)
		corrupt(seg, "Sections do not tile the %llu-byte stream.", ULL(size));
	schema = palloc(h->dictroot_off);
	seg_read(seg, s[SIFT_SEC_DOCMAP].off, h->dictroot_off, schema);
	seg->schema = sift_cschema_parse(mcxt, schema, h->dictroot_off);
	if (seg->schema == NULL || seg->schema->ncols != (int) h->nterms)
		corrupt(seg, "The schema section is not a schema of %llu leaves.", ULL(h->nterms));
	pfree(schema);
	seg->nsb = h->dictroot_n;
	{
		SiftColSBRef *dir = palloc((Size) seg->nsb * sizeof(SiftColSBRef));

		seg_read(seg, s[SIFT_SEC_DOCMAP].off + ALIGN8(h->dictroot_off), (Size) seg->nsb * sizeof(SiftColSBRef), dir);
		for (uint32 i = 0; i < seg->nsb; i++)
		{
			const SiftColSBRef *r = &dir[i];

			if (r->sb < h->sb_lo || r->sb > h->sb_hi || (i > 0 && r->sb <= dir[i - 1].sb) || r->off % 8 != 0 ||
				r->nrows == 0 || r->nrows > SIFT_COL_UNIT_MAX_ROWS || r->rank_base != rank ||
				r->off + r->len > s[SIFT_SEC_POSTINGS].len || (i > 0 && r->off < dir[i - 1].off + dir[i - 1].len))
				corrupt(seg, "SBDIR entry %u (SB %u at %llu, %u rows, rank %llu) is out of order.", i, r->sb,
						ULL(r->off), r->nrows, ULL(r->rank_base));
			rank += r->nrows;
		}
		if (rank != h->ndocs)
			corrupt(seg, "SBDIR lists %llu rows, the header %llu.", ULL(rank), ULL(h->ndocs));
		seg->sbdir = dir;
	}
	pv->live = (SiftLive) {index, entry->gen, h->live_first, h->live_npages, h->sb_hi - h->sb_lo + 1, h->ndocs};
	sift_live_check(&pv->live);
	pv->dict = palloc0(h->nterms * sizeof(SiftCDict *));
	pv->zones = palloc0(h->nterms * sizeof(ZoneArrays *));
	MemoryContextSwitchTo(old);
	return seg;
}

void
sift_cseg_close(SiftColSegment *seg)
{
	sift_sr_release(&seg->sr);
}

uint32
sift_cseg_find_unit(const SiftColSegment *seg, uint32 sb)
{
	uint32		lo = 0,
				hi = seg->nsb;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (seg->sbdir[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

const uint8 *
sift_cseg_unit(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	if (r->len > pv->unitcap)
	{
		pv->unitcap = Max(r->len, 2 * pv->unitcap);
		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, pv->unitcap) : MemoryContextAlloc(seg->mcxt, pv->unitcap);
	}
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, r->len, pv->unitbuf);
	*len = r->len;
	return pv->unitbuf;
}

const uint8 *
sift_cseg_unit_head(SiftColSegment *seg, uint32 u, uint32 *len, uint32 *avail)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	if (r->len > pv->unitcap)
	{
		pv->unitcap = Max(r->len, 2 * pv->unitcap);
		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, pv->unitcap) : MemoryContextAlloc(seg->mcxt, pv->unitcap);
	}
	*len = r->len;
	*avail = Min(r->len, SIFT_COL_UNIT_HEAD);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, *avail, pv->unitbuf);
	return pv->unitbuf;
}

void
sift_cseg_unit_fill(SiftColSegment *seg, uint32 u, uint32 off, uint32 len)
{
	SegPriv    *pv = PRIV(seg);

	Assert((uint64) off + len <= seg->sbdir[u].len);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off + off, len, pv->unitbuf + off);
}

SiftCDict *
sift_cseg_dict(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
	{
		uint64		off;
		uint32		hdr[2];
		uint64		next = s->len,
					tab[SIFT_COL_MAX_COLS];
		char	   *data;
		Size		used;

		seg_read(seg, s->off, sizeof(hdr), hdr);
		if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
			corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
		seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
		off = tab[col];
		for (int c = 0; c < seg->schema->ncols; c++)
			if (tab[c] > off && tab[c] < next)
				next = tab[c];
		if (off == 0 || off >= s->len)
			corrupt(seg, "Leaf %d has no dictionary.", col);
		data = palloc(next - off);
		seg_read(seg, s->off + off, next - off, data);
		pv->dict[col] = sift_cdict_parse(seg->mcxt, data, next - off, &used);
		if (pv->dict[col] == NULL)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		pfree(data);
	}
	return pv->dict[col];
}

static const ZoneArrays *
zone_arrays(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);

	if (pv->zones[col] == NULL)
	{
		const SiftColDef *def = &seg->schema->cols[col];
		uint32		hdr[2];
		uint64		tab[SIFT_COL_MAX_COLS],
					off;
		Size		sz = zone_block_size(def, seg->nsb);
		char	   *blk;
		ZoneArrays *z = MemoryContextAllocZero(seg->mcxt, sizeof(ZoneArrays));

		seg_read(seg, s->off, sizeof(hdr), hdr);
		if (hdr[0] != ZONES_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
			corrupt(seg, "ZONES section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
		seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
		off = tab[col];
		if (off % 8 != 0 || off + sz > s->len)
			corrupt(seg, "The zones of leaf %d (%llu, %zu bytes) lie outside the section.", col, ULL(off), sz);
		blk = MemoryContextAlloc(seg->mcxt, sz);
		seg_read(seg, s->off + off, sz, blk);
		z->min = (const int64 *) blk;
		z->max = z->min + seg->nsb;
		z->sum = z->max + seg->nsb;
		z->nnull = (const uint32 *) (z->sum + seg->nsb);
		z->nvals = z->nnull + seg->nsb;
		z->flags = z->nvals + seg->nsb;
		if (def->store == SIFT_COLS_STR)
			z->set = (const uint64 (*)[4]) (blk + ALIGN8(seg->nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))));
		for (uint32 u = 0; u < seg->nsb; u++)
			z->any_ovf |= (z->flags[u] & SIFT_COLZ_OVERFLOW) != 0, z->any_inl |= (z->flags[u] & SIFT_COLZ_INLINE) != 0;
		pv->zones[col] = z;
	}
	return pv->zones[col];
}

void
sift_cseg_zone(SiftColSegment *seg, int col, uint32 u, SiftCZone *out)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	const SiftColDef *def = &seg->schema->cols[col];

	memset(out, 0, sizeof(SiftCZone));
	out->min = z->min[u];
	out->max = z->max[u];
	if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
		memcpy(&out->sum_f, &z->sum[u], sizeof(double));
	else
		out->sum_i = z->sum[u];
	out->nnull = z->nnull[u];
	out->nvals = z->nvals[u];
	out->flags = z->flags[u];
	if (z->set)
		memcpy(out->set, z->set[u], 4 * sizeof(uint64));
}

void
sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	uint32		n = seg->nsb,
				nw = SIFT_COL_NWORDS(n);
	uint64	   *tmp = palloc(nw * sizeof(uint64));
	uint64	   *save = NULL;

	if (z->any_ovf || z->any_inl)
	{
		save = palloc(nw * sizeof(uint64));
		memcpy(save, keep, nw * sizeof(uint64));
	}
	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(keep, 0, nw * sizeof(uint64));
			break;
		case SIFT_CT_SET:			/* lo..hi bound the keys that can match */
		case SIFT_CT_RANGE:
			sift_colk->zone_range(z->min, z->max, n, t->lo, t->hi, tmp);
			sift_colk->bits_and(keep, tmp, nw);
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set, n, t->fold, keep);
			break;
		case SIFT_CT_LUT:
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set, n, t->fold, keep);
			break;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
		case SIFT_CT_NSET:
		case SIFT_CT_STRS:
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			break;
		case SIFT_CT_ISNULL:
			for (uint32 u = 0; u < n; u++)
				if (z->nnull[u] == 0)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
		case SIFT_CT_NRANGE:
			for (uint32 u = 0; u < n; u++)
				if (z->nnull[u] < z->nvals[u] && z->min[u] >= t->lo && z->max[u] <= t->hi)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
				else if (z->nnull[u] == z->nvals[u])
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
	}
	if (save)
	{
		/* an inline unit has no codes: the passes above said nothing about it, its zone is the strings' */
		for (uint32 u = 0; z->any_inl && u < n; u++)
			if (z->flags[u] & SIFT_COLZ_INLINE)
			{
				SiftCZone	uz = {.min = z->min[u], .max = z->max[u], .nnull = z->nnull[u], .nvals = z->nvals[u], .flags = z->flags[u]};

				if (sift_ctest_zone(t, &uz))
					keep[u / 64] |= save[u / 64] & (UINT64CONST(1) << (u % 64));
				else
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			}
		/* units with overflow values cannot be pruned by this leaf: their zone does not describe the heap */
		for (uint32 u = 0; z->any_ovf && u < n; u++)
			if (z->flags[u] & SIFT_COLZ_OVERFLOW)
				keep[u / 64] |= save[u / 64] & (UINT64CONST(1) << (u % 64));
		pfree(save);
	}
	pfree(tmp);
}

/* The superblocks the immutable segments of dir cover: the ranges of the segments of a parallel build interleave, so they are not summed */
uint64
sift_cseg_dir_units(const SiftDirSnapshot *dir)
{
	const SiftSegEntry *imm[SIFT_MAX_SEGMENTS];
	int			n = 0;
	uint64		units = 0,
				end = 0;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			int			j = n++;

			for (; j > 0 && imm[j - 1]->sb_lo > dir->segs[i].sb_lo; j--)
				imm[j] = imm[j - 1];
			imm[j] = &dir->segs[i];
		}
	for (int i = 0; i < n; i++)
	{
		uint64		lo = Max(imm[i]->sb_lo, end),
					hi = (uint64) imm[i]->sb_hi + 1;

		if (hi > lo)
		{
			units += hi - lo;
			end = hi;
		}
	}
	return units;
}

/* ---------- liveness ---------- */

static bool
unit_dirty(SiftColSegment *seg, uint32 u)
{
	SegPriv    *pv = PRIV(seg);
	uint32		i = seg->sbdir[u].sb - seg->hdr->sb_lo;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return false;
	if (pv->dirty == NULL)
	{
		pv->dirty = MemoryContextAlloc(seg->mcxt, (pv->live.nsb + 7) / 8);
		sift_live_dirty(&pv->live, pv->dirty);
	}
	return (pv->dirty[i / 8] >> (i % 8)) & 1;
}

bool
sift_cseg_dirty(SiftColSegment *seg, uint32 u)
{
	return unit_dirty(seg, u);
}

void
sift_cseg_live(SiftColSegment *seg, uint32 u, uint64 *bits)
{
	uint32		n = seg->sbdir[u].nrows,
				nw = SIFT_COL_NWORDS(n);

	if (unit_dirty(seg, u))
	{
		sift_live_bits(&PRIV(seg)->live, seg->sbdir[u].rank_base, n, bits, nw);
		return;
	}
	memset(bits, 0, nw * sizeof(uint64));
	memset(bits, 0xFF, n / 64 * sizeof(uint64));
	if (n % 64)
		bits[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
}

uint64
sift_cseg_live_rows(SiftColSegment *seg)
{
	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return seg->hdr->ndocs;
	return seg->hdr->ndocs - sift_live_ndead(&PRIV(seg)->live);
}

/* ---------- decoding for rewrites and verification ---------- */

void
sift_cseg_decode_unit(SiftColSegment *seg, uint32 u, SiftColBatch *dst)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	SiftCDict **dicts = palloc0(seg->schema->ncols * sizeof(SiftCDict *));
	uint64	   *live = palloc(SIFT_COL_NWORDS(r->nrows) * sizeof(uint64));

	if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->sb);
	for (int c = 0; c < seg->schema->ncols; c++)
		dicts[c] = sift_cseg_dict(seg, c);
	sift_cseg_live(seg, u, live);
	sift_cbatch_reset(dst, r->sb);
	sift_cunit_decode(&v, r->sb, dicts, unit_dirty(seg, u) ? live : NULL, dst);
	pfree(dicts);
	pfree(live);
}

/* ---------- VACUUM ---------- */

static void
set_dirty(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == *(uint64 *) arg)
			dir->segs[i].flags |= SIFT_SEG_DIRTY;
}

int64
sift_cseg_bulkdelete(SiftColSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	SegPriv    *pv = PRIV(seg);
	uint64	   *dead = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint64));
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	int64		removed = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const SiftColSBRef *r = &seg->sbdir[u];
		SiftCUnitView v;
		const uint8 *rec;
		uint32		len;
		int			nd = 0;

		rec = sift_cseg_unit(seg, u, &len);
		if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != r->nrows)
			corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->sb);
		sift_cunit_keys(&v, r->sb, keys);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < r->nrows; i++)
		{
			ItemPointerData tid;

			if (!(live[i / 64] >> (i % 64) & 1))
				continue;
			sift_key_to_tid(keys[i], &tid);
			if (cb(&tid, cbstate))
				dead[nd++] = r->rank_base + i;
			else
				stats->num_index_tuples++;
		}
		if (nd == 0)
			continue;
		if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		{
			sift_dir_update(seg->index, &(SiftDirChange) {.fn = set_dirty,.arg = &seg->entry.gen,.extra = InvalidBuffer});
			seg->entry.flags |= SIFT_SEG_DIRTY;
		}
		sift_live_clear(&pv->live, r->sb - seg->hdr->sb_lo, dead, nd);
		if (pv->dirty)
			pv->dirty[(r->sb - seg->hdr->sb_lo) / 8] |= 1 << ((r->sb - seg->hdr->sb_lo) % 8);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	pfree(dead);
	pfree(keys);
	pfree(live);
	stats->tuples_removed += removed;
	return removed;
}

/* ---------- verification ---------- */

static bool
zone_equal(const SiftColDef *def, const SiftCZone *a, const SiftCZone *b)
{
	bool		fl = def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8;

	if (a->nvals != b->nvals || a->nnull != b->nnull || a->flags != b->flags)
		return false;
	if (a->nnull == a->nvals)
		return true;
	if (a->min != b->min || a->max != b->max)
		return false;
	if (!fl && !(a->flags & SIFT_COLZ_NOSUM) && a->sum_i != b->sum_i)
		return false;
	if (fl && memcmp(&a->sum_f, &b->sum_f, sizeof(double)) != 0)
		return false;
	return def->store != SIFT_COLS_STR || memcmp(a->set, b->set, sizeof(a->set)) == 0;
}

/* The unit checks of verify: structure, chunk sizes, dictionary codes, ascending keys inside the SB, zones */
static void
verify_unit(SiftColSegment *seg, uint32 u, SiftKey *keys)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	const SiftColSchema *sc = seg->schema;
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	uint64		group_elems[SIFT_COL_MAX_COLS] = {0};

	if (!sift_cunit_parse(rec, len, sc, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse or lists another row count.", u, r->sb);
	sift_cunit_keys(&v, r->sb, keys);
	for (uint32 i = 0; i < r->nrows; i++)
		if (sift_block_sb(sift_key_block(keys[i])) != r->sb || (i > 0 && keys[i] <= keys[i - 1]) ||
			(keys[i] & 0xFFFF) == 0)
			corrupt(seg, "Row %u of the unit of SB %u has key %llx out of order or outside the SB.", i, r->sb, ULL(keys[i]));
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		SiftCChunk	ch;
		SiftCZone	stored,
					computed;
		int64	   *vals;

		if (!sift_cunit_chunk(&v, c, &ch))
			corrupt(seg, "Unit %u has no chunk for leaf %d.", u, c);
		if (ch.n != (def->parent < 0 ? r->nrows : group_elems[def->parent]))
			corrupt(seg, "Chunk of leaf %d in the unit of SB %u holds %u values, expected %llu.", c, r->sb, ch.n,
					ULL(def->parent < 0 ? r->nrows : group_elems[def->parent]));
		vals = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
		sift_cchunk_decode(&ch, vals);
		sift_cchunk_zone(&ch, def->store, &computed);
		sift_cseg_zone(seg, c, u, &stored);
		if (!zone_equal(def, &stored, &computed))
			corrupt(seg, "The zone of leaf %d in the unit of SB %u differs from its chunk.", c, r->sb);
		if (def->kind == SIFT_COLK_GROUP)
			group_elems[c] = computed.sum_i;
		if (def->store == SIFT_COLS_STR && ch.enc != SIFT_COLENC_ISTR)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			for (uint32 i = 0; i < ch.n; i++)
				if (!(ch.nulls && (ch.nulls[i / 64] >> (i % 64) & 1)) && (uint64) vals[i] >= d->n)
					corrupt(seg, "Leaf %d of the unit of SB %u holds code %lld of a dictionary of %u.", c, r->sb,
							(long long) vals[i], d->n);
		}
		pfree(vals);
	}
}

uint64
sift_cseg_verify(SiftColSegment *seg)
{
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64		live = seg->hdr->ndocs;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		verify_unit(seg, u, keys);
		CHECK_FOR_INTERRUPTS();
	}
	if (seg->entry.flags & SIFT_SEG_DIRTY)
		live -= sift_live_ndead(&PRIV(seg)->live);
	sift_sr_release(&seg->sr);
	pfree(keys);
	return live;
}
