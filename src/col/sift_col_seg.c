/*
 * sift_col_seg.c - immutable column segments: writer and reader over the page store (sift_col_seg.h, docs/COL.md 2.1, 2.3, 2.4, 2.7).
 *
 * Stream: UNITS (the unit records, 8-byte aligned), DICTS (a directory, then the serialized dictionary of each STR leaf), ZONES (a
 * directory, then per leaf the zone arrays of all units), then SCHEMA and SBDIR. The header page is a SiftSegHeaderData whose sections
 * are, in order of the stream: POSTINGS = UNITS, DICT = DICTS, DICTIDX = ZONES, DOCMAP = SCHEMA + SBDIR (dictroot_off = the schema's bytes,
 * dictroot_n = the units).
 */
#include "sift_host.h"

#include "sift_col_preagg.h"
#include "sift_col_seg.h"
#include "sift_live.h"

#define ULL(x)			((unsigned long long) (x))
#define ALIGN8(x)		TYPEALIGN(8, x)
#define DICTS_MAGIC		0x44444F43		/* "COLD" */
#define ZONES_MAGIC		0x5A4C4F43		/* "COLZ" */
#define PAGG_MAGIC		0x504C4F43		/* "COLP": the pre-aggregates, at the end of the ZONES section (docs/COL-PREAGG.md section 2) */

#define corrupt(seg, ...) SIFT_SEG_CORRUPT((seg)->index, (seg)->entry.gen, __VA_ARGS__)

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
	SiftCPaggSpec pagg;			/* the pre-aggregates to write (docs/COL-PREAGG.md) */
	SiftCLeafAgg *lagg;			/* [unit * ncols + col] exact sums of the INT leaves */
	uint64	   *cubeoff;		/* [unit]: offset of the unit's cube blob in cube; [nsb] the end */
	StringInfoData cube;		/* the cube blobs of all units */
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
	w->lagg = palloc(w->capsb * schema->ncols * sizeof(SiftCLeafAgg));
	w->cubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	sift_cpagg_spec_default(schema, &w->pagg);
	initStringInfo(&w->cube);
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
sift_colw_set_preagg(SiftColSegWriter *w, const SiftCPaggSpec *spec)
{
	w->pagg = *spec;
}

/*
 * The pre-aggregates of the unit just encoded (docs/COL-PREAGG.md): the exact sums of its INT leaves and the cube blob, from the batch's
 * columns with the dictionary leaves in the segment's codes (what a reader of the unit sees).
 */
static void
add_preagg(SiftColSegWriter *w, const SiftColBatch *b, Size unit_bytes)
{
	int			ncols = w->schema->ncols;
	SiftCPaggCol *cols = palloc(ncols * sizeof(SiftCPaggCol));
	bool	   *dim = palloc0(ncols * sizeof(bool));
	int64	  **tk = palloc0(ncols * sizeof(int64 *));
	SiftCLeafAgg *la = w->lagg + (Size) w->nsb * ncols;

	for (int d = 0; d < w->pagg.ndims; d++)
		dim[w->pagg.dim[d].leaf] = true;
	for (int c = 0; c < ncols; c++)
	{
		const SiftCVals *v = &b->col[c];
		const SiftColDef *def = &w->schema->cols[c];

		cols[c] = (SiftCPaggCol) {v->key, v->nulls, v->ovf, v->n, def->store != SIFT_COLS_VAR && !w->inl[c]};
		if (def->store == SIFT_COLS_STR && dim[c] && !w->inl[c])
		{
			tk[c] = palloc(Max(v->n, 1) * sizeof(int64));
			for (uint32 i = 0; i < v->n; i++)
				tk[c][i] = (v->nulls && (v->nulls[i / 64] >> (i % 64) & 1)) ? 0 : w->dictmap[c][v->key[i]];
			cols[c].key = tk[c];
		}
		if (def->store == SIFT_COLS_INT)
			sift_cpagg_leaf(&cols[c], &la[c]);
	}
	w->cubeoff[w->nsb] = w->cube.len;
	sift_cpagg_cube(w->schema, &w->pagg, cols, b->nrows, unit_bytes, &w->cube);
	w->cubeoff[w->nsb + 1] = w->cube.len;
	for (int c = 0; c < ncols; c++)
		if (tk[c])
			pfree(tk[c]);
	pfree(tk);
	pfree(dim);
	pfree(cols);
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
		w->lagg = repalloc_huge(w->lagg, (Size) w->capsb * ncols * sizeof(SiftCLeafAgg));
		w->cubeoff = repalloc_huge(w->cubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->sbdir = repalloc_huge(w->sbdir, w->capsb * sizeof(SiftColSBRef));
	}
	for (int c = 0; c < ncols; c++)
		w->inl[c] = w->dict[c] != NULL && sift_cbatch_str_inline(b, c, w->dict[c], SIFT_COL_DICT_MAX_BYTES);
	map_dicts(w, b, w->inl);
	resetStringInfo(&w->buf);
	sift_cunit_encode(b, w->dictmap, w->inl, w->pol, &w->buf, &start, w->zones + (Size) w->nsb * ncols);
	add_preagg(w, b, w->buf.len - start);
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
	Size		m = (Size) w->nsb * (w->schema->ncols * (sizeof(SiftCZone) + sizeof(SiftCLeafAgg)) + sizeof(SiftColSBRef) + sizeof(uint64)) + w->cube.len;

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
		sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, &blob);
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

/*
 * The pre-aggregates, at the end of the ZONES section (docs/COL-PREAGG.md section 2): a header (magic, version, ncols, nsb, the offset of the
 * cube directory and its blob bytes, the offset of each INT leaf's sums), then per INT leaf the sums of all units as arrays, then the cube
 * directory (u64 offset of each unit's blob, nsb + 1 entries) and the blobs. Offsets are from the start of the header.
 */
typedef struct PaggHeader
{
	uint32		magic;
	uint32		version;
	uint32		ncols;
	uint32		nsb;
	uint64		cubedir_off;			/* 0: no cube in any unit */
	uint64		cube_bytes;
	uint64		lagg_off[FLEXIBLE_ARRAY_MEMBER];	/* [ncols], 0: the leaf has none */
} PaggHeader;

#define PAGG_HDR_SIZE(ncols)	ALIGN8(offsetof(PaggHeader, lagg_off) + (ncols) * sizeof(uint64))

/* One INT leaf's sums: sum_lo, sum_hi, sumsq_lo, sumsq_hi (u64 each), flags (u32), all over the units */
static Size
lagg_block_size(uint32 nsb)
{
	return ALIGN8((Size) nsb * (4 * sizeof(uint64) + sizeof(uint32)));
}

static void
write_pagg(SiftColSegWriter *w)
{
	int			ncols = w->schema->ncols;
	uint32		nsb = w->nsb;
	Size		hsz = PAGG_HDR_SIZE(ncols),
				at = hsz;
	PaggHeader *h = palloc0(hsz);

	h->magic = PAGG_MAGIC;
	h->version = 1;
	h->ncols = ncols;
	h->nsb = nsb;
	for (int c = 0; c < ncols; c++)
		if (w->schema->cols[c].store == SIFT_COLS_INT)
		{
			h->lagg_off[c] = at;
			at += lagg_block_size(nsb);
		}
	if (w->cube.len > 0)
	{
		h->cubedir_off = at;
		h->cube_bytes = w->cube.len;
	}
	sift_sw_write(w->sw, h, hsz);
	for (int c = 0; c < ncols; c++)
	{
		uint64	   *blk;
		uint64	   *slo,
				   *shi,
				   *qlo,
				   *qhi;
		uint32	   *fl;

		if (!h->lagg_off[c])
			continue;
		blk = palloc0(lagg_block_size(nsb));
		slo = blk;
		shi = slo + nsb;
		qlo = shi + nsb;
		qhi = qlo + nsb;
		fl = (uint32 *) (qhi + nsb);
		for (uint32 u = 0; u < nsb; u++)
		{
			const SiftCLeafAgg *a = &w->lagg[(Size) u * ncols + c];

			slo[u] = (uint64) a->sum;
			shi[u] = (uint64) (a->sum >> 64);
			qlo[u] = (uint64) a->sumsq;
			qhi[u] = (uint64) (a->sumsq >> 64);
			fl[u] = a->flags;
		}
		sift_sw_write(w->sw, blk, lagg_block_size(nsb));
		pfree(blk);
	}
	if (w->cube.len > 0)
	{
		sift_sw_write(w->sw, w->cubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->cube.data, w->cube.len);
	}
	pfree(h);
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
	write_pagg(w);
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
	SiftCDict **dict;
	ZoneArrays **zones;
	uint8	   *unitbuf;
	Size		unitcap;
	PaggHeader *pagg;			/* the sidecar's header, read on first use */
	Size		pagg_end;		/* offset of the sidecar in the ZONES section */
	uint64	   *cubedir;		/* nsb + 1 */
	uint8	   *cubebuf;
	Size		cubecap;
	const uint64 **lagg;		/* [ncols] sums of the INT leaves, read on first use: slo, shi, qlo, qhi, then flags */
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
	SiftSegHeaderData *h;
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
	seg->hdr = sift_seg_read_header(index, entry);
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

/* Where the dictionary of leaf col lies in the stream: offset and length (up to the next dictionary or the end of the section) */
static void
dict_region(SiftColSegment *seg, int col, uint64 *off, uint64 *len)
{
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2];
	uint64		next = s->len,
				tab[SIFT_COL_MAX_COLS];

	seg_read(seg, s->off, sizeof(hdr), hdr);
	if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
		corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
	seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
	*off = tab[col];
	for (int c = 0; c < seg->schema->ncols; c++)
		if (tab[c] > *off && tab[c] < next)
			next = tab[c];
	if (*off == 0 || *off >= s->len)
		corrupt(seg, "Leaf %d has no dictionary.", col);
	*len = next - *off;
}

uint64
sift_cseg_dict_stored(SiftColSegment *seg, int col)
{
	uint64		off,
				len;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return 0;
	dict_region(seg, col, &off, &len);
	return len;
}

SiftCDict *
sift_cseg_dict(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
	{
		uint64		off,
					len;
		char	   *data;
		Size		used;

		dict_region(seg, col, &off, &len);
		data = palloc(len);
		seg_read(seg, sec(seg, SIFT_SEC_DICT)->off + off, len, data);
		pv->dict[col] = sift_cdict_parse(seg->mcxt, data, len, &used);
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

/* The entries of the segment's dictionary of leaf col: the count in its header (after the form), without parsing the dictionary */
static uint32
dict_count(SiftColSegment *seg, int col)
{
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2],
				n;
	uint64		tab[SIFT_COL_MAX_COLS];

	seg_read(seg, s->off, sizeof(hdr), hdr);
	if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
		corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
	seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
	if (tab[col] == 0 || tab[col] + 2 * sizeof(n) > s->len)
		corrupt(seg, "Leaf %d has no dictionary.", col);
	seg_read(seg, s->off + tab[col] + sizeof(n), sizeof(n), &n);
	return n;
}

void
sift_col_leaf_stats_init(SiftColLeafStats *acc)
{
	memset(acc, 0, sizeof(*acc));
	acc->min = PG_INT64_MAX;
	acc->max = PG_INT64_MIN;
	acc->has_range = acc->has_sum = true;
}

void
sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc)
{
	const SiftColDef *def = &seg->schema->cols[col];
	const ZoneArrays *z = zone_arrays(seg, col);

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		bool		any = z->nnull[u] < z->nvals[u];

		acc->nvals += z->nvals[u];
		acc->nnull += z->nnull[u];
		if (z->flags[u] & SIFT_COLZ_OVERFLOW)
		{
			acc->has_range = acc->has_sum = false;
			acc->has_ovf = true;
		}
		if (def->kind == SIFT_COLK_GROUP)
			acc->has_sum &= !(z->flags[u] & SIFT_COLZ_NOSUM);
		else if (def->store == SIFT_COLS_INT && any)
		{
			acc->min = Min(acc->min, z->min[u]);
			acc->max = Max(acc->max, z->max[u]);
		}
		else if (def->store != SIFT_COLS_INT)
			acc->has_range = false;
		if (def->kind == SIFT_COLK_GROUP)
			acc->sum += (double) z->sum[u];
	}
	if (def->store == SIFT_COLS_STR)
	{
		acc->inl |= z->any_inl;
		acc->dict_n = Max(acc->dict_n, dict_count(seg, col));
	}
}

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* Where the sidecar starts: after the zone blocks, which are laid out back to back */
static Size
zones_end(const SiftColSegment *seg)
{
	Size		at = ALIGN8(sizeof(uint32) * 2 + seg->schema->ncols * sizeof(uint64));

	for (int c = 0; c < seg->schema->ncols; c++)
		at += zone_block_size(&seg->schema->cols[c], seg->nsb);
	return at;
}

static const PaggHeader *
pagg_header(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->pagg == NULL)
	{
		const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);
		int			ncols = seg->schema->ncols;
		Size		end = zones_end(seg),
					hsz = PAGG_HDR_SIZE(ncols);
		PaggHeader *h;

		if (end + hsz > s->len)
			corrupt(seg, "The ZONES section holds %llu bytes, no room for the pre-aggregates after %zu.", ULL(s->len), end);
		h = MemoryContextAlloc(seg->mcxt, hsz);
		seg_read(seg, s->off + end, hsz, h);
		if (h->magic != PAGG_MAGIC || h->version != 1 || h->ncols != (uint32) ncols || h->nsb != seg->nsb)
			corrupt(seg, "The pre-aggregates start with 0x%08x, version %u, %u leaves, %u units.", h->magic, h->version, h->ncols, h->nsb);
		for (int c = 0; c < ncols; c++)
			if (h->lagg_off[c] % 8 != 0 || (h->lagg_off[c] && end + h->lagg_off[c] + lagg_block_size(seg->nsb) > s->len))
				corrupt(seg, "The sums of leaf %d lie outside the section.", c);
		if (h->cubedir_off && (h->cubedir_off % 8 != 0 || end + h->cubedir_off + (seg->nsb + 1) * sizeof(uint64) + h->cube_bytes > s->len))
			corrupt(seg, "The cubes (%llu bytes at %llu) lie outside the section.", ULL(h->cube_bytes), ULL(h->cubedir_off));
		pv->pagg_end = end;
		pv->pagg = h;
		pv->lagg = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(uint64 *));
	}
	return pv->pagg;
}

void
sift_cseg_agg(SiftColSegment *seg, int col, uint32 u, SiftCAgg *a)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);
	SiftCZone	z;

	sift_cseg_zone(seg, col, u, &z);
	memset(a, 0, sizeof(*a));
	a->nvals = z.nvals;
	a->nnull = z.nnull;
	a->min = z.min;
	a->max = z.max;
	a->sum_f = z.sum_f;
	a->zflags = z.flags;
	if (h->lagg_off[col])
	{
		const uint64 *b = pv->lagg[col];

		if (b == NULL)
		{
			uint64	   *blk = MemoryContextAlloc(seg->mcxt, lagg_block_size(seg->nsb));

			seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + h->lagg_off[col], lagg_block_size(seg->nsb), blk);
			pv->lagg[col] = b = blk;
		}
		a->sum = (SiftCI128) (((unsigned __int128) b[seg->nsb + u] << 64) | b[u]);
		a->sumsq = (SiftCI128) (((unsigned __int128) b[3 * seg->nsb + u] << 64) | b[2 * seg->nsb + u]);
		a->flags = ((const uint32 *) (b + 4 * seg->nsb))[u];
		a->has_sums = true;
	}
}

const uint8 *
sift_cseg_cube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);
	uint64		b0,
				b1;

	*len = 0;
	if (!h->cubedir_off)
		return NULL;
	if (pv->cubedir == NULL)
	{
		pv->cubedir = MemoryContextAlloc(seg->mcxt, (seg->nsb + 1) * sizeof(uint64));
		seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + h->cubedir_off, (seg->nsb + 1) * sizeof(uint64), pv->cubedir);
		for (uint32 i = 0; i < seg->nsb; i++)
			if (pv->cubedir[i] > pv->cubedir[i + 1] || pv->cubedir[i + 1] > h->cube_bytes)
				corrupt(seg, "The cube directory is out of order at unit %u.", i);
	}
	b0 = pv->cubedir[u];
	b1 = pv->cubedir[u + 1];
	if (b1 == b0)
		return NULL;
	if (b1 - b0 > pv->cubecap)
	{
		pv->cubecap = Max(b1 - b0, 2 * pv->cubecap);
		pv->cubebuf = pv->cubebuf ? repalloc(pv->cubebuf, pv->cubecap) : MemoryContextAlloc(seg->mcxt, pv->cubecap);
	}
	seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + h->cubedir_off + (seg->nsb + 1) * sizeof(uint64) + b0, b1 - b0, pv->cubebuf);
	*len = b1 - b0;
	return pv->cubebuf;
}

uint64
sift_cseg_preagg_bytes(SiftColSegment *seg)
{
	pagg_header(seg);
	return sec(seg, SIFT_SEC_DICTIDX)->len - PRIV(seg)->pagg_end;
}

uint32
sift_cseg_preagg_units(SiftColSegment *seg)
{
	uint32		n = 0,
				len;

	for (uint32 u = 0; u < seg->nsb; u++)
		if (sift_cseg_cube(seg, u, &len) != NULL)
			n++;
	return n;
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

/* The dirty bit of the unit's SB, read from the LIVE page now: neither the directory snapshot nor an earlier read may stand for it, because
 * a scan must read liveness after it reads the all-visible mask (docs/COL.md 2.7, "Order of reads") */
static bool
unit_dirty(SiftColSegment *seg, uint32 u)
{
	uint32		i = seg->sbdir[u].sb - seg->hdr->sb_lo;
	Buffer		buf = InvalidBuffer;
	uint8		b = sift_live_byte(&PRIV(seg)->live, sizeof(SiftLiveHeaderData) + i / 8, &buf);

	UnlockReleaseBuffer(buf);
	return (b >> (i % 8)) & 1;
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

static bool
vals_have_ovf(const SiftCVals *v)
{
	for (uint32 w = 0; v->ovf != NULL && w < SIFT_COL_NWORDS(v->n); w++)
		if (v->ovf[w])
			return true;
	return false;
}

bool
sift_cseg_stale_ovf(SiftColSegment *seg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col stale overflow", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColBatch *live = sift_cbatch_create(cx, seg->schema);
	bool		stale = false;

	for (uint32 u = 0; u < seg->nsb && !stale; u++)
	{
		bool		decoded = false;

		for (int c = 0; c < seg->schema->ncols && !stale; c++)
		{
			const ZoneArrays *z = zone_arrays(seg, c);

			if (!z->any_ovf || !(z->flags[u] & SIFT_COLZ_OVERFLOW) || !unit_dirty(seg, u))
				continue;
			if (!decoded)
				sift_cseg_decode_unit(seg, u, live);
			decoded = true;
			stale = !vals_have_ovf(&live->col[c]);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return stale;
}

/* ---------- VACUUM ---------- */

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
		sift_seg_mark_dirty(seg->index, &seg->entry);
		sift_live_clear(&pv->live, r->sb - seg->hdr->sb_lo, dead, nd);
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
	SiftCPaggCol pcol[SIFT_COL_MAX_COLS];
	int64	   *kept[SIFT_COL_MAX_COLS] = {NULL};
	bool		need[SIFT_COL_MAX_COLS] = {false};
	uint32		clen;
	const uint8 *cube = sift_cseg_cube(seg, u, &clen);

	if (!sift_cunit_parse(rec, len, sc, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse or lists another row count.", u, r->sb);
	if (cube)
	{
		MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift verify cube", ALLOCSET_SMALL_SIZES);
		SiftCHist	hist;

		if (!sift_cpagg_decode(sc, cube, clen, cx, &hist))
			corrupt(seg, "The cube of the unit of SB %u does not decode.", r->sb);
		for (int d = 0; d < hist.ndims; d++)
		{
			need[hist.dim[d].leaf] = true;
			for (int m = 0; m < hist.dim[d].nmeas; m++)
				need[hist.dim[d].meas[m]] = true;
		}
		MemoryContextDelete(cx);
	}
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
		pcol[c] = (SiftCPaggCol) {vals, ch.nulls, ch.ovf, ch.n, def->store != SIFT_COLS_VAR && ch.enc != SIFT_COLENC_ISTR};
		if (def->store == SIFT_COLS_INT)
		{
			SiftCLeafAgg la;
			SiftCAgg	sa;

			sift_cpagg_leaf(&pcol[c], &la);
			sift_cseg_agg(seg, c, u, &sa);
			if (sa.sum != la.sum || sa.sumsq != la.sumsq || sa.flags != la.flags)
				corrupt(seg, "The exact sums of leaf %d in the unit of SB %u differ from its chunk.", c, r->sb);
		}
		if (def->store == SIFT_COLS_STR && ch.enc != SIFT_COLENC_ISTR)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			for (uint32 i = 0; i < ch.n; i++)
				if (!(ch.nulls && (ch.nulls[i / 64] >> (i % 64) & 1)) && (uint64) vals[i] >= d->n)
					corrupt(seg, "Leaf %d of the unit of SB %u holds code %lld of a dictionary of %u.", c, r->sb,
							(long long) vals[i], d->n);
		}
		if (need[c])
			kept[c] = vals;
		else
			pfree(vals);
	}
	if (cube && !sift_cpagg_check(sc, pcol, r->nrows, cube, clen))
		corrupt(seg, "The cube of the unit of SB %u differs from its chunks.", r->sb);
	for (int c = 0; c < sc->ncols; c++)
		if (kept[c])
			pfree(kept[c]);
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
