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
