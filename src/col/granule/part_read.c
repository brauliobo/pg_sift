/*
 * col/granule/part_read.c - the dictionaries and the granules of an opened part read back: the directory, the chunks of the columns wanted, the versions and the time strides.
 */
#include "core/host.h"

#include "access/transam.h"
#include "lib/stringinfo.h"

#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/part_int.h"
#include "col/f2/preagg_delta.h"
#include "core/page_io.h"

typedef struct DdictCb
{
	MemoryContextCallback cb;
	void	   *ddict;
} DdictCb;

static void
ddict_free(void *arg)
{
	sift_colz_zstd_free_ddict(((DdictCb *) arg)->ddict);
}

/* The strings of a dictionary of the part (SIFT_DICT_KIND_STR), decoded from their lists of SIFT_DICTS_CHUNK */
static void
read_str_dict(Relation rel, const SiftPart *part, MemoryContext cx, const char *sec, const SiftDictsEntry *e, SiftPartDict *d)
{
	uint64		at = e->off;
	uint32		total = 0;
	char	   *bytes = palloc(Max(e->len, 1) * 4);
	uint64		cap = Max(e->len, 1) * 4,
				used = 0;

	if (e->n == 0)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A dictionary of the part lies outside its section.");
	d->n = e->n;
	d->off = palloc((e->n + 1) * sizeof(uint32));
	d->off[0] = 0;
	for (uint32 ch = 0; ch < SIFT_DICT_ENTRY_NCHUNKS(e->nchunks); ch++)
	{
		uint32		hdr[2],
				   *coff;
		char	   *cbytes;

		if (at + 8 > e->off + e->len)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A dictionary of the part is cut short.");
		memcpy(hdr, sec + at, 8);
		if (at + 8 + hdr[0] > e->off + e->len || total + hdr[1] > e->n)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A chunk of a dictionary of the part does not fit it.");
		sift_cc_decode_strs(cx, (const uint8 *) sec + at + 8, hdr[0], hdr[1], hdr[1], 0, &coff, &cbytes);
		if (used + coff[hdr[1]] > cap)
		{
			cap = used + coff[hdr[1]];
			bytes = repalloc(bytes, cap);
		}
		memcpy(bytes + used, cbytes, coff[hdr[1]]);
		for (uint32 j = 1; j <= hdr[1]; j++)
			d->off[total + j] = used + coff[j];
		used += coff[hdr[1]];
		total += hdr[1];
		pfree(coff);
		pfree(cbytes);
		at += 8 + MAXALIGN(hdr[0]);
	}
	if (total != e->n)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A dictionary of the part holds %u strings, its table says %u.", total, e->n);
	d->bytes = bytes;
}

/* The table of symbols all the chunks of a column of the part are coded by (SIFT_DICT_KIND_FSST) */
static void
read_fsst_dict(Relation rel, const SiftPart *part, const char *sec, const SiftDictsEntry *e, SiftPartDict *d)
{
	d->fsst = palloc(sizeof(SiftFsst));
	if (e->len > UINT32_MAX || sift_fsst_read_table(d->fsst, (const uint8 *) sec + e->off, (uint32) e->len) == 0)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The table of symbols of a column of the part is damaged.");
	d->n = d->fsst->nsym;
}

/* The zstd dictionary the string bytes of all the chunks of a column of the part are compressed with (SIFT_DICT_KIND_ZSTD); its handle goes with the memory context of the dictionaries */
static void
read_zstd_dict(Relation rel, const SiftPart *part, MemoryContext cx, const char *sec, const SiftDictsEntry *e, SiftPartDict *d)
{
	DdictCb    *cb;

	if (e->len != e->n || e->len < SIFT_ZSTD_DICT_MIN_BYTES || e->len > SIFT_ZSTD_DICT_MAX_BYTES)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The zstd dictionary of a column of the part has a length of %llu.", (unsigned long long) e->len);
	d->bytes = memcpy(palloc(e->len), sec + e->off, e->len);
	d->n = e->n;
	d->zstd_ddict = sift_colz_zstd_create_ddict(d->bytes, e->len);
	if (d->zstd_ddict == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift part requires zstd dictionary support"),
						errhint("Install, on every node that reads the index, the pg_sift release that wrote it (built with zstd).")));
	cb = MemoryContextAlloc(cx, sizeof(DdictCb));
	cb->cb = (MemoryContextCallback) {.func = ddict_free,.arg = cb};
	cb->ddict = d->zstd_ddict;
	MemoryContextRegisterResetCallback(cx, &cb->cb);
}

const SiftPartDict *
sift_part_dict_find(const SiftPartDicts *ds, int col, SiftDictKind kind)
{
	for (int i = 0; ds != NULL && i < ds->n; i++)
		if (ds->d[i].col == col && ds->d[i].kind == kind)
			return &ds->d[i];
	return NULL;
}

const SiftPartDicts *
sift_part_dicts(Relation rel, const SiftPart *cpart)
{
	SiftPart   *part = (SiftPart *) cpart;

	uint64		len;
	char	   *sec;
	uint32		nd;
	const SiftDictsEntry *ent;
	SiftPartDicts *out;
	MemoryContext cx,
				old;

	if (part->dicts_read)
		return part->dicts;
	part->dicts_read = true;
	if (sift_part_section(part, SEC_DICTS) == NULL)
		return NULL;
	sec = sift_part_read_section(rel, part, SEC_DICTS, &len);
	cx = GetMemoryChunkContext(part);
	old = MemoryContextSwitchTo(cx);
	memcpy(&nd, sec, 4);
	if (len < 8 + (uint64) nd * sizeof(SiftDictsEntry))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The dictionaries of the part are shorter than their table.");
	ent = (const SiftDictsEntry *) (sec + 8);
	out = palloc0(sizeof(SiftPartDicts));
	out->n = nd;
	out->d = palloc0(Max(nd, 1) * sizeof(SiftPartDict));
	for (uint32 i = 0; i < nd; i++)
	{
		SiftPartDict *d = &out->d[i];

		if (ent[i].off + ent[i].len > len)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A dictionary of the part lies outside its section.");
		d->col = ent[i].col;
		d->kind = SIFT_DICT_ENTRY_KIND(ent[i].nchunks);
		switch (d->kind)
		{
			case SIFT_DICT_KIND_STR:
				read_str_dict(rel, part, cx, sec, &ent[i], d);
				break;
			case SIFT_DICT_KIND_FSST:
				read_fsst_dict(rel, part, sec, &ent[i], d);
				break;
			case SIFT_DICT_KIND_ZSTD:
				read_zstd_dict(rel, part, cx, sec, &ent[i], d);
				break;
			default:
				SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A dictionary of the part is of the kind %d, which this release does not know.", d->kind);
		}
	}
	MemoryContextSwitchTo(old);
	pfree(sec);
	part->dicts = out;
	return out;
}

SiftGranuleDesc *
sift_part_granule_dir(Relation rel, const SiftPart *part)
{
	uint64		len;
	SiftGranuleDesc *dir = sift_part_read_section(rel, part, SEC_GRANULE_DIR, &len);

	if (dir == NULL || len != (uint64) part->hdr.ngranules * sizeof(SiftGranuleDesc))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The granule directory of the part holds %llu bytes for %u granules.", (unsigned long long) len, part->hdr.ngranules);
	return dir;
}

SiftGranuleDesc
sift_part_read_gdesc(Relation rel, const SiftPart *part, uint32 g)
{
	const SiftSecDirEntry *ds = sift_part_section(part, SEC_GRANULE_DIR);
	SiftGranuleDesc d;

	if (ds == NULL || ds->len != (uint64) part->hdr.ngranules * sizeof(d) || g >= part->hdr.ngranules)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The granule directory of the part does not hold granule %u.", g);
	sift_part_read_bytes(rel, part, ds->off + (uint64) g * sizeof(d), sizeof(d), &d);
	return d;
}

char *
sift_part_read_granule_cols(Relation rel, const SiftPart *part, uint32 g, const bool *want, SiftGranuleDesc *desc)
{
	const SiftSecDirEntry *gs = sift_part_section(part, SEC_GRANULES);
	SiftGranuleDesc d = sift_part_read_gdesc(rel, part, g);
	uint32		nchunks = part->hdr.ncols + 1,
				hdrlen = offsetof(SiftGranuleHeader, chunks) + nchunks * sizeof(SiftChunkRef);
	const SiftGranuleHeader *h;
	char	   *rec;

	if (gs == NULL || d.data_off + d.data_len > gs->len || hdrlen > d.data_len)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u of the part is outside its section.", g);
	/* check-memory: allow one granule record, bounded by its rows */
	rec = palloc(d.data_len);
	sift_part_read_bytes(rel, part, gs->off + d.data_off, want == NULL ? d.data_len : hdrlen, rec);
	h = (const SiftGranuleHeader *) rec;
	if (want != NULL)
	{
		SiftByteRange *ranges = palloc(nchunks * sizeof(SiftByteRange));
		int			nr = 0;

		if (h->magic != SIFT_GRANULE_MAGIC || h->nchunks != nchunks)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u of the part does not start with its header.", g);
		for (uint32 c = 0; c < nchunks; c++)
		{
			const SiftChunkRef *ref = &h->chunks[c];

			if (c > 0 && !want[c - 1])
				continue;
			if ((uint64) ref->off + ref->len > d.data_len || ref->off < hdrlen)
				SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Chunk %u of granule %u lies outside it.", c, g);
			ranges[nr++] = (SiftByteRange) {.off = gs->off + d.data_off + ref->off,.len = ref->len,.dst_off = ref->off};
		}
		sift_part_read_ranges(rel, part, ranges, nr, rec, SIFT_IOS_SELECTIVE);
		pfree(ranges);
	}
	*desc = d;
	return rec;
}

void
sift_lazy_granule_open_desc(Relation rel, const SiftPart *part, uint32 g, const SiftGranuleDesc *gd, SiftLazyGranule *lg)
{
	const SiftSecDirEntry *gs = sift_part_section(part, SEC_GRANULES);
	uint32		nchunks = part->hdr.ncols + 1,
				hdrlen = offsetof(SiftGranuleHeader, chunks) + nchunks * sizeof(SiftChunkRef);
	const SiftGranuleHeader *h;

	if (gs == NULL || gd->data_off + gd->data_len > gs->len || hdrlen > gd->data_len)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u of the part is outside its section.", g);
	/* check-memory: allow one granule record, bounded by its rows */
	lg->rec = palloc(gd->data_len);
	sift_part_read_bytes(rel, part, gs->off + gd->data_off, hdrlen, lg->rec);
	h = (const SiftGranuleHeader *) lg->rec;
	if (h->magic != SIFT_GRANULE_MAGIC || h->nchunks != nchunks)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u of the part does not start with its header.", g);
	lg->part = part;
	lg->gd = *gd;
	lg->nchunks = nchunks;
	lg->loaded = palloc0(nchunks);
}

void
sift_lazy_granule_open(Relation rel, const SiftPart *part, uint32 g, SiftLazyGranule *lg)
{
	SiftGranuleDesc d = sift_part_read_gdesc(rel, part, g);

	sift_lazy_granule_open_desc(rel, part, g, &d, lg);
}

void
sift_lazy_granule_load(Relation rel, SiftLazyGranule *lg, const int *chunks, int n)
{
	const SiftSecDirEntry *gs = sift_part_section(lg->part, SEC_GRANULES);
	const SiftGranuleHeader *h = (const SiftGranuleHeader *) lg->rec;
	uint32		hdrlen = offsetof(SiftGranuleHeader, chunks) + lg->nchunks * sizeof(SiftChunkRef);
	SiftByteRange *ranges = palloc(Max(n, 1) * sizeof(SiftByteRange));
	int			nr = 0;

	for (int i = 0; i < n; i++)
	{
		const SiftChunkRef *ref;

		if (chunks[i] < 0 || (uint32) chunks[i] >= lg->nchunks)
			SIFT_SEG_CORRUPT(rel, lg->part->hdr.part_gen, "Chunk %d is not in a granule of %u chunks.", chunks[i], lg->nchunks);
		if (lg->loaded[chunks[i]])
			continue;
		ref = &h->chunks[chunks[i]];
		if ((uint64) ref->off + ref->len > lg->gd.data_len || ref->off < hdrlen)
			SIFT_SEG_CORRUPT(rel, lg->part->hdr.part_gen, "Chunk %d of a granule lies outside it.", chunks[i]);
		lg->loaded[chunks[i]] = true;
		ranges[nr++] = (SiftByteRange) {.off = gs->off + lg->gd.data_off + ref->off,.len = ref->len,.dst_off = ref->off};
	}
	if (nr > 0)
		sift_part_read_ranges(rel, lg->part, ranges, nr, lg->rec, SIFT_IOS_SELECTIVE);
	pfree(ranges);
}

char *
sift_part_read_granule(Relation rel, const SiftPart *part, uint32 g, SiftGranuleDesc *desc)
{
	return sift_part_read_granule_cols(rel, part, g, NULL, desc);
}

uint16
sift_part_read_nonnull_rows(Relation rel, const SiftPart *part, const SiftGranuleDesc *gdesc, int col, uint16 *out, uint16 cap)
{
	const SiftSecDirEntry *gs = sift_part_section(part, SEC_GRANULES);
	uint32		nchunks = part->hdr.ncols + 1,
				hdrlen = offsetof(SiftGranuleHeader, chunks) + nchunks * sizeof(SiftChunkRef),
				vlen;
	char	   *hdr;
	SiftChunkRef ref;
	char	   *bytes;

	if (col < 0 || (uint32) col + 1 >= nchunks || gs == NULL || gdesc->data_off + gdesc->data_len > gs->len || hdrlen > gdesc->data_len)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Column %d of a granule is outside the part.", col);
	hdr = palloc(hdrlen);
	sift_part_read_bytes(rel, part, gs->off + gdesc->data_off, hdrlen, hdr);
	memcpy(&ref, ((const SiftGranuleHeader *) hdr)->chunks + col + 1, sizeof(ref));
	vlen = sift_chunk_validity_len(&ref, gdesc->nrows);
	if ((uint64) ref.off + ref.len > gdesc->data_len || ref.off < hdrlen || ref.len < vlen || (ref.flags & SIFT_CHUNK_ELEMS))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Chunk %d of a granule lies outside it.", col + 1);
	if (!(ref.flags & (SIFT_CHUNK_ALL_VALID | SIFT_CHUNK_ALL_NULL)) && ref.nnonnull > cap)
		return cap + 1;
	if (ref.flags & SIFT_CHUNK_COMPRESSED)
	{
		/* check-memory: allow one granule record, bounded by its rows */
		char	   *rec = palloc(gdesc->data_len);

		memcpy(rec, hdr, hdrlen);
		sift_part_read_bytes(rel, part, gs->off + gdesc->data_off + ref.off, ref.len, rec + ref.off);
		return sift_granule_nonnull_rows(&(SiftGranuleView) {.rec = rec,.len = gdesc->data_len,.hdr = (const SiftGranuleHeader *) rec}, col, out, cap);
	}
	bytes = palloc(Max(vlen, 1));
	if (vlen > 0)
		sift_part_read_bytes(rel, part, gs->off + gdesc->data_off + ref.off, vlen, bytes);
	return sift_chunk_nonnull_rows(&ref, bytes, vlen, gdesc->nrows, out, cap);
}

SiftRowVer *
sift_part_read_vers(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g)
{
	const SiftSecDirEntry *vs = sift_part_section(part, SEC_VERS);
	uint64		before = 0;
	SiftRowVer *out;

	if (dir[g].flags & SIFT_GDESC_ALL_FROZEN)
		return NULL;
	if (dir[g].flags & SIFT_GDESC_SINGLE_XACT)
	{
		/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
		out = palloc(dir[g].nrows * sizeof(SiftRowVer));
		for (int r = 0; r < dir[g].nrows; r++)
			out[r] = (SiftRowVer) {XidFromFullTransactionId(dir[g].min_xid), dir[g].single_cmin};
		return out;
	}
	if (vs == NULL)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u is not frozen and the part has no version section.", g);
	for (uint32 i = 0; i < g; i++)
		if (!(dir[i].flags & (SIFT_GDESC_ALL_FROZEN | SIFT_GDESC_SINGLE_XACT)))
			before += dir[i].nrows;
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	out = palloc(dir[g].nrows * sizeof(SiftRowVer));
	sift_part_read_bytes(rel, part, vs->off + before * sizeof(SiftRowVer), dir[g].nrows * sizeof(SiftRowVer), out);
	return out;
}

uint64 *
sift_part_read_tids(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g)
{
	const SiftSecDirEntry *gs = sift_part_section(part, SEC_GRANULES);
	uint32		hdrlen = offsetof(SiftGranuleHeader, chunks) + (part->hdr.ncols + 1) * sizeof(SiftChunkRef),
				end;
	char	   *rec;
	SiftGranuleView v;
	uint64	   *out;

	if (gs == NULL || dir[g].data_off + dir[g].data_len > gs->len || hdrlen > dir[g].data_len)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u of the part is outside its section.", g);
	rec = palloc(dir[g].data_len);
	sift_part_read_bytes(rel, part, gs->off + dir[g].data_off, hdrlen, rec);
	end = ((SiftGranuleHeader *) rec)->chunks[SIFT_COL_TID_IDX].off + ((SiftGranuleHeader *) rec)->chunks[SIFT_COL_TID_IDX].len;
	if (end > dir[g].data_len || end < hdrlen)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The _tid chunk of granule %u lies outside it.", g);
	sift_part_read_bytes(rel, part, gs->off + dir[g].data_off + hdrlen, end - hdrlen, rec + hdrlen);
	v = (SiftGranuleView) {.rec = rec,.len = end,.hdr = (const SiftGranuleHeader *) rec};
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	out = palloc(dir[g].nrows * sizeof(uint64));
	sift_granule_tids(&v, out);
	return out;
}

/* The _tid column of granules [g0, g0 + n): the headers of all of them in one read, then the chunks, each read through one stream of the blocks they share */
uint64 **
sift_part_read_tids_batch(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g0, uint32 n, SiftIoMode mode)
{
	const SiftSecDirEntry *gs = sift_part_section(part, SEC_GRANULES);
	uint32		hdrlen = offsetof(SiftGranuleHeader, chunks) + (part->hdr.ncols + 1) * sizeof(SiftChunkRef),
				total = 0;
	char	   *hdrs = palloc(Max(n, 1) * hdrlen),
			   *recs;
	SiftByteRange *ranges = palloc(Max(n, 1) * sizeof(SiftByteRange));
	uint32	   *recoff = palloc(Max(n, 1) * sizeof(uint32)),
			   *end = palloc(Max(n, 1) * sizeof(uint32));
	uint64	  **out = palloc(Max(n, 1) * sizeof(uint64 *));

	for (uint32 i = 0; i < n; i++)
	{
		const SiftGranuleDesc *gd = &dir[g0 + i];

		if (gs == NULL || gd->data_off + gd->data_len > gs->len || hdrlen > gd->data_len)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "Granule %u of the part is outside its section.", g0 + i);
		ranges[i] = (SiftByteRange) {gs->off + gd->data_off, hdrlen, i * hdrlen};
	}
	sift_part_read_ranges(rel, part, ranges, n, hdrs, mode);
	for (uint32 i = 0; i < n; i++)
	{
		const SiftChunkRef *tid = &((const SiftGranuleHeader *) (hdrs + i * hdrlen))->chunks[SIFT_COL_TID_IDX];

		end[i] = tid->off + tid->len;
		if (end[i] > dir[g0 + i].data_len || end[i] < hdrlen)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The _tid chunk of granule %u lies outside it.", g0 + i);
		recoff[i] = total;
		total += end[i];
	}
	/* check-memory: allow n granules (at most SIFT_TID_BATCH_GRANULES), the _tid chunk of each */
	recs = palloc(Max(total, 1));
	for (uint32 i = 0; i < n; i++)
	{
		memcpy(recs + recoff[i], hdrs + i * hdrlen, hdrlen);
		ranges[i] = (SiftByteRange) {gs->off + dir[g0 + i].data_off + hdrlen, end[i] - hdrlen, recoff[i] + hdrlen};
	}
	sift_part_read_ranges(rel, part, ranges, n, recs, mode);
	for (uint32 i = 0; i < n; i++)
	{
		SiftGranuleView v = {.rec = recs + recoff[i],.len = end[i],.hdr = (const SiftGranuleHeader *) (recs + recoff[i])};

		/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
		out[i] = palloc(dir[g0 + i].nrows * sizeof(uint64));
		sift_granule_tids(&v, out[i]);
	}
	pfree(hdrs);
	pfree(ranges);
	pfree(recoff);
	pfree(end);
	pfree(recs);
	return out;
}

SiftTimeStride *
sift_part_read_tstride(Relation rel, const SiftPart *part, uint32 g, int *n)
{
	uint64		len;
	char	   *sec = sift_part_read_section(rel, part, SEC_GRANULE_TSTRIDE, &len);
	const SiftTStrideHeader *h = (const SiftTStrideHeader *) sec;
	const uint32 *off;
	uint64		strides;
	SiftTimeStride *out;

	if (sec == NULL)
		return NULL;
	if (len < sizeof(*h) || g >= h->ngranules || len != MAXALIGN(sizeof(*h) + (h->ngranules + 1) * sizeof(uint32)) + (uint64) h->total_strides * sizeof(SiftTimeStride))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The time stride index of the part does not hold granule %u.", g);
	off = (const uint32 *) (sec + sizeof(*h));
	strides = (uint64) MAXALIGN(sizeof(*h) + (h->ngranules + 1) * sizeof(uint32));
	*n = off[g + 1] - off[g];
	out = palloc(Max(*n, 1) * sizeof(SiftTimeStride));
	memcpy(out, sec + strides + (uint64) off[g] * sizeof(SiftTimeStride), *n * sizeof(SiftTimeStride));
	pfree(sec);
	return out;
}
