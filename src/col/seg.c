/*
 * col/seg.c - immutable column segments: writer and reader over the page store (col/seg.h, docs/COL.md 2.1, 2.3, 2.4, 2.7).
 *
 * Stream: UNITS (the unit records, 8-byte aligned), DICTS (a directory, then the serialized dictionary of each STR leaf), ZONES (a
 * directory, then per leaf the zone arrays of all units), then SCHEMA and SBDIR. The header page is a SiftSegHeaderData whose sections
 * are, in order of the stream: POSTINGS = UNITS, DICT = DICTS, DICTIDX = ZONES, DOCMAP = SCHEMA + SBDIR (dictroot_off = the schema's bytes,
 * dictroot_n = the units).
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/setsum.h"
#include "col/ver.h"
#include "core/live.h"
#include "core/cmp.h"

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
		SIFT_SEG_CORRUPT(index, entry->gen, "The segment is not immutable.");
	if (entry->format < SIFT_COL_FORMAT_MIN_READ || entry->format > SIFT_COL_FORMAT_MAX_READ)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" has a segment of column format 1.%d, this build reads 1.%d to 1.%d",
						RelationGetRelationName(index), entry->format, SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MAX_READ),
				 errhint(entry->format > SIFT_COL_FORMAT_MAX_READ ? "Use a newer pg_sift, or REINDEX." : "REINDEX; segments of a pre-release column format are not read.")));
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
	if (seg->schema->wide && entry->format < SIFT_COL_FORMAT_WIDE)
		corrupt(seg, "The schema has wide leaves, a segment of column format 1.%d cannot.", entry->format);
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
	pv->vers_at = ALIGN8(h->dictroot_off) + (Size) seg->nsb * sizeof(SiftColSBRef);
	if (pv->vers_at > s[SIFT_SEC_DOCMAP].len)
		corrupt(seg, "The DOCMAP section ends inside the unit directory.");
	pv->vers_len = s[SIFT_SEC_DOCMAP].len - pv->vers_at;
	if (pv->vers_len > 0 && (entry->format < SIFT_COL_FORMAT_VERS || pv->vers_len < sizeof(VersHeader) + (Size) seg->nsb * sizeof(VersUnit)))
		corrupt(seg, "The DOCMAP section holds %zu bytes after the unit directory, which is no version section of %u units (format 1.%d).",
				pv->vers_len, seg->nsb, entry->format);
	if ((entry->flags & SIFT_SEG_VERS) && pv->vers_len == 0)
		corrupt(seg, "The directory entry says the segment has unfrozen rows but it has no version section.");
	pv->live =(SiftLive) {index, entry->gen, h->live_first, h->live_npages, h->sb_hi - h->sb_lo + 1, h->ndocs};
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

	SIFT_BISECT(uint32, mid, lo, hi, seg->sbdir[mid].sb < sb);
	return lo;
}

/* The buffer of a unit record holds len bytes and the SIFT_COL_PAD bytes the kernels read past the packed payload of its last chunk */
static void
unit_room(SiftColSegment *seg, SegPriv *pv, uint32 len)
{
	if (len + SIFT_COL_PAD > pv->unitcap)
	{
		pv->unitcap = Max(len + SIFT_COL_PAD, 2 * pv->unitcap);
		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, pv->unitcap) : MemoryContextAlloc(seg->mcxt, pv->unitcap);
	}
}

const uint8 *
sift_cseg_unit(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	unit_room(seg, pv, r->len);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, r->len, pv->unitbuf);
	memset(pv->unitbuf + r->len, 0, SIFT_COL_PAD);
	*len = r->len;
	return pv->unitbuf;
}

const uint8 *
sift_cseg_unit_head(SiftColSegment *seg, uint32 u, uint32 *len, uint32 *avail)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	unit_room(seg, pv, r->len);
	*len = r->len;
	*avail = Min(r->len, SIFT_COL_UNIT_HEAD);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, *avail, pv->unitbuf);
	if (*avail == r->len)
		memset(pv->unitbuf + r->len, 0, SIFT_COL_PAD);
	return pv->unitbuf;
}

void
sift_cseg_unit_fill(SiftColSegment *seg, uint32 u, uint32 off, uint32 len)
{
	SegPriv    *pv = PRIV(seg);

	Assert((uint64) off + len <= seg->sbdir[u].len);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off + off, len, pv->unitbuf + off);
	memset(pv->unitbuf + seg->sbdir[u].len, 0, SIFT_COL_PAD);
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

/* The dictionary of leaf col read from the segment into mcxt (the loader of the backend's cache, col/dictcache.c) */
static SiftCDict *
dict_load(SiftColSegment *seg, int col, MemoryContext mcxt)
{
	uint64		off,
				len;
	char	   *data;
	Size		used;
	SiftCDict  *d;

	dict_region(seg, col, &off, &len);
	data = palloc(len);
	seg_read(seg, sec(seg, SIFT_SEC_DICT)->off + off, len, data);
	d = sift_cdict_parse(mcxt, data, len, &used);
	if (d == NULL)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	pfree(data);
	return d;
}

SiftCDict *
sift_cseg_dict(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_cached(seg, col, dict_load);
	return pv->dict[col];
}

const ZoneArrays *
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

/* The kinds of a leaf in a unit of this segment that has nnull NULL slots among nvals: complete in a segment of format SIFT_COL_FORMAT_KINDS, and where nothing needs them */
static bool
leaf_kinds_exact(const SiftColSegment *seg, const SiftColDef *def, uint32 nnull, uint32 nvals)
{
	if (seg->entry.format >= SIFT_COL_FORMAT_KINDS || !sift_col_def_has_kinds(def))
		return true;
	return nnull == 0 && !((def->flags & SIFT_COLD_ASTEXT) && nvals > 0);
}

bool
sift_cseg_kinds_exact(SiftColSegment *seg)
{
	if (seg->entry.format >= SIFT_COL_FORMAT_KINDS)
		return true;
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const SiftColDef *def = &seg->schema->cols[c];
		const ZoneArrays *z;

		if (!sift_col_def_has_kinds(def))
			continue;
		z = zone_arrays(seg, c);
		for (uint32 u = 0; u < seg->nsb; u++)
			if (!leaf_kinds_exact(seg, def, z->nnull[u], z->nvals[u]))
				return false;
	}
	return true;
}

#define SET_STATS_UNITS	64				/* units of a segment the planner's statistics look at for a leaf's summaries */
static const SetsLeaf *set_leaf(SiftColSegment *seg, int col);

void
sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc)
{
	const SiftColDef *def = &seg->schema->cols[col];
	const ZoneArrays *z = zone_arrays(seg, col);

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		bool		any = z->nnull[u] < z->nvals[u];

		acc->nunits++;
		acc->nvals += z->nvals[u];
		acc->nnull += z->nnull[u];
		acc->nokinds |= !leaf_kinds_exact(seg, def, z->nnull[u], z->nvals[u]);
		acc->has_jnull |= (z->flags[u] & SIFT_COLZ_JNULL) != 0;
		if (z->flags[u] & SIFT_COLZ_OVERFLOW)
		{
			acc->has_range = acc->has_sum = false;
			acc->has_ovf = true;
			acc->novf++;
		}
		if (def->kind == SIFT_COLK_GROUP)
			acc->has_sum &= !(z->flags[u] & SIFT_COLZ_NOSUM);
		else if (def->store == SIFT_COLS_INT && any)
		{
			acc->min = Min(acc->min, z->min[u]);
			acc->max = Max(acc->max, z->max[u]);
			acc->span += (double) z->max[u] - (double) z->min[u] + 1.0;
			acc->nzone++;
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
	if (set_leaf(seg, col) != NULL)
	{
		uint32		stride = Max(seg->nsb / SET_STATS_UNITS, 1);

		for (uint32 u = 0; u < seg->nsb; u += stride)
		{
			uint32		len,
						n;
			const uint8 *blob = sift_cseg_set(seg, col, u, &len);

			acc->set_sampled++;
			if (blob != NULL && len >= SIFT_CSET_HDR)
			{
				memcpy(&n, blob + 4, sizeof(n));
				acc->set_units++;
				acc->set_keys += n;
			}
		}
	}
	else
		acc->set_sampled += Min(seg->nsb, SET_STATS_UNITS);
}

void
sift_cseg_zone_sample(SiftColSegment *seg, SiftColZones *zs)
{
	const ZoneArrays *z[SIFT_COL_MAX_COLS] = {NULL};

	for (int c = 0; c < seg->schema->ncols && c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
			z[c] = zone_arrays(seg, c);
	for (uint32 u = 0; u < seg->nsb; u++, zs->units++)
	{
		if (zs->units % zs->stride != 0)
			continue;
		if (zs->n == SIFT_COL_ZONE_SAMPLE)
		{
			/* the units kept are the even ones: every other sample goes, and one unit in twice as many is sampled from here on */
			for (int c = 0; c < zs->ncols; c++)
				if (zs->lo[c] != NULL)
					for (uint32 k = 0; k < SIFT_COL_ZONE_SAMPLE / 2; k++)
					{
						zs->lo[c][k] = zs->lo[c][2 * k];
						zs->hi[c][k] = zs->hi[c][2 * k];
					}
			zs->n /= 2;
			zs->stride *= 2;
			if (zs->units % zs->stride != 0)
				continue;
		}
		for (int c = 0; c < zs->ncols; c++)
		{
			if (z[c] == NULL)
				continue;
			if (z[c]->flags[u] & SIFT_COLZ_OVERFLOW)
			{
				zs->lo[c][zs->n] = PG_INT64_MIN;
				zs->hi[c][zs->n] = PG_INT64_MAX;
			}
			else if (z[c]->nnull[u] >= z[c]->nvals[u])
			{
				zs->lo[c][zs->n] = PG_INT64_MAX;
				zs->hi[c][zs->n] = PG_INT64_MIN;
			}
			else
			{
				zs->lo[c][zs->n] = z[c]->min[u];
				zs->hi[c][zs->n] = z[c]->max[u];
			}
		}
		zs->n++;
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

/* Do off, then a bytes, then b bytes lie inside room bytes: the stored offsets and sizes are 64 bit, their sum may wrap */
static bool
within(uint64 room, uint64 off, uint64 a, uint64 b)
{
	return off <= room && a <= room - off && b <= room - off - a;
}

static const PaggHeader *
pagg_header(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->pagg == NULL)
	{
		const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);
		int			ncols = seg->schema->ncols;
		int			ver = PAGG_VERSION(seg->entry.format);
		Size		end = zones_end(seg),
					hsz = PAGG_HDR_SIZE(ver, ncols);
		PaggHeader *h;

		if (end + hsz > s->len)
			corrupt(seg, "The ZONES section holds %llu bytes, no room for the pre-aggregates after %zu.", ULL(s->len), end);
		h = MemoryContextAlloc(seg->mcxt, hsz);
		seg_read(seg, s->off + end, hsz, h);
		if (h->magic != PAGG_MAGIC || h->version != ver || h->ncols != (uint32) ncols || h->nsb != seg->nsb)
			corrupt(seg, "The pre-aggregates start with 0x%08x, version %u, %u leaves, %u units; a segment of column format 1.%d has version %d.",
					h->magic, h->version, h->ncols, h->nsb, seg->entry.format, ver);
		for (int c = 0; c < ncols; c++)
			if (h->lagg_off[c] % 8 != 0 || (h->lagg_off[c] && !within(s->len - end, h->lagg_off[c], lagg_block_size(seg->nsb), 0)))
				corrupt(seg, "The sums of leaf %d lie outside the section.", c);
		if (h->cubedir_off && (h->cubedir_off % 8 != 0 || !within(s->len - end, h->cubedir_off, (seg->nsb + 1) * sizeof(uint64), h->cube_bytes)))
			corrupt(seg, "The cubes (%llu bytes at %llu) lie outside the section.", ULL(h->cube_bytes), ULL(h->cubedir_off));
		if (ver > 1 && PAGG_ECUBEDIR(h) &&
			(PAGG_ECUBEDIR(h) % 8 != 0 || !within(s->len - end, PAGG_ECUBEDIR(h), (seg->nsb + 1) * sizeof(uint64), PAGG_ECUBE_BYTES(h))))
			corrupt(seg, "The element cubes (%llu bytes at %llu) lie outside the section.", ULL(PAGG_ECUBE_BYTES(h)), ULL(PAGG_ECUBEDIR(h)));
		if (ver > 2 && PAGG_SETS(h) &&
			(PAGG_SETS(h) % 8 != 0 || PAGG_SETS_BYTES(h) < sizeof(SetsHeader) + sizeof(SetsLeaf) || !within(s->len - end, PAGG_SETS(h), PAGG_SETS_BYTES(h), 0)))
			corrupt(seg, "The small-set summaries (%llu bytes at %llu) lie outside the section.", ULL(PAGG_SETS_BYTES(h)), ULL(PAGG_SETS(h)));
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

/* The blob of unit u from the directory at dir_off (nsb + 1 offsets, then the blobs) of the sidecar; NULL when the unit has none */
static const uint8 *
unit_blob(SiftColSegment *seg, uint32 u, uint64 dir_off, uint64 bytes, uint64 **dir, uint8 **buf, Size *cap, uint32 *len)
{
	uint64		at = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + dir_off,
				b0,
				b1;

	*len = 0;
	if (!dir_off)
		return NULL;
	if (*dir == NULL)
	{
		*dir = MemoryContextAlloc(seg->mcxt, (seg->nsb + 1) * sizeof(uint64));
		seg_read(seg, at, (seg->nsb + 1) * sizeof(uint64), *dir);
		for (uint32 i = 0; i < seg->nsb; i++)
			if ((*dir)[i] > (*dir)[i + 1] || (*dir)[i + 1] > bytes)
				corrupt(seg, "The cube directory is out of order at unit %u.", i);
	}
	b0 = (*dir)[u];
	b1 = (*dir)[u + 1];
	if (b1 == b0)
		return NULL;
	if (b1 - b0 > *cap)
	{
		*cap = Max(b1 - b0, 2 * *cap);
		*buf = *buf ? repalloc(*buf, *cap) : MemoryContextAlloc(seg->mcxt, *cap);
	}
	seg_read(seg, at + (seg->nsb + 1) * sizeof(uint64) + b0, b1 - b0, *buf);
	*len = b1 - b0;
	return *buf;
}

const uint8 *
sift_cseg_cube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	return unit_blob(seg, u, h->cubedir_off, h->cube_bytes, &pv->cubedir, &pv->cubebuf, &pv->cubecap, len);
}

const uint8 *
sift_cseg_ecube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	if (h->version < 2)
	{
		*len = 0;
		return NULL;
	}
	return unit_blob(seg, u, PAGG_ECUBEDIR(h), PAGG_ECUBE_BYTES(h), &pv->ecubedir, &pv->ecubebuf, &pv->ecubecap, len);
}

const uint8 *
sift_cseg_rollup(SiftColSegment *seg, uint32 u, int list, uint32 *len)
{
	const uint8 *e;
	uint32		elen;

	e = sift_cseg_ecube(seg, u, &elen);
	if (e == NULL)
	{
		*len = 0;
		return NULL;
	}
	e = sift_cpagg_elem_cube(seg->schema, e, elen, list, len);
	if (*len == UINT32_MAX)
		corrupt(seg, "The rollups of unit %u do not decode.", u);
	return e;
}

const uint8 *
sift_cseg_dim_cube(SiftColSegment *seg, uint32 u, int dim, uint32 *len)
{
	int			list = seg->schema->cols[dim].parent;

	return list < 0 ? sift_cseg_cube(seg, u, len) : sift_cseg_rollup(seg, u, list, len);
}

uint64
sift_cseg_set_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = pagg_header(seg);

	return h->version > 2 ? PAGG_SETS_BYTES(h) : 0;
}

uint64
sift_cseg_preagg_bytes(SiftColSegment *seg)
{
	pagg_header(seg);
	return sec(seg, SIFT_SEC_DICTIDX)->len - PRIV(seg)->pagg_end - sift_cseg_set_bytes(seg);
}

/* ---------- small-set summaries (col/setsum.h, docs/COL.md 2.13) ---------- */

/* The record of leaf col in the section, read and checked with the whole section on first use; NULL when the leaf has no summary in any unit */
static const SetsLeaf *
set_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;
	int			ncols = seg->schema->ncols;

	if (seg->entry.format < SIFT_COL_FORMAT_SETS)
		return NULL;
	h = pagg_header(seg);
	if (h->version < 3 || !PAGG_SETS(h))
		return NULL;
	if (pv->sets == NULL)
	{
		const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);
		uint64		at = s->off + pv->pagg_end + PAGG_SETS(h),
					room = PAGG_SETS_BYTES(h),
					lowest;
		SetsHeader	sh;
		SetsLeaf   *rec,
				   *sets;
		int			prev = -1;

		seg_read(seg, at, sizeof(sh), &sh);
		if (sh.magic != SETS_MAGIC || sh.version != 1 || sh.nsb != seg->nsb || sh.nleaves < 1 || sh.nleaves > (uint32) ncols ||
			sizeof(sh) + (uint64) sh.nleaves * sizeof(SetsLeaf) > room)
			corrupt(seg, "The small-set summaries start with 0x%08x, version %u, %u leaves, %u units.", sh.magic, sh.version, sh.nleaves, sh.nsb);
		rec = palloc(sh.nleaves * sizeof(SetsLeaf));
		seg_read(seg, at + sizeof(sh), sh.nleaves * sizeof(SetsLeaf), rec);
		lowest = PAGG_SETS(h) + sizeof(sh) + sh.nleaves * sizeof(SetsLeaf);
		sets = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(SetsLeaf));
		for (uint32 i = 0; i < sh.nleaves; i++)
		{
			const SetsLeaf *r = &rec[i];
			uint64		rel = r->dir_off - PAGG_SETS(h);

			if (r->leaf >= (uint32) ncols || (int) r->leaf <= prev || r->flags == 0 || (r->flags & ~(SETS_EXACT | SETS_BLOOM)) || !sift_cset_leaf_ok(seg->schema, &seg->schema->cols[r->leaf]))
				corrupt(seg, "The small-set record %u names leaf %u, which cannot have one.", i, r->leaf);
			if (r->dir_off < lowest || r->dir_off % 8 != 0 ||
				!within(room, rel, (uint64) (seg->nsb + 1) * sizeof(uint64), r->bytes))
				corrupt(seg, "The small-sets of leaf %u (%llu bytes at %llu) lie outside the section.", r->leaf, ULL(r->bytes), ULL(r->dir_off));
			lowest = ALIGN8(r->dir_off + (uint64) (seg->nsb + 1) * sizeof(uint64) + r->bytes);
			prev = r->leaf;
			sets[r->leaf] = *r;
		}
		pfree(rec);
		pv->setdir = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(uint64 *));
		pv->sets = sets;
	}
	return pv->sets[col].dir_off ? &pv->sets[col] : NULL;
}

const uint8 *
sift_cseg_set(SiftColSegment *seg, int col, uint32 u, uint32 *len)
{
	const SetsLeaf *sl = set_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);

	*len = 0;
	return sl ? unit_blob(seg, u, sl->dir_off, sl->bytes, &pv->setdir[col], &pv->setbuf, &pv->setcap, len) : NULL;
}

#define SET_PROBE_KEYS	64				/* a LUT of more matches is tested through the summaries that list their keys, not probed key by key */
#define SET_PROBE_WORK	(4 * 1024 * 1024)	/* IN members times the units to test, past which the summaries are not read */

/* Can a unit whose keys are s hold a row that passes t; codes: the keys of a LUT's matches when there are few (ncodes), else all of them are unknown */
static bool
set_may_pass(const SiftCSet *s, const SiftCTest *t, const int64 *codes, int ncodes)
{
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
			return sift_cset_may_hold(s, t->lo, t->hi);
		case SIFT_CT_SET:
			for (uint32 i = 0; i < t->nset; i++)
				if (sift_cset_may_hold(s, t->set[i], t->set[i]))
					return true;
			return false;
		default:					/* LUT */
			if (ncodes >= 0)
			{
				for (int i = 0; i < ncodes; i++)
					if (sift_cset_may_hold(s, codes[i], codes[i]))
						return true;
				return false;
			}
			if (s->kind == SIFT_CSET_EXACT)
			{
				int64		member[SIFT_CSET_LIMIT_MAX];

				sift_cset_members(s, member);
				for (uint32 i = 0; i < s->n; i++)
					if (member[i] >= 0 && (uint64) member[i] < t->lutbits && sift_col_bit_test(t->lut, (uint32) member[i]))
						return true;
				return false;
			}
			return true;
	}
}

/* Clears the bits of the units [ulo, uhi) whose summary shows that no key can pass t, and sets their bits in setcut (NULL: not told). Units with overflow values or inline strings are left alone */
void
sift_cseg_prune_sets(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut)
{
	const ZoneArrays *z;
	const SetsLeaf *sl;
	int64		codes[SET_PROBE_KEYS];
	int			ncodes = -1;
	uint32		todo = 0;

	if (t->kind != SIFT_CT_RANGE && t->kind != SIFT_CT_SET && t->kind != SIFT_CT_LUT)
		return;
	if (t->kind == SIFT_CT_LUT && t->lo <= t->hi)
	{
		/* the matches, when few, are keys to probe in a filter; the bounds of the LUT hold them all */
		int			m = 0;

		for (int64 c = Max(t->lo, 0); c <= t->hi && c < t->lutbits && m <= SET_PROBE_KEYS; c++)
			if (sift_col_bit_test(t->lut, (uint32) c))
			{
				if (m < SET_PROBE_KEYS)
					codes[m] = c;
				m++;
			}
		ncodes = m <= SET_PROBE_KEYS ? m : -1;
	}
	if ((sl = set_leaf(seg, col)) == NULL)
		return;
	/* a filter answers only a key: a range of more keys or a LUT of many matches is left to the zones when the leaf has no exact set */
	if (!(sl->flags & SETS_EXACT) && ((t->kind == SIFT_CT_RANGE && (uint64) t->hi - (uint64) t->lo >= SIFT_CSET_PROBES) || (t->kind == SIFT_CT_LUT && ncodes < 0)))
		return;
	for (uint32 u = ulo; u < uhi; u++)
		todo += (keep[u / 64] >> (u % 64)) & 1;
	if (t->kind == SIFT_CT_SET && (uint64) t->nset * todo > SET_PROBE_WORK)
		return;
	z = zone_arrays(seg, col);
	for (uint32 u = ulo; u < uhi; u++)
	{
		const uint8 *blob;
		uint32		len;
		SiftCSet	s;

		if (!(keep[u / 64] >> (u % 64) & 1) || (z->flags[u] & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE)) || z->nnull[u] >= z->nvals[u])
			continue;
		blob = sift_cseg_set(seg, col, u, &len);
		if (blob == NULL)
			continue;
		if (!sift_cset_open(blob, len, &s))
			corrupt(seg, "The small-set summary of leaf %d in unit %u is malformed.", col, u);
		if (!set_may_pass(&s, t, codes, ncodes))
		{
			keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			if (setcut)
				setcut[u / 64] |= UINT64CONST(1) << (u % 64);
		}
	}
}

uint32
sift_cseg_preagg_units(SiftColSegment *seg)
{
	uint32		n = 0,
				len;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const uint8 *c = sift_cseg_cube(seg, u, &len);

		n += c != NULL || sift_cseg_ecube(seg, u, &len) != NULL;
	}
	return n;
}

void
sift_cseg_pre_census(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, SiftCPaggCensus *c)
{
	/* the segment is minor 7 only when the setting allows and the kinds of its rows are known: what a rewrite of it writes */
	bool		minmax = spec->minmax && spec->ndims > 0 && setting >= SIFT_COL_FORMAT_MINMAX && sift_cseg_kinds_exact(seg);

	memset(c, 0, sizeof(*c));
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		uint32		len,
					elen;
		const uint8 *cube = sift_cseg_cube(seg, u, &len);
		const uint8 *ecube = sift_cseg_ecube(seg, u, &elen);

		sift_cpagg_census(seg->schema, spec, minmax, cube, cube ? len : 0, ecube, ecube ? elen : 0, c);
	}
}

/* Can every unit of the segment hold a histogram or a rollup dim of the leaf with at most cap groups: its values lie in a range of that width, or its dictionary has that many entries */
static bool
leaf_fits(SiftColSegment *seg, int leaf, uint32 cap)
{
	const SiftColDef *def = &seg->schema->cols[leaf];
	SiftColLeafStats ls;

	sift_col_leaf_stats_init(&ls);
	sift_cseg_leaf_stats(seg, leaf, &ls);
	if (ls.nvals == ls.nnull)
		return false;
	if (def->store == SIFT_COLS_STR)
		return !ls.inl && ls.dict_n <= cap;
	return def->store == SIFT_COLS_INT && ls.has_range && (double) ls.max - (double) ls.min + 1 <= cap;
}

bool
sift_cseg_pre_stale(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, const SiftCPaggCensus *c)
{
	if (c->odd > 0)
		return true;
	if (c->units == 0)
		return false;
	/*
	 * A section the spec asks and no unit holds is missing only where a unit is sure to fit it: a dim of cube(...) has no byte budget, a default
	 * dim fits when it has few groups; the rollups of rollup(...) likewise, the default ones are missing in a segment that is too old for them.
	 * A unit that is over a cap or a budget has none, and rewriting it would not change that.
	 */
	for (int i = 0; i < spec->ndims; i++)
		if (!spec->dim[i].cal && c->dim[i] == 0 && leaf_fits(seg, spec->dim[i].leaf, spec->fixed ? spec->max_groups : Min(spec->max_groups, 32)))
			return true;
	for (int r = 0; setting >= PAGG_ECUBE_MINOR && r < spec->nrollups; r++)
	{
		bool		rows = spec->rollup[r].list < 0;
		bool		fits = spec->rfixed || rows || seg->entry.format < PAGG_ECUBE_MINOR;

		/* the rollup of the rows is minor 8, written only where the kinds of the rows are known (as for the min and max) */
		if (rows && (setting < SIFT_COL_FORMAT_ROWS || !sift_cseg_kinds_exact(seg)))
			continue;
		for (int d = 0; (spec->rfixed || rows) && d < spec->rollup[r].ndims; d++)
			fits = fits && leaf_fits(seg, spec->rollup[r].dim[d], SIFT_PAGG_ROLLUP_TUPLES);
		if (c->roll[r] == 0 && fits)
			return true;
	}
	return false;
}

/* does the sorted set hold a key in [lo, hi]? */
static bool
set_meets(const int64 *set, uint32 n, int64 lo, int64 hi)
{
	uint32		a = 0,
				b = n;

	SIFT_BISECT(uint32, m, a, b, set[m] < lo);
	return a < n && set[a] <= hi;
}

void
sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut)
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
		case SIFT_CT_SET:			/* lo..hi bound the keys that can match; a unit whose zone holds none of them is out */
		case SIFT_CT_RANGE:
			sift_colk->zone_range(z->min, z->max, n, t->lo, t->hi, tmp);
			sift_colk->bits_and(keep, tmp, nw);
			if (t->kind == SIFT_CT_SET)
				for (uint32 u = 0; u < n; u++)
					if ((keep[u / 64] >> (u % 64) & 1) && z->nnull[u] < z->nvals[u] && !set_meets(t->set, t->nset, z->min[u], z->max[u]))
						keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set, n, t->fold, keep);
			sift_cseg_prune_sets(seg, col, t, keep, ulo, uhi, setcut);
			break;
		case SIFT_CT_LUT:
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set, n, t->fold, keep);
			sift_cseg_prune_sets(seg, col, t, keep, ulo, uhi, setcut);
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

/* The dirty bit of the unit's SB, read from the LIVE page now: neither the directory snapshot nor an earlier read may stand for it, because
 * a scan must read liveness after it reads the all-visible mask (docs/COL.md 2.7, "Order of reads") */
bool
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


/* A group of 3.0.1: one transaction's one command wrote every row (entry.reserved = xid, header sum_dl = cid) */
static bool
vers_tag(const SiftColSegment *seg)
{
	return seg->entry.format < SIFT_COL_FORMAT_VERS && seg->entry.reserved != 0;
}

bool
sift_cseg_vers(const SiftColSegment *seg)
{
	return vers_tag(seg) || ((seg->entry.flags & SIFT_SEG_VERS) && PRIV(seg)->vers_len > 0);
}

const VersUnit *
vers_units(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->vu == NULL)
	{
		VersHeader	h;
		uint64		at = sec(seg, SIFT_SEC_DOCMAP)->off + pv->vers_at;
		Size		dirsz = (Size) seg->nsb * sizeof(VersUnit);
		VersUnit   *vu;

		if (pv->vers_len == 0)
			corrupt(seg, "The segment has no version section.");
		seg_read(seg, at, sizeof(h), &h);
		if (h.magic != VERS_MAGIC || h.version != 1 || h.nsb != seg->nsb || sizeof(h) + dirsz + h.blob_bytes > pv->vers_len)
			corrupt(seg, "The version section starts with 0x%08x, version %u, %u units, %llu bytes of vectors.", h.magic, h.version, h.nsb,
					ULL(h.blob_bytes));
		vu = MemoryContextAlloc(seg->mcxt, dirsz);
		seg_read(seg, at + sizeof(h), dirsz, vu);
		for (uint32 u = 0; u < seg->nsb; u++)
			if ((vu[u].flags & ~(VU_VEC | VU_FROZEN)) != 0 || (!(vu[u].flags & VU_VEC) && (vu[u].xlen || vu[u].clen)) ||
				vu[u].off > h.blob_bytes || (uint64) vu[u].xlen + vu[u].clen > h.blob_bytes - vu[u].off)
				corrupt(seg, "The versions of unit %u (flags %u, %u + %u bytes at %llu) lie outside the section.", u, vu[u].flags,
						vu[u].xlen, vu[u].clen, ULL(vu[u].off));
		pv->vblob_at = at + sizeof(h) + dirsz;
		pv->vblob_len = h.blob_bytes;
		pv->vmin = h.min_xid;
		pv->vmax = h.max_xid;
		pv->vu = vu;
	}
	return pv->vu;
}

void
sift_cseg_ver_range(SiftColSegment *seg, uint32 *xmin_lo, uint32 *xmin_hi)
{
	Assert(sift_cseg_vers(seg));
	if (vers_tag(seg))
		*xmin_lo = *xmin_hi = seg->entry.reserved;
	else
	{
		(void) vers_units(seg);
		*xmin_lo = PRIV(seg)->vmin;
		*xmin_hi = PRIV(seg)->vmax;
	}
}

void
sift_cseg_ver_sum(SiftColSegment *seg, uint32 u, SiftColVerSum *sum)
{
	memset(sum, 0, sizeof(*sum));
	if (vers_tag(seg))
	{
		*sum = (SiftColVerSum) {.any = true,.xmin_lo = seg->entry.reserved,.xmin_hi = seg->entry.reserved,.cmin_hi = (uint32) seg->hdr->sum_dl};
		return;
	}
	if (!sift_cseg_vers(seg))
		return;
	{
		const VersUnit *vu = &vers_units(seg)[u];

		*sum = (SiftColVerSum) {.any = (vu->flags & VU_VEC) != 0,.frozen_rows = !(vu->flags & VU_VEC) || (vu->flags & VU_FROZEN),
		.xmin_lo = vu->xmin_lo,.xmin_hi = vu->xmin_hi,.cmin_hi = vu->cmin_hi};
	}
}

void
sift_cseg_ver_unit(SiftColSegment *seg, uint32 u, uint32 *xmin, uint32 *cmin)
{
	uint32		n = seg->sbdir[u].nrows;

	if (vers_tag(seg))
	{
		for (uint32 i = 0; i < n; i++)
		{
			xmin[i] = seg->entry.reserved;
			cmin[i] = (uint32) seg->hdr->sum_dl;
		}
		return;
	}
	if (sift_cseg_vers(seg) && (vers_units(seg)[u].flags & VU_VEC))
	{
		const VersUnit *vu = &vers_units(seg)[u];
		uint8	   *buf = palloc(vu->xlen + vu->clen);

		seg_read(seg, PRIV(seg)->vblob_at + vu->off, vu->xlen + vu->clen, buf);
		if (!sift_cver_decode(buf, vu->xlen, n, xmin) || !sift_cver_decode(buf + vu->xlen, vu->clen, n, cmin))
			corrupt(seg, "The versions of unit %u (SB %u) do not decode as %u rows.", u, seg->sbdir[u].sb, n);
		pfree(buf);
		return;
	}
	memset(xmin, 0, n * sizeof(uint32));
	memset(cmin, 0, n * sizeof(uint32));
}

bool
sift_cseg_page_rows(SiftColSegment *seg, uint32 u, BlockNumber blk, uint32 *first, uint64 *offmask)
{
	SiftCUnitView v;
	uint32		len,
				avail;
	const uint8 *rec = sift_cseg_unit_head(seg, u, &len, &avail);
	uint32		pg = blk % SIFT_SB_PAGES;

	if (!sift_cunit_parse_head(rec, avail, len, seg->schema, &v) || v.nrows != seg->sbdir[u].nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, seg->sbdir[u].sb);
	for (uint32 p = 0; p < v.npages; p++)
		if (v.page[p] == pg)
		{
			*first = v.rowstart[p];
			sift_cunit_page_offmask(&v, p, offmask);
			return true;
		}
	return false;
}

void
sift_cseg_unit_keys(SiftColSegment *seg, uint32 u, SiftKey *keys)
{
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);

	if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != seg->sbdir[u].nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, seg->sbdir[u].sb);
	sift_cunit_keys(&v, seg->sbdir[u].sb, keys);
}
