/*
 * col/seg/seg.c - the segment reader: open, close, the head, the unit directory, a unit
 * and its view; every section of a segment is read by the file
 * next to it that names it
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/seg_int.h"

#ifdef SIFT_TESTING
/*
 * col/seg/seg.c - immutable column segments: writer and reader over the page store.
 *
 * Stream: UNITS (the unit records, 8-byte aligned), DICTS (a directory, then the serialized dictionary of each STR leaf), ZONES (a
 * directory, then per leaf the zone arrays of all units), then SCHEMA and SBDIR. The header page is a SiftSegHeaderData whose sections
 * are, in order of the stream: POSTINGS = UNITS, DICT = DICTS, DICTIDX = ZONES, DOCMAP = SCHEMA + SBDIR (dictroot_off = the schema's bytes,
 * dictroot_n = the units).
 */

uint64		sift_test_seg_flip_at = UINT64_MAX;

uint8		sift_test_seg_flip_mask;
#endif

void
seg_read(SiftColSegment *seg, uint64 off, Size len, void *dst)
{
	char	   *p = dst;
#ifdef SIFT_TESTING
	uint64		off0 = off;
	Size		len0 = len;
#endif

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
#ifdef SIFT_TESTING
	if (sift_test_seg_flip_at != UINT64_MAX && sift_test_seg_flip_at >= off0 && sift_test_seg_flip_at - off0 < len0)
		((uint8 *) dst)[sift_test_seg_flip_at - off0] ^= sift_test_seg_flip_mask;
#endif
}

const SiftSectionRef *
seg_sec(const SiftColSegment *seg, SiftSection s)
{
	return &seg->hdr->sections[s];
}

/* The schema and the unit directory of seg, read and checked against its header, made in cx */
static SegHead *
head_read(SiftColSegment *seg, MemoryContext cx)
{
	const SiftSegHeaderData *h = seg->hdr;
	const SiftSectionRef *s = h->sections;
	SegHead    *hd = MemoryContextAlloc(cx, sizeof(SegHead));
	char	   *schema = palloc(h->dictroot_off);
	SiftColSBRef *dir = MemoryContextAlloc(cx, (Size) h->dictroot_n * sizeof(SiftColSBRef));
	uint64		rank = 0;

	hd->hdrlen = SEG_HDRLEN(h);
	hd->hdr = MemoryContextAlloc(cx, hd->hdrlen);
	memcpy(hd->hdr, h, hd->hdrlen);
	seg_read(seg, s[SIFT_SEC_DOCMAP].off, h->dictroot_off, schema);
	hd->schema = sift_cschema_parse(cx, schema, h->dictroot_off);
	if (hd->schema == NULL || hd->schema->ncols != (int) h->nterms)
		corrupt(seg, "The schema section is not a schema of %llu leaves.", ULL(h->nterms));
	if (hd->schema->wide && seg->entry.format < SIFT_COL_FORMAT_WIDE)
		corrupt(seg, "The schema has wide leaves, a segment of column format 1.%d cannot.", seg->entry.format);
	if (hd->schema->contain && seg->entry.format < SIFT_COL_FORMAT_CONTAIN)
		corrupt(seg, "The schema has containment lanes, a segment of column format 1.%d cannot.", seg->entry.format);
	pfree(schema);
	seg_read(seg, s[SIFT_SEC_DOCMAP].off + ALIGN8(h->dictroot_off), (Size) h->dictroot_n * sizeof(SiftColSBRef), dir);
	for (uint32 i = 0; i < h->dictroot_n; i++)
	{
		const SiftColSBRef *r = &dir[i];

		if (r->id < h->sb_lo || r->id > h->sb_hi || (i > 0 && r->id <= dir[i - 1].id) || r->off % 8 != 0 ||
			r->nrows == 0 || r->nrows > SIFT_COL_UNIT_MAX_ROWS || r->rank_base != rank ||
			r->off + r->len > s[SIFT_SEC_POSTINGS].len || (i > 0 && r->off < dir[i - 1].off + dir[i - 1].len))
			corrupt(seg, "SBDIR entry %u (SB %u at %llu, %u rows, rank %llu) is out of order.", i, r->id,
					ULL(r->off), r->nrows, ULL(r->rank_base));
		rank += r->nrows;
	}
	if (rank != h->ndocs)
		corrupt(seg, "SBDIR lists %llu rows, the header %llu.", ULL(rank), ULL(h->ndocs));
	hd->sbdir = dir;
	return hd;
}

SiftColSegment *
sift_cseg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSegment *seg = palloc0(sizeof(SiftColSegment));
	SegPriv    *pv = palloc0(sizeof(SegPriv));
	SiftSegHeaderData *h;
	uint64		size;
	const SegHead *head;
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
	head = HEAD_CACHED() ? sift_cseg_head_find(index, entry, h, mcxt) : NULL;
	if (head == NULL)
	{
		/* under the segment until it is kept: a head that does not check leaves nothing behind */
		MemoryContext cx = AllocSetContextCreate(mcxt, "sift cache segment head", ALLOCSET_SMALL_SIZES);

		head = head_read(seg, cx);
		if (HEAD_CACHED())
			sift_cseg_head_keep(index, entry, cx, (SegHead *) head, mcxt);
	}
	seg->schema = head->schema;
	seg->nsb = h->dictroot_n;
	seg->sbdir = head->sbdir;
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
	pv->look = palloc0(h->nterms * sizeof(DictLook *));
	pv->look_checked = palloc0(h->nterms * sizeof(bool));
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
sift_cseg_unit_lower_bound(const SiftColSegment *seg, uint32 sb)
{
	uint32		lo = 0,
				hi = seg->nsb,
				g,
				step = 1;

	if (seg->nsb == 0 || sb <= seg->sbdir[0].id)
		return 0;
	if (sb > seg->sbdir[seg->nsb - 1].id)
		return seg->nsb;
	/*
	 * The superblocks of a segment are nearly evenly spaced (all of them, or every other one where two workers built it): the unit at the same share of the
	 * directory is near, and a gallop from it brackets the answer in the steps of its distance, not of the directory.
	 */
	g = (uint32) ((uint64) (sb - seg->sbdir[0].id) * (seg->nsb - 1) / (seg->sbdir[seg->nsb - 1].id - seg->sbdir[0].id));
	if (seg->sbdir[g].id < sb)
	{
		for (lo = g + 1; lo + step < seg->nsb && seg->sbdir[lo + step - 1].id < sb; step <<= 1)
			lo += step;
		hi = Min(lo + step, seg->nsb);
	}
	else
	{
		for (hi = g; hi >= step && seg->sbdir[hi - step].id >= sb; step <<= 1)
			hi -= step;
		lo = hi >= step ? hi - step + 1 : 0;
	}
	SIFT_BISECT(uint32, mid, lo, hi, seg->sbdir[mid].id < sb);
	return lo;
}

uint32
sift_cseg_unit_of_sb(const SiftColSegment *seg, uint32 sb)
{
	uint32		u;

	if (sift_cseg_keyed(&seg->entry))
		return SIFT_CUNIT_UNKNOWN;
	u = sift_cseg_unit_lower_bound(seg, sb);
	return u < seg->nsb && seg->sbdir[u].id == sb ? u : seg->nsb;
}

/*
 * The buffer of a unit record is the largest of a lookup after the cursors (a unit of 100,000 rows is 400 kB), and a block that size grows the heap and goes back to the kernel
 * with its top when the statement ends (75 page faults for a lookup of 0.35 million instructions): the backend keeps the buffers of the segments it has closed, and the next
 * statement opens its segments with them. Taken by the first unit a segment reads, given back when the segment's context ends.
 */
#define UNIT_KEEP		4

#define UNIT_KEEP_BYTES	((Size) 4 << 20)

static MemoryContext unit_spare_cx;

static struct
{
	uint8	   *buf;
	Size		cap;
} unit_spare[UNIT_KEEP];

static int	nunit_spare;

static void
unit_release(void *arg)
{
	SegPriv    *pv = arg;

	if (nunit_spare < UNIT_KEEP && pv->unitcap <= UNIT_KEEP_BYTES)
		unit_spare[nunit_spare++] = (typeof(unit_spare[0])) {pv->unitbuf, pv->unitcap};
	else
		pfree(pv->unitbuf);
}

/* The buffer of a unit record holds len bytes and the SIFT_COL_PAD bytes the kernels read past the packed payload of its last chunk */
static void
unit_room(SiftColSegment *seg, SegPriv *pv, uint32 len)
{
	if (len + SIFT_COL_PAD <= pv->unitcap)
		return;
	if (!pv->unit_kept)
	{
		MemoryContextCallback *cb;

		if (unit_spare_cx == NULL)
			unit_spare_cx = AllocSetContextCreate(TopMemoryContext, "sift cache unit buffers", ALLOCSET_DEFAULT_SIZES);
		if (nunit_spare > 0 && pv->unitbuf == NULL)
		{
			pv->unitbuf = unit_spare[--nunit_spare].buf;
			pv->unitcap = unit_spare[nunit_spare].cap;
		}
		cb = MemoryContextAlloc(seg->mcxt, sizeof(MemoryContextCallback));
		*cb = (MemoryContextCallback) {unit_release, pv, NULL};
		MemoryContextRegisterResetCallback(seg->mcxt, cb);
		pv->unit_kept = true;
	}
	if (len + SIFT_COL_PAD > pv->unitcap)
	{
		pv->unitcap = Max(len + SIFT_COL_PAD, 2 * pv->unitcap);
		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, pv->unitcap) : MemoryContextAlloc(unit_spare_cx, pv->unitcap);
	}
}

const uint8 *
sift_cseg_unit(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	unit_room(seg, pv, r->len);
	seg_read(seg, seg_sec(seg, SIFT_SEC_POSTINGS)->off + r->off, r->len, pv->unitbuf);
	memset(pv->unitbuf + r->len, 0, SIFT_COL_PAD);
	*len = r->len;
	return pv->unitbuf;
}

uint64
sift_cseg_units_len(const SiftColSegment *seg)
{
	return seg_sec(seg, SIFT_SEC_POSTINGS)->len;
}

void
sift_cseg_unit_head_span(const SiftColSegment *seg, uint32 u, uint64 *off, uint64 *len)
{
	*off = seg_sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off;
	*len = Min(seg->sbdir[u].len, SIFT_COL_UNIT_HEAD);
}

const uint8 *
sift_cseg_unit_head(SiftColSegment *seg, uint32 u, uint32 *len, uint32 *avail)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];
	uint64		off,
				n;

	unit_room(seg, pv, r->len);
	*len = r->len;
	sift_cseg_unit_head_span(seg, u, &off, &n);
	*avail = n;
	seg_read(seg, off, *avail, pv->unitbuf);
	/* the directory and the chunk table of a unit of many pages, or of a key-cut unit of many rows, end beyond the head */
	for (uint32 need; *avail < r->len && (need = sift_cunit_head_need(pv->unitbuf, *avail, r->len, seg->schema->ncols)) > 0;)
	{
		sift_cseg_unit_fill(seg, u, *avail, need - *avail);
		*avail = need;
	}
	if (*avail == r->len)
		memset(pv->unitbuf + r->len, 0, SIFT_COL_PAD);
	return pv->unitbuf;
}

void
sift_cseg_unit_fill(SiftColSegment *seg, uint32 u, uint32 off, uint32 len)
{
	SegPriv    *pv = PRIV(seg);

	Assert((uint64) off + len <= seg->sbdir[u].len);
	seg_read(seg, seg_sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off + off, len, pv->unitbuf + off);
	memset(pv->unitbuf + seg->sbdir[u].len, 0, SIFT_COL_PAD);
}

static void
unit_view_fill(void *seg, uint32 u, uint32 off, uint32 len)
{
	sift_cseg_unit_fill(seg, u, off, len);
}

/* The head of unit u parsed: false when it does not parse. The offsets of the rows of a key-cut unit are read by sift_cunit_keyed_load when a scan needs the TIDs */
bool
sift_cseg_unit_view(SiftColSegment *seg, uint32 u, SiftCUnitView *v)
{
	uint32		len,
				avail;
	const uint8 *rec = sift_cseg_unit_head(seg, u, &len, &avail);

	if (!sift_cunit_parse_head(rec, avail, len, seg->schema, v) || v->nrows != seg->sbdir[u].nrows)
		return false;
	v->fill = unit_view_fill;
	v->fill_seg = seg;
	v->fill_u = u;
	return true;
}

/*
 * The point directories at the end of the DICT section, found by the footer in its last 16 bytes: the tables of the leaves, read on
 * first use. A section without them ends with its last dictionary. The frame is checked whole, so that no byte of it can change what is read without a
 * refusal: the reserved words are 0, the records ascend by leaf, and the tables follow the records one after the other and end at the footer (a record
 * dropped, a leaf changed or a table moved shows as a gap or an overlap). The bytes of a table are the check of the segment's (sift_cseg_verify_points).
 */
static bool
point_version_ok(uint32 v)
{
	return v == SIFT_CPOINT_VERSION_ROWS || v == SIFT_CPOINT_VERSION || v == SIFT_CPOINT_VERSION_ORD;
}

const PointLeaf *
seg_points_read(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICT);
	uint64		foot[2],
				next,
				par,
				rat,
				hat,
				first,
				end;
	bool		rooted = seg->entry.format >= SIFT_COL_FORMAT_ROOT;
	int64		prev = -1;
	uint32		hdr[4],
				tail[2],
				npk = 0,
				ipk = 0;
	char	   *recs;
	SiftCPackPar *pars = NULL;

	if (pv->pt_checked)
		return pv->pt;
	pv->pt_checked = true;
	pv->dict_end = s->len;
	if (seg->entry.format < SIFT_COL_FORMAT_DICTIDX || s->len < sizeof(foot) + sizeof(hdr))
		return NULL;
	seg_read(seg, s->off + s->len - sizeof(foot), sizeof(foot), foot);
	memcpy(tail, &foot[1], sizeof(tail));
	if (tail[0] != SIFT_CPOINT_MAGIC || !point_version_ok(tail[1]))
	{
		/* a footer of another version is no directory; a header of a known version where the offset says is a footer that was damaged */
		if (rooted || (foot[0] % 8 == 0 && foot[0] + sizeof(hdr) <= s->len - sizeof(foot)))
		{
			seg_read(seg, s->off + (rooted ? s->len - sizeof(foot) - sizeof(hdr) : foot[0]), sizeof(hdr), hdr);
			if (hdr[0] == SIFT_CPOINT_MAGIC && point_version_ok(hdr[1]))
				corrupt(seg, "The footer of the point directories of the DICT section has the magic 0x%08x, version %u.", tail[0], tail[1]);
		}
		return NULL;
	}
	/* the header, the records and the parameters follow the dictionaries and precede the tables (before minor 37), or follow the tables and end at the footer, which
	 * is then the one page a lookup reads for them (minor 37) */
	hat = rooted ? s->len - sizeof(foot) - sizeof(hdr) : foot[0];
	if (foot[0] % 8 != 0 || (rooted ? s->len < sizeof(foot) + sizeof(hdr) || foot[0] > hat : foot[0] + sizeof(hdr) > s->len - sizeof(foot)))
		corrupt(seg, "The point directories of the DICT section start at %llu.", (unsigned long long) foot[0]);
	seg_read(seg, s->off + hat, sizeof(hdr), hdr);
	if (hdr[0] != SIFT_CPOINT_MAGIC || hdr[1] != tail[1] || hdr[2] > (uint32) seg->schema->ncols || hdr[3] > (hdr[1] == SIFT_CPOINT_VERSION_ORD ? 2 * (uint32) seg->schema->ncols : 0) ||
		(rooted ? (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN > hat - foot[0] : foot[0] + sizeof(hdr) + (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN > s->len - sizeof(foot)))
		corrupt(seg, "The point directories of the DICT section have the header 0x%08x, version %u, %u leaves.", hdr[0], hdr[1], hdr[2]);
	rat = rooted ? hat - (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN : hat + sizeof(hdr);
	pv->pt = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(PointLeaf));
	pv->go = MemoryContextAllocZero(seg->mcxt, 2 * seg->schema->ncols * sizeof(GOrdLeaf));
	recs = palloc((hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN);
	seg_read(seg, s->off + rat, (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN, recs);
	/* the parameters of the packed tables are next to the records, one for each, in the order of the leaves */
	for (uint32 i = 0; i < hdr[2]; i++)
	{
		uint32		w5;

		memcpy(&w5, recs + (uint64) i * SIFT_CPOINT_RECLEN + 5 * sizeof(uint32), sizeof(w5));
		npk += (w5 & SIFT_CPOINT_PACKED) != 0;
	}
	par = rooted ? rat - (uint64) npk * SIFT_CPOINT_PK_PAR : rat + (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN;
	first = rooted ? foot[0] : par + (uint64) npk * SIFT_CPOINT_PK_PAR;
	end = rooted ? par : s->len - sizeof(foot);
	if (npk > 0)
	{
		if (rooted ? (uint64) npk * SIFT_CPOINT_PK_PAR > rat - foot[0] : first > end)
			corrupt(seg, "The packed point directories of the DICT section have no room for their parameters.");
		pars = palloc(npk * sizeof(SiftCPackPar));
		seg_read(seg, s->off + par, npk * SIFT_CPOINT_PK_PAR, pars);
	}
	next = first;
	for (uint32 i = 0; i < hdr[2] + hdr[3]; i++)
	{
		uint32		rec[6],
					mode;
		bool		exact,
					packed;
		uint64		at;
		uint64		size;

		memcpy(rec, recs + (uint64) i * SIFT_CPOINT_RECLEN, sizeof(rec));
		memcpy(&at, recs + (uint64) i * SIFT_CPOINT_RECLEN + sizeof(rec), sizeof(at));
		if (i >= hdr[2])
		{
			/* a global order block (col/gorder.h): chunks, entries, values, 0, words of 8 bytes; its header and directory are checked when it is read */
			if (i == hdr[2])
				prev = -1;
			size = (uint64) rec[4] * 8;
			if (rec[0] >= 2 * (uint32) seg->schema->ncols || (int64) rec[0] <= prev || rec[5] != SIFT_CPOINT_MODE_ORDER || rec[1] == 0 || rec[2] == 0 || rec[2] >= SIFT_CPOINT_SEG_ROWS ||
				rec[3] > rec[2] || (rec[1] != (rec[2] + SIFT_CGORD_ROWS - 1) / SIFT_CGORD_ROWS && rec[1] != (rec[2] + SIFT_CGORD_ROWS_FINE - 1) / SIFT_CGORD_ROWS_FINE) || size < SIFT_CGORD_HDR + (uint64) rec[1] * SIFT_CGORD_DIRENT ||
				at != next || size > end - next)
				corrupt(seg, "The global order %u of the DICT section is malformed.", i - hdr[2]);
			pv->go[rec[0]] = (GOrdLeaf) {s->off + at, size, {rec[2], rec[3], rec[1], rec[1] == (rec[2] + SIFT_CGORD_ROWS - 1) / SIFT_CGORD_ROWS ? SIFT_CGORD_ROWS : SIFT_CGORD_ROWS_FINE, 0, -1}, NULL, 0};
			prev = rec[0];
			next += size;
			continue;
		}
		packed = (rec[5] & SIFT_CPOINT_PACKED) != 0;
		size = packed ? (uint64) rec[4] * sizeof(uint32) : ALIGN8((uint64) rec[1] * sizeof(uint64) + (uint64) rec[4] * sizeof(uint32));
		mode = rec[5] & ~(SIFT_CPOINT_EXACT | SIFT_CPOINT_PACKED);
		exact = (rec[5] & SIFT_CPOINT_EXACT) != 0;
		if (packed)
		{
			const SiftCPackPar *pk = &pars[ipk++];

			if (rec[0] >= (uint32) seg->schema->ncols || (int64) rec[0] <= prev || (mode != SIFT_CPOINT_MODE_ROWS && mode != SIFT_CPOINT_MODE_TIDS) || !exact ||
				seg->entry.format < SIFT_COL_FORMAT_PACKED || rec[1] != rec[2] || rec[3] == 0 || rec[3] > rec[2] || rec[2] >= SIFT_CPOINT_SEG_ROWS ||
				pk->dense > 1 || pk->zero0 != 0 || pk->zero1 != 0 || pk->nbuckets == 0 || pk->pbits == 0 || pk->pbits > 48 || pk->rbits > SIFT_CPOINT_PK_RBITS ||
				(pk->dense ? pk->rbits >= SIFT_CPOINT_PK_RBITS : (pk->rbits != SIFT_CPOINT_PK_RBITS || (pk->nbuckets & (pk->nbuckets - 1)) != 0 || pk->kmin != 0)) ||
				(mode == SIFT_CPOINT_MODE_ROWS ? (pk->pbase != 0 || pk->pmul != 1 || pk->pbits > 31) : (pk->pmul == 0 || pk->pmul > SIFT_CPOINT_TID_MAXOFF)) ||
				at != next || size != sift_cpoint_pk_bytes(pk, rec[1]) || size > end - next)
				corrupt(seg, "The packed point directory %u of the DICT section is malformed.", i);
			if (mode == SIFT_CPOINT_MODE_TIDS && (hdr[1] == SIFT_CPOINT_VERSION_ROWS || sift_cseg_keyed(&seg->entry)))
				corrupt(seg, "The point directory of leaf %u lists heap addresses, which this segment cannot; REINDEX the index.", rec[0]);
			pv->pt[rec[0]] = (PointLeaf) {s->off + at, rec[1], rec[3], rec[2], rec[4], mode == SIFT_CPOINT_MODE_TIDS, false, true, 0, true, *pk};
			prev = rec[0];
			next += size;
			continue;
		}
		if (rec[0] >= (uint32) seg->schema->ncols || (int64) rec[0] <= prev || mode > (hdr[1] == SIFT_CPOINT_VERSION || hdr[1] == SIFT_CPOINT_VERSION_ORD ? SIFT_CPOINT_MODE_ZROWS : SIFT_CPOINT_MODE_ROWS) ||
			rec[1] < 16 || (!exact && (rec[1] & (rec[1] - 1)) != 0) || rec[3] == 0 ||
			rec[3] > rec[1] || rec[2] < rec[3] || rec[2] >= SIFT_CPOINT_SEG_ROWS || (uint64) rec[4] > (mode == SIFT_CPOINT_MODE_TIDS || mode == SIFT_CPOINT_MODE_ZROWS ? 3 * (uint64) rec[2] : (uint64) rec[2] + rec[3]) ||
			at != next || size > end - next)
			corrupt(seg, "The point directory %u of the DICT section is malformed.", i);
		/* the units of a key-cut segment are not block ranges: an address names no unit, so such a segment lists rows */
		if (mode == SIFT_CPOINT_MODE_TIDS && sift_cseg_keyed(&seg->entry))
			corrupt(seg, "The point directory of leaf %u lists heap addresses, which a key-cut segment cannot; REINDEX the index.", rec[0]);
		pv->pt[rec[0]] = (PointLeaf) {s->off + at, rec[1], rec[3], rec[2], rec[4], mode == SIFT_CPOINT_MODE_TIDS, mode == SIFT_CPOINT_MODE_ZROWS, exact};
		prev = rec[0];
		next += size;
	}
	pfree(recs);
	if (pars)
		pfree(pars);
	if (next != end)
		corrupt(seg, "The point directories of the DICT section end at %llu of %llu.", (unsigned long long) next, (unsigned long long) end);
	pv->dict_end = foot[0];
	return pv->pt;
}

/* ---- the global order of a leaf (col/gorder.h) ---- */

/* The block of leaf col with its header read (the directory entries are read and checked when they are used, the whole directory by verify), NULL when the segment has none */
GOrdLeaf *
seg_gord_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	GOrdLeaf   *g;
	SiftCGOrdHdr h;
	Size		front;
	uint8	   *raw;

	seg_points_read(seg);
	if (pv->go == NULL || pv->go[col].bytes == 0)
		return NULL;
	g = &pv->go[col];
	if (g->dir != NULL)
		return g;
	front = SIFT_CGORD_HDR + (Size) g->hdr.chunks * SIFT_CGORD_DIRENT;
	raw = MemoryContextAlloc(seg->mcxt, SIFT_CGORD_HDR + SIFT_CGORD_DIRENT);
	seg_read(seg, g->at, SIFT_CGORD_HDR, raw);
	if (!sift_cgord_head(raw, g->bytes, &h) || h.entries != g->hdr.entries || h.values != g->hdr.values || h.chunks != g->hdr.chunks ||
		(h.gcol >= 0 && h.gcol >= seg->schema->ncols))
		corrupt(seg, "The global order of leaf %d has a malformed header.", col);
	/* the chunks end where the groups begin */
	g->gat = (uint64) front;
	if (h.chunks > 0)
	{
		SiftCGOrdDir last;

		seg_read(seg, g->at + SIFT_CGORD_HDR + (uint64) (h.chunks - 1) * SIFT_CGORD_DIRENT, SIFT_CGORD_DIRENT, raw + SIFT_CGORD_HDR);
		if (!sift_cgord_dirent(raw + SIFT_CGORD_HDR, &h, g->bytes, h.chunks - 1, &last))
			corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, h.chunks - 1);
		g->gat = (uint64) last.off + ALIGN8((uint64) last.klen) + ALIGN8((uint64) last.alen);
	}
	if (g->gat + (h.strbytes != 0 ? (uint64) h.strbytes : (uint64) h.ngroups * SIFT_CGORD_GRPENT) != g->bytes)
		corrupt(seg, "The global order of leaf %d ends at %llu of %llu.", col, (unsigned long long) g->gat, (unsigned long long) g->bytes);
	g->hdr = h;
	g->dir = raw;
	return g;
}

/* the directory, for the bisections of a seek: a few reads of the entries would cost more than the copy of it */
static void
gord_dir_read(SiftColSegment *seg, GOrdLeaf *g)
{
	if (g->ents == NULL)
	{
		Size		len = (Size) g->hdr.chunks * SIFT_CGORD_DIRENT;

		g->ents = MemoryContextAlloc(seg->mcxt, Max(len, 1));
		seg_read(seg, g->at + SIFT_CGORD_HDR, len, g->ents);
	}
}

/* the directory entry of chunk c, checked against the block */
static SiftCGOrdDir
gord_dirent(SiftColSegment *seg, const GOrdLeaf *g, int col, uint32 c)
{
	SiftCGOrdDir d;
	uint8		one[SIFT_CGORD_DIRENT];
	const uint8 *ent = one;

	if (c >= g->hdr.chunks)
		corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, c);
	if (g->ents != NULL)
		ent = g->ents + (uint64) c * SIFT_CGORD_DIRENT;
	else
		seg_read(seg, g->at + SIFT_CGORD_HDR + (uint64) c * SIFT_CGORD_DIRENT, SIFT_CGORD_DIRENT, one);
	if (!sift_cgord_dirent(ent, &g->hdr, g->bytes, c, &d))
		corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, c);
	return d;
}

/* The collation of the strings of a text block (0: the leaf is not text, or the segment has no block) */
uint32
sift_cseg_gorder_collation(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	return g == NULL ? 0 : g->hdr.collid;
}

/* The number of strings of a text block, read on first use; the start of its strings in the segment */
static uint64
gord_strs(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	uint64		seg_sec = g->at + g->bytes - g->hdr.strbytes;

	if (g->sbno == 0 && g->nstr == 0)
	{
		uint8		head[16];
		uint32		nblk;

		seg_read(seg, seg_sec, sizeof(head), head);
		if (!sift_cgord_str_head(head, &g->hdr, &g->nstr, &nblk))
			corrupt(seg, "The strings of the global order of leaf %d are malformed.", col);
		g->sbno = UINT32_MAX;
	}
	return seg_sec;
}

/* The string of rank r of a text block, valid until the next call for the leaf; *len its bytes */
const char *
sift_cseg_gorder_str(SiftColSegment *seg, int col, uint32 r, uint32 *len)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		b = r / SIFT_CGORD_STR_BLK;
	uint32		off[2];
	uint64		seg_sec;
	char	   *str;

	if (g == NULL || g->hdr.strbytes == 0)
		corrupt(seg, "The global order of leaf %d has no strings.", col);
	seg_sec = gord_strs(seg, g, col);
	if (r >= g->nstr)
		corrupt(seg, "The global order of leaf %d has no string %u.", col, r);
	if (g->sbno != b)
	{
		/* the offsets of the block and of the next, read where they are: a segment of 200,000 strings has 12,000 blocks */
		seg_read(seg, seg_sec + 16 + (uint64) b * sizeof(uint32), sizeof(off), off);
		if (off[0] > off[1] || off[1] > g->hdr.strbytes || off[1] - off[0] > (uint32) SIFT_CGORD_STR_BLK * (SIFT_CGORD_STR_MAX + 8))
			corrupt(seg, "The strings of the global order of leaf %d have a malformed block %u.", col, b);
		if (g->sblk == NULL)
			g->sblk = MemoryContextAlloc(seg->mcxt, (Size) SIFT_CGORD_STR_BLK * (SIFT_CGORD_STR_MAX + 8) + SIFT_CGORD_STR_MAX);
		seg_read(seg, seg_sec + off[0], off[1] - off[0], g->sblk);
		g->sbno = b;
		g->sblen = off[1] - off[0];
		g->spos = (SiftCStrPos) {0, 0, 0};
	}
	str = (char *) g->sblk + (Size) SIFT_CGORD_STR_BLK * (SIFT_CGORD_STR_MAX + 8);
	if (!sift_cgord_str_get(g->sblk, g->sblen, &g->spos, r % SIFT_CGORD_STR_BLK, str, len))
		corrupt(seg, "The strings of the global order of leaf %d have a malformed block %u.", col, b);
	return str;
}

/* the order of the string of rank r against s */
static int
gord_str_vs(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 r, const char *s, uint32 slen)
{
	uint32		len;
	const char *str = sift_cseg_gorder_str(seg, col, r, &len);

	return sift_cgord_str_order(str, len, s, slen, g->hdr.collid);
}

uint32
sift_cseg_gorder_strrank(SiftColSegment *seg, int col, const char *s, uint32 slen, bool after)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		lo = 0,
				hi;

	if (g == NULL || g->hdr.strbytes == 0)
		corrupt(seg, "The global order of leaf %d has no strings.", col);
	(void) gord_strs(seg, g, col);
	hi = g->nstr;
	SIFT_BISECT(uint32, mid, lo, hi, after ? gord_str_vs(seg, g, col, mid, s, slen) <= 0 : gord_str_vs(seg, g, col, mid, s, slen) < 0);
	return lo;
}

/* The slot of the block of leaf col to read: the one sorted by group (the second slot, ncols + col; the first when it is grouped, as the user's order_array_group writes it) or the plain one; -1 when the segment has none */
int
sift_cseg_gorder_slot(SiftColSegment *seg, int col, bool grouped)
{
	const GOrdLeaf *g = grouped ? seg_gord_leaf(seg, seg->schema->ncols + col) : NULL;

	if (g != NULL)
		return seg->schema->ncols + col;
	g = seg_gord_leaf(seg, col);
	return g != NULL && (g->hdr.gcol >= 0) == grouped ? col : -1;
}

bool
sift_cseg_gorder(SiftColSegment *seg, int col, uint32 *entries, uint32 *values)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL)
		return false;
	*entries = g->hdr.entries;
	*values = g->hdr.values;
	return true;
}

/* The first w entries (the m of the chunk when fewer) of chunk c of leaf col: keys and heap addresses, each SIFT_CGORD_CAP slots; the count decoded */
uint32
sift_cseg_gorder_head(SiftColSegment *seg, int col, uint32 c, uint32 w, int64 *key, uint64 *addr)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	SiftCGOrdDir d;
	uint32		m;
	uint8	   *buf;

	if (g == NULL || c >= g->hdr.chunks)
		corrupt(seg, "The global order of leaf %d has no chunk %u.", col, c);
	d = gord_dirent(seg, g, col, c);
	m = sift_cgord_chunk_rows(&g->hdr, c);
	buf = palloc(ALIGN8((Size) d.klen) + ALIGN8((Size) d.alen) + SIFT_COL_PAD);
	seg_read(seg, g->at + d.off, ALIGN8((Size) d.klen) + ALIGN8((Size) d.alen), buf);
	if (!sift_cgord_decode(seg->mcxt, buf, d.klen, buf + ALIGN8((Size) d.klen), d.alen, m, w, key, addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	pfree(buf);
	return Min(w, m);
}

/* The first max(w, up to p) entries of the chunk that holds the entry p, into buf (SiftCGOrdBuf: its first entry, the entries of the chunk, the count decoded) */
void
sift_cseg_gorder_fill(SiftColSegment *seg, int col, uint32 p, uint32 w, SiftCGOrdBuf *buf)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		c;

	if (g == NULL)
		corrupt(seg, "The global order of leaf %d is not there.", col);
	c = p / g->hdr.crows;
	buf->at = c * g->hdr.crows;
	buf->rows = sift_cgord_chunk_rows(&g->hdr, c);
	buf->cm = sift_cseg_gorder_head(seg, col, c, Max(w, p - buf->at + 1), buf->key, buf->addr);
}

uint32
sift_cseg_gorder_chunk(SiftColSegment *seg, int col, uint32 c, int64 *key, uint64 *addr)
{
	return sift_cseg_gorder_head(seg, col, c, UINT32_MAX, key, addr);
}

/* The leaf the block of col is grouped by, -1 when it is not */
int
sift_cseg_gorder_group(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	return g == NULL ? -1 : g->hdr.gcol;
}

/*
 * The entries of a group of a grouped block: the value entries [*vs, *ve) and the NULL ones [*ns, *ne); empty ranges when the group has none. A block that is not
 * grouped has one group of all its entries.
 */
void
sift_cseg_gorder_range(SiftColSegment *seg, int col, int64 group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		lo = 0,
				hi;

	if (g == NULL)
		corrupt(seg, "The global order of leaf %d is not there.", col);
	if (g->hdr.gcol < 0)
	{
		*vs = 0;
		*ve = *ns = g->hdr.values;
		*ne = g->hdr.entries;
		return;
	}
	hi = g->hdr.ngroups;
	/* the groups are searched where they are: a table of many groups is not read whole for one probe */
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		SiftCGOrdGrp e;

		seg_read(seg, g->at + g->gat + (uint64) mid * SIFT_CGORD_GRPENT, sizeof(e), &e);
		if (e.group < group)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < g->hdr.ngroups)
	{
		SiftCGOrdGrp e;

		seg_read(seg, g->at + g->gat + (uint64) lo * SIFT_CGORD_GRPENT, sizeof(e), &e);
		if (e.group == group)
		{
			if ((uint64) e.vstart + e.vcount > g->hdr.values || e.nstart < g->hdr.values || (uint64) e.nstart + e.ncount > g->hdr.entries)
				corrupt(seg, "The global order of leaf %d has a malformed group.", col);
			*vs = e.vstart;
			*ve = e.vstart + e.vcount;
			*ns = e.nstart;
			*ne = e.nstart + e.ncount;
			return;
		}
	}
	*vs = *ve = *ns = *ne = 0;
}

static uint32
gorder_chunk_memo(SiftColSegment *seg, int col, uint32 c, SiftCGOrdBuf *memo)
{
	uint32		at = c * seg_gord_leaf(seg, col)->hdr.crows;

	if (memo->at != at)
		sift_cseg_gorder_fill(seg, col, at, UINT32_MAX, memo);
	return memo->cm;
}

/*
 * The first entry of [vs, ve) (value entries ascending by key there) whose key is not before key (after: is after it), found in the directory by the first keys of the
 * chunks that start inside the range and in one chunk; ve when there is none.
 */
static uint32
gorder_seek(SiftColSegment *seg, int col, uint32 vs, uint32 ve, int64 key, bool after, SiftCGOrdBuf *memo)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		c0,
				lo,
				hi,
				m,
				from,
				to;
	uint32		crows;

	if (g == NULL)
		corrupt(seg, "The global order of leaf %d is not there.", col);
	crows = g->hdr.crows;
	if (vs >= ve)
		return ve;
	/* the chunks after the first one of the range start inside it: the last of them whose first key is before key (or equal to it, for an upper bound) holds the answer or ends before it */
	c0 = vs / crows;
	lo = c0 + 1;
	hi = (ve - 1) / crows + 1;
	gord_dir_read(seg, g);
	SIFT_BISECT(uint32, mid, lo, hi, after ? gord_dirent(seg, g, col, mid).first <= key : gord_dirent(seg, g, col, mid).first < key);
	lo--;
	m = gorder_chunk_memo(seg, col, lo, memo);
	from = Max(vs, lo * crows);
	to = Min(ve, lo * crows + m);
	for (uint32 i = from; i < to; i++)
		if (after ? memo->key[i - lo * crows] > key : memo->key[i - lo * crows] >= key)
			return i;
	return to;
}

uint32
sift_cseg_gorder_seek(SiftColSegment *seg, int col, uint32 vs, uint32 ve, int64 key, bool after, SiftCGOrdBuf *keep)
{
	SiftCGOrdBuf memo;

	memo.at = UINT32_MAX;
	return gorder_seek(seg, col, vs, ve, key, after, keep != NULL ? keep : &memo);
}

/* The entries [*a, *b) of the global order of leaf col (ungrouped) whose key lies in [lo, hi]; false when the segment has no such block */
static bool
gorder_span(SiftColSegment *seg, int col, int64 lo, int64 hi, uint32 *a, uint32 *b, SiftCGOrdBuf *memo)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	/* the keys of a text block are ranks of the segment's strings, not the codes of its dictionary that a range of the scan is in */
	if (g == NULL || g->hdr.gcol >= 0 || g->hdr.strbytes != 0)
		return false;
	*a = lo > hi ? 0 : gorder_seek(seg, col, 0, g->hdr.values, lo, false, memo);
	*b = lo > hi ? 0 : gorder_seek(seg, col, *a, g->hdr.values, hi, true, memo);
	return true;
}

/* How many rows of the segment have a key in [lo, hi], by the global order of the leaf; UINT32_MAX when it has none */
uint32
sift_cseg_gorder_count(SiftColSegment *seg, int col, int64 lo, int64 hi)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b;

	memo.at = UINT32_MAX;
	return gorder_span(seg, col, lo, hi, &a, &b, &memo) ? b - a : UINT32_MAX;
}

/*
 * The heap addresses of the rows whose key lies in [lo, hi], from the global order of leaf col (ungrouped), ascending: the two bounds are sought in the directory and
 * one chunk each, and the chunks between are decoded. False when the segment has no such block or the range holds more than limit entries (the caller reads the units).
 */
bool
sift_cseg_gorder_tids(SiftColSegment *seg, int col, int64 lo, int64 hi, uint32 limit, SiftKey **tids, uint32 *ntids)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b,
				n = 0,
				crows;
	SiftKey    *t;

	memo.at = UINT32_MAX;
	if (!gorder_span(seg, col, lo, hi, &a, &b, &memo) || b - a > limit)
		return false;
	crows = seg_gord_leaf(seg, col)->hdr.crows;
	t = palloc(Max(b - a, 1) * sizeof(SiftKey));
	for (uint32 c = a / crows; a < b && c <= (b - 1) / crows; c++)
	{
		uint32		from = Max(a, c * crows),
					to = Min(b, (c + 1) * crows);

		(void) gorder_chunk_memo(seg, col, c, &memo);
		for (uint32 i = from; i < to; i++)
			t[n++] = memo.addr[i - c * crows];
	}
	if (n > 1)
		sift_sort_u64(t, n);
	*tids = t;
	*ntids = n;
	return true;
}

/* The global order of a leaf holds every row of the segment once, the values by key then address, the rows with no value by address */
static void
verify_gorder(SiftColSegment *seg, int col, uint32 total)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
	uint32		have = UINT32_MAX;
	int64		pk = 0;
	uint64		pa = 0;
	uint32		nranges;
	SiftCGOrdGrp *grp = NULL;

	if (g == NULL)
		return;
	if (g->hdr.strbytes != 0)
		(void) gord_strs(seg, g, col);
	gord_dir_read(seg, g);
	if (g->hdr.gcol < 0 ? g->hdr.entries != total : g->hdr.entries > total)
		corrupt(seg, "The global order of leaf %d holds %u entries for %u rows.", col, g->hdr.entries, total);
	/* the chunks follow one another, and the first keys of a block that is not grouped ascend */
	{
		uint64		next = SIFT_CGORD_HDR + (uint64) g->hdr.chunks * SIFT_CGORD_DIRENT;
		uint32		vchunks = (g->hdr.values + g->hdr.crows - 1) / g->hdr.crows;
		int64		pf = 0;

		for (uint32 c = 0; c < g->hdr.chunks; c++)
		{
			SiftCGOrdDir d = gord_dirent(seg, g, col, c);

			if (d.off != next || (g->hdr.gcol < 0 && c > 0 && c < vchunks && d.first < pf))
				corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, c);
			pf = d.first;
			next = (uint64) d.off + ALIGN8((uint64) d.klen) + ALIGN8((uint64) d.alen);
		}
	}
	/* the entries of a range ascend by (key, address), those of a NULL range by address; a block that is not grouped is two ranges, the values and the NULL rows */
	if (g->hdr.gcol >= 0)
	{
		uint8	   *gb = palloc((Size) g->hdr.ngroups * SIFT_CGORD_GRPENT);

		grp = palloc((Size) g->hdr.ngroups * sizeof(SiftCGOrdGrp));
		seg_read(seg, g->at + g->gat, (Size) g->hdr.ngroups * SIFT_CGORD_GRPENT, gb);
		if (!sift_cgord_groups(gb, &g->hdr, (uint64) g->hdr.ngroups * SIFT_CGORD_GRPENT, grp))
			corrupt(seg, "The global order of leaf %d has malformed groups.", col);
		pfree(gb);
	}
	nranges = g->hdr.gcol < 0 ? 2 : 2 * g->hdr.ngroups;
	for (uint32 r = 0; r < nranges; r++)
	{
		bool		isnull = g->hdr.gcol < 0 ? r == 1 : r % 2 == 1;
		uint32		from,
					count;

		if (g->hdr.gcol < 0)
		{
			from = isnull ? g->hdr.values : 0;
			count = isnull ? g->hdr.entries - g->hdr.values : g->hdr.values;
		}
		else
		{
			const SiftCGOrdGrp *gr = &grp[r / 2];

			from = isnull ? gr->nstart : gr->vstart;
			count = isnull ? gr->ncount : gr->vcount;
		}
		for (uint32 p = from; p < from + count; p++)
		{
			if (p / g->hdr.crows != have)
			{
				have = p / g->hdr.crows;
				(void) sift_cseg_gorder_chunk(seg, col, have, key, addr);
			}
			if (p != from && (isnull ? addr[p % g->hdr.crows] <= pa : key[p % g->hdr.crows] < pk || (key[p % g->hdr.crows] == pk && addr[p % g->hdr.crows] <= pa)))
				corrupt(seg, "The global order of leaf %d is out of order at entry %u.", col, p);
			pk = key[p % g->hdr.crows];
			pa = addr[p % g->hdr.crows];
			if (!isnull && g->hdr.strbytes != 0 && (uint64) pk >= g->nstr)
				corrupt(seg, "The global order of leaf %d has the rank %lld of %u strings at entry %u.", col, (long long) pk, g->nstr, p);
			if (p % 4096 == 0)
				CHECK_FOR_INTERRUPTS();
		}
	}
	/* the strings of a text block: every one is there, and each is at most a dictionary entry long */
	if (g->hdr.strbytes != 0)
	{
		uint32		len;

		for (uint32 r = 0; r < g->nstr; r++)
		{
			(void) sift_cseg_gorder_str(seg, col, r, &len);
			if (r % 4096 == 0)
				CHECK_FOR_INTERRUPTS();
		}
	}
}

/*
 * The entries of a packed table: the directory ascends from 0 to the entries, the remainders ascend in a bucket and the codes in a run of
 * one remainder, a row is a row of the segment and an address is in a unit of it. Returns the distinct k.
 */
static uint32
verify_packed(SiftColSegment *seg, int c, const PointLeaf *pl, uint32 total)
{
	const SiftCPackPar *par = &pl->pk;
	uint32		width = par->rbits + par->pbits,
				keys = 0,
				chunk = 4096;
	uint64		dir = ((uint64) (par->nbuckets + 1) * 4 + 7) & ~UINT64CONST(7),
				prem = 0,
				pcode = 0;
	uint32	   *starts = palloc((uint64) (par->nbuckets + 1) * sizeof(uint32));
	uint8	   *buf = palloc((uint64) chunk * width / 8 + 24);
	uint32		b = 0;

	seg_read(seg, pl->at, (uint64) (par->nbuckets + 1) * sizeof(uint32), starts);
	if (starts[0] != 0 || starts[par->nbuckets] != pl->nslots)
		corrupt(seg, "The packed point directory of leaf %d starts at entry %u and ends at %u of %u.", c, starts[0], starts[par->nbuckets], pl->nslots);
	for (uint32 i = 0; i < par->nbuckets; i++)
		if (starts[i] > starts[i + 1])
			corrupt(seg, "The packed point directory of leaf %d has the bucket %u from entry %u to %u.", c, i, starts[i], starts[i + 1]);
	for (uint32 e = 0; e < pl->nslots; e += chunk)
	{
		uint32		m = Min(chunk, pl->nslots - e);
		uint64		first = (uint64) e * width / 8,
					last = ((uint64) (e + m) * width + 7) / 8;

		memset(buf, 0, last - first + 8);
		seg_read(seg, pl->at + dir + first, last - first, buf);
		for (uint32 j = 0; j < m; j++)
		{
			uint64		at = (uint64) (e + j) * width - first * 8,
						rem = sift_cpoint_pk_get(buf, at, par->rbits),
						code = sift_cpoint_pk_get(buf, at + par->rbits, par->pbits);

			while (starts[b + 1] <= e + j)
				b++;
			if (e + j != starts[b] && rem < prem + (code <= pcode && rem == prem))
				corrupt(seg, "The packed point directory of leaf %d has entry %u out of order.", c, e + j);
			keys += e + j == starts[b] || rem != prem;
			prem = rem;
			pcode = code;
			if (pl->tids)
			{
				uint64		key = sift_cpoint_pk_tid(par, code);
				uint32		sb = (uint32) (key >> 16) >> SIFT_SB_SHIFT,
							u = sift_cseg_unit_of_sb(seg, sb);

				if ((key >> 16) > UINT32_MAX || u >= seg->nsb || (key & 0xFFFF) == 0 || (key & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF)
					corrupt(seg, "The packed point directory of leaf %d lists the address %llx, which is in no unit of the segment.", c, (unsigned long long) key);
			}
			else if (code >= total)
				corrupt(seg, "The packed point directory of leaf %d lists row %llu of %u.", c, (unsigned long long) code, total);
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(starts);
	pfree(buf);
	return keys;
}

void
sift_cseg_verify_points(SiftColSegment *seg)
{
	const PointLeaf *pl = seg_points_read(seg);
	uint32		total = seg->nsb > 0 ? seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows : 0;
	uint64		win[1024];
	uint32		rows[1024];

	for (int c = 0; pl && c < seg->schema->ncols; c++)
	{
		uint64		listed = 0,
					words = 0;
		uint32		keys = 0;
		uint64		lists = pl[c].at + (uint64) pl[c].nslots * sizeof(uint64);

		if (pl[c].nslots == 0)
			continue;
		if (pl[c].packed)
		{
			if (verify_packed(seg, c, &pl[c], total) != pl[c].nkeys)
				corrupt(seg, "The packed point directory of leaf %d has a different number of distinct keys than its record %u.", c, pl[c].nkeys);
			continue;
		}
		if (pl[c].exact && seg->entry.format < SIFT_COL_FORMAT_EXACTSLOT)
			corrupt(seg, "The point directory of leaf %d has an exact slot count in a segment of column format 1.%d.", c, seg->entry.format);
		((PointLeaf *) pl)[c].ndense = 0;
		for (uint32 p = 0; p < pl[c].nslots; p += lengthof(win))
		{
			uint32		m = Min(lengthof(win), pl[c].nslots - p);

			seg_read(seg, pl[c].at + (uint64) p * sizeof(uint64), m * sizeof(uint64), win);
			for (uint32 j = 0; j < m; j++)
			{
				uint32		payload = (uint32) win[j],
							cnt = 1;

				if (win[j] == SIFT_CPOINT_EMPTY)
					continue;
				keys++;
				if (pl[c].tids)
				{
					TidAcc		a = {.seg = seg,.col = c,.pl = &pl[c]};

					seg_tid_slot_read(&a, win[j]);
					for (uint32 i = 1; i < a.n; i++)
						if (a.t[i] <= a.t[i - 1])
							corrupt(seg, "The point directory of leaf %d lists an address twice or out of order.", c);
					cnt = a.n;
					words += a.words;
					pfree(a.t);
				}
				else if ((payload & SIFT_CPOINT_LIST) && pl[c].zrows)
				{
					uint32		nw;

					pfree(seg_point_list_read(seg, c, &pl[c], payload & ~SIFT_CPOINT_LIST, total - 1, UINT32_MAX, &cnt, &nw));
					words += nw;
				}
				else if (payload & SIFT_CPOINT_LIST)
				{
					uint32		prev = 0;

					cnt = seg_point_list_count(seg, c, &pl[c], payload & ~SIFT_CPOINT_LIST);
					words += 1 + cnt;
					for (uint32 i = 0, got; i < cnt; i += got)
					{
						got = Min(lengthof(rows), cnt - i);
						seg_read(seg, lists + (uint64) ((payload & ~SIFT_CPOINT_LIST) + 1 + i) * sizeof(uint32), got * sizeof(uint32), rows);
						for (uint32 k = 0; k < got; k++)
						{
							if (rows[k] >= total || (i + k > 0 && rows[k] <= prev))
								corrupt(seg, "The point directory of leaf %d lists row %u of %u out of order.", c, rows[k], total);
							prev = rows[k];
						}
					}
				}
				else if (payload == SIFT_CPOINT_DENSE)
				{
					cnt = 1;
					((PointLeaf *) pl)[c].ndense++;
				}
				else if (payload >= total)
					corrupt(seg, "The point directory of leaf %d lists row %u of %u.", c, payload, total);
				listed += cnt;
			}
			CHECK_FOR_INTERRUPTS();
		}
		if (keys != pl[c].nkeys || listed != pl[c].count || words != pl[c].words)
			corrupt(seg, "The point directory of leaf %d lists %u keys, %llu rows and %llu words, its record %u, %u and %u.", c, keys, (unsigned long long) listed,
					(unsigned long long) words, pl[c].nkeys, pl[c].count, pl[c].words);
	}
	for (int c = 0; c < 2 * seg->schema->ncols; c++)
		verify_gorder(seg, c, total);
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
sift_cseg_pre_census(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, uint32 sample, SiftCPaggCensus *c)
{
	/* the segment is minor 7 only when the setting allows and the kinds of its rows are known: what a rewrite of it writes */
	bool		minmax = spec->minmax && spec->ndims > 0 && setting >= SIFT_COL_FORMAT_MINMAX && sift_cseg_kinds_exact(seg);
	uint32		n = sample > 0 ? Min(sample, seg->nsb) : seg->nsb;

	memset(c, 0, sizeof(*c));
	for (uint32 k = 0; k < n; k++)
	{
		uint32		u = (uint64) k * seg->nsb / n;	/* the units counted are spread over the segment */
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

/*
 * Narrows keep for the units [ulo, uhi) to those whose zone may pass t. The passes run over the words of keep that hold the range (the units of those words
 * outside it are narrowed too, which is exact), so that a begin of two superblocks of a segment of thousands tests those only: an aggregate begins for every
 * morsel, and a begin that tested the whole segment made the scan quadratic in its units.
 */
static void
prune_words(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut, const ZoneArrays *z, uint32 lo, uint32 hi, bool sets)
{
	uint32		n = hi - lo,
				nw = (n + 63) / 64;
	uint64	   *kw = keep + lo / 64;
	uint64	   *tmp = palloc(SIFT_COL_NWORDS(n) * sizeof(uint64));
	uint64	   *save = NULL;

	if (z->any_ovf || z->any_inl)
	{
		save = palloc(nw * sizeof(uint64));
		memcpy(save, kw, nw * sizeof(uint64));
	}
	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(kw, 0, nw * sizeof(uint64));
			break;
		case SIFT_CT_SET:			/* lo..hi bound the keys that can match; a unit whose zone holds none of them is out */
		case SIFT_CT_RANGE:
			sift_colk->zone_range(z->min + lo, z->max + lo, n, t->lo, t->hi, tmp);
			for (uint32 w = 0; w < nw; w++)	/* not bits_and: its vectors round nw up and would reach the words past the window */
				kw[w] &= tmp[w];
			if (t->kind == SIFT_CT_SET)
				for (uint32 u = lo; u < hi; u++)
					if ((keep[u / 64] >> (u % 64) & 1) && z->nnull[u] < z->nvals[u] && !sift_set_meets_i64(t->set, t->nset, z->min[u], z->max[u]))
						keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set + lo, n, t->fold, kw);
			if (sets)
				sift_cseg_prune_sets(seg, col, t, keep, Max(ulo, lo), Min(uhi, hi), setcut);
			break;
		case SIFT_CT_LUT:
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set + lo, n, t->fold, kw);
			if (sets)
				sift_cseg_prune_sets(seg, col, t, keep, Max(ulo, lo), Min(uhi, hi), setcut);
			break;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
		case SIFT_CT_NSET:
		case SIFT_CT_STRS:
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			break;
		case SIFT_CT_ISNULL:
			for (uint32 u = lo; u < hi; u++)
				if (z->nnull[u] == 0)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
		case SIFT_CT_NRANGE:
			for (uint32 u = lo; u < hi; u++)
				if (z->nnull[u] < z->nvals[u] && z->min[u] >= t->lo && z->max[u] <= t->hi)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
				else if (z->nnull[u] == z->nvals[u])
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
	}
	if (save)
	{
		/* an inline unit has no codes: the passes above said nothing about it, its zone is the strings' */
		for (uint32 u = lo; z->any_inl && u < hi; u++)
			if (z->flags[u] & SIFT_COLZ_INLINE)
			{
				SiftCZone	uz = {.min = z->min[u], .max = z->max[u], .nnull = z->nnull[u], .nvals = z->nvals[u], .flags = z->flags[u]};

				if (sift_ctest_zone(t, &uz))
					keep[u / 64] |= save[(u - lo) / 64] & (UINT64CONST(1) << (u % 64));
				else
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			}
		/* units with overflow values cannot be pruned by this leaf: their zone does not describe the heap */
		for (uint32 u = lo; z->any_ovf && u < hi; u++)
			if (z->flags[u] & SIFT_COLZ_OVERFLOW)
				keep[u / 64] |= save[(u - lo) / 64] & (UINT64CONST(1) << (u % 64));
		pfree(save);
	}
	pfree(tmp);
}

void
sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut, bool sets)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	uint32		lo = ulo / 64 * 64,
				hi;
	uint32		w0,
				w1;

	uhi = Min(uhi, seg->nsb);
	if (ulo >= uhi)
		return;
	hi = Min(seg->nsb, (uhi + 63) / 64 * 64);
	w0 = lo / 64;
	w1 = (hi + 63) / 64;
	/*
	 * The zones of a word of 64 units (their smallest minimum and largest maximum) clear the whole word of a range or list that none of them meets, which a unit
	 * by unit test would find word by word: a request that keeps a few units of a table in order tests the words, not the units. A word with an overflow unit is
	 * left to the unit tests.
	 */
	if (z->wmin && (t->kind == SIFT_CT_RANGE || t->kind == SIFT_CT_SET))
		for (uint32 w = w0; w < w1; w++)
			if (keep[w] && !(z->wbad[w / 64] >> (w % 64) & 1) && (z->wmax[w] < t->lo || z->wmin[w] > t->hi))
				keep[w] = 0;
	/* the tests only clear bits: the words nothing keeps are left alone, and the others are tested in runs */
	for (uint32 w = w0; w < w1;)
	{
		uint32		e;

		if (!keep[w] && t->kind != SIFT_CT_NONE)
		{
			w++;
			continue;
		}
		for (e = w + 1; e < w1 && (keep[e] || t->kind == SIFT_CT_NONE); e++)
			;
		prune_words(seg, col, t, keep, ulo, uhi, setcut, z, w * 64, Min(seg->nsb, e * 64), sets);
		w = e;
	}
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
 * a scan must read liveness after it reads the all-visible mask */
bool
unit_dirty(SiftColSegment *seg, uint32 u)
{
	uint32		i;
	Buffer		buf = InvalidBuffer;
	uint8		b;

	if (u >= seg->nsb || seg->sbdir[u].id < seg->hdr->sb_lo)
		corrupt(seg, "Unit %u of %u (SB %u, the segment's SBs start at %u) is not a unit of this segment.", u, seg->nsb, u < seg->nsb ? seg->sbdir[u].id : 0, seg->hdr->sb_lo);
	i = seg->sbdir[u].id - seg->hdr->sb_lo;
	b = sift_live_byte(&PRIV(seg)->live, sizeof(SiftLiveHeaderData) + i / 8, &buf);

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
	uint32		n,
				nw;

	if (u >= seg->nsb)
		corrupt(seg, "Unit %u of %u is not a unit of this segment.", u, seg->nsb);
	n = seg->sbdir[u].nrows;
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
		uint64		at = seg_sec(seg, SIFT_SEC_DOCMAP)->off + pv->vers_at;
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
			corrupt(seg, "The versions of unit %u (SB %u) do not decode as %u rows.", u, seg->sbdir[u].id, n);
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
	uint32		pg = blk % SIFT_SB_PAGES;

	if (!sift_cseg_unit_view(seg, u, &v))
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, seg->sbdir[u].id);
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
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, seg->sbdir[u].id);
	sift_cunit_keys(&v, seg->sbdir[u].id, keys);
}
