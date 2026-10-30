/*
 * col/seg/seg.c - the segment reader: open, close, the head, the unit directory, a unit
 * and its view; every section of a segment is read by the file
 * next to it that names it
 */
#include "core/host.h"
#include "col/hll.h"
#include "col/ktab.h"
#include "col/setsum.h"
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

/* ---------- small-set summaries ---------- */

/*
 * The records of a section of blobs per leaf and unit (small-set summaries, sorted runs, sketches), read and checked with the whole section on first use: the
 * record of each leaf in an array of ncols (dir_off 0: the leaf has none)
 */
static SetsLeaf *
blob_records(SiftColSegment *seg, const char *what, uint32 magic, uint64 off, uint64 room, uint32 flags_ok,
			 bool (*leaf_ok) (const SiftColSchema *, const SiftColDef *))
{
	int			ncols = seg->schema->ncols;
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICTIDX);
	uint64		at = s->off + PRIV(seg)->pagg_end + off,
				lowest;
	SetsHeader	sh;
	SetsLeaf   *rec,
			   *out;
	int			prev = -1;

	seg_read(seg, at, sizeof(sh), &sh);
	if (sh.magic != magic || sh.version != 1 || sh.nsb != seg->nsb || sh.nleaves < 1 || sh.nleaves > (uint32) ncols ||
		sizeof(sh) + (uint64) sh.nleaves * sizeof(SetsLeaf) > room)
		corrupt(seg, "The %s start with 0x%08x, version %u, %u leaves, %u units.", what, sh.magic, sh.version, sh.nleaves, sh.nsb);
	rec = palloc(sh.nleaves * sizeof(SetsLeaf));
	seg_read(seg, at + sizeof(sh), sh.nleaves * sizeof(SetsLeaf), rec);
	lowest = off + sizeof(sh) + sh.nleaves * sizeof(SetsLeaf);
	out = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(SetsLeaf));
	for (uint32 i = 0; i < sh.nleaves; i++)
	{
		const SetsLeaf *r = &rec[i];
		uint64		rel = r->dir_off - off;

		if (r->leaf >= (uint32) ncols || (int) r->leaf <= prev || r->flags == 0 || (r->flags & ~flags_ok) || !leaf_ok(seg->schema, &seg->schema->cols[r->leaf]))
			corrupt(seg, "The %s record %u names leaf %u, which cannot have one.", what, i, r->leaf);
		if (r->dir_off < lowest || r->dir_off % 8 != 0 ||
			!seg_within(room, rel, (uint64) (seg->nsb + 1) * sizeof(uint64), r->bytes))
			corrupt(seg, "The %s of leaf %u (%llu bytes at %llu) lie outside the section.", what, r->leaf, ULL(r->bytes), ULL(r->dir_off));
		lowest = ALIGN8(r->dir_off + (uint64) (seg->nsb + 1) * sizeof(uint64) + r->bytes);
		prev = r->leaf;
		out[r->leaf] = *r;
	}
	/* the records are all there are: one fewer would leave the last leaf's blobs unread and unnoticed */
	if (lowest != off + room)
		corrupt(seg, "The %s end at %llu, not at the %llu the section has.", what, ULL(lowest), ULL(off + room));
	pfree(rec);
	return out;
}

/* The record of leaf col in the section, read and checked with the whole section on first use; NULL when the leaf has no summary in any unit */
const SetsLeaf *
seg_set_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_SETS)
		return NULL;
	h = seg_pagg_header(seg);
	if (h->version < 3 || !PAGG_SETS(h))
		return NULL;
	if (pv->sets == NULL)
	{
		pv->sets = blob_records(seg, "small-set summaries", SETS_MAGIC, PAGG_SETS(h), PAGG_SETS_BYTES(h), SETS_EXACT | SETS_BLOOM | SETS_PRES, sift_cset_leaf_ok);
		pv->setdir = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(uint64 *));
	}
	return pv->sets[col].dir_off ? &pv->sets[col] : NULL;
}

const uint8 *
sift_cseg_set(SiftColSegment *seg, int col, uint32 u, uint32 *len)
{
	const SetsLeaf *sl = seg_set_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	const uint8 *blob;

	*len = 0;
	if (sl == NULL)
		return NULL;
	blob = seg_unit_blob(seg, seg->nsb, u, sl->dir_off, sl->bytes, &pv->setdir[col], &pv->setbuf, &pv->setcap, len);
	/* the blobs of a leaf end where its record says: a smaller end would drop the last unit's blob unnoticed */
	if (pv->setdir[col] != NULL && pv->setdir[col][seg->nsb] != sl->bytes)
		corrupt(seg, "The directory of the small-set summaries of leaf %d ends at %llu, not at %llu.", col, (unsigned long long) pv->setdir[col][seg->nsb], (unsigned long long) sl->bytes);
	return blob;
}

bool
sift_cseg_presence(SiftColSegment *seg, int col, uint32 u, SiftCPresence *p)
{
	uint32		len;
	const uint8 *blob = sift_cseg_set(seg, col, u, &len);
	SiftCSet	s;

	if (blob == NULL)
		return false;
	if (!sift_cset_open(blob, len, &s))
		corrupt(seg, "The small-set summary of leaf %d in unit %u is malformed.", col, u);
	memset(p, 0, sizeof(*p));
	if (s.kind == SIFT_CSET_PLIST || s.kind == SIFT_CSET_RUNS)
	{
		p->nkeys = s.n;
		p->keys = palloc(s.n * sizeof(int64));
		sift_cset_members(&s, p->keys);
		return true;
	}
	if (s.kind != SIFT_CSET_PRES)
		return false;
	p->wbase = s.base / 64;
	p->nwords = s.nbits / 64;
	p->words = s.data;
	return true;
}

/* ---------- HyperLogLog sketches ---------- */

static const SetsLeaf *
hll_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_HLL)
		return NULL;
	h = seg_pagg_header(seg);
	if (h->version < 7 || !PAGG_HLL(h))
		return NULL;
	if (pv->hlls == NULL)
	{
		pv->hlls = blob_records(seg, "sketches", HLL_MAGIC, PAGG_HLL(h), PAGG_HLL_BYTES(h), HLL_LIST | HLL_DENSE, sift_hll_leaf_ok);
		pv->hlldir = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(uint64 *));
	}
	return pv->hlls[col].dir_off ? &pv->hlls[col] : NULL;
}

const uint8 *
sift_cseg_hll(SiftColSegment *seg, int col, uint32 u, uint32 *len)
{
	const SetsLeaf *sl = hll_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	const uint8 *blob;

	*len = 0;
	if (sl == NULL)
		return NULL;
	blob = seg_unit_blob(seg, seg->nsb, u, sl->dir_off, sl->bytes, &pv->hlldir[col], &pv->hllbuf, &pv->hllcap, len);
	if (pv->hlldir[col][seg->nsb] != sl->bytes)
		corrupt(seg, "The directory of the sketches of leaf %d ends at %llu, not at %llu.", col, ULL(pv->hlldir[col][seg->nsb]), ULL(sl->bytes));
	if (blob != NULL && !sift_hll_check(blob, *len))
		corrupt(seg, "The sketch of leaf %d in unit %u is malformed.", col, u);
	return blob;
}

/* The key tables' bytes at *off, from the file start; false when the segment has none */
bool
seg_ktab_span(SiftColSegment *seg, uint64 *off, uint64 *len)
{
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_KTAB || (h = seg_pagg_header(seg))->version < 8 || !PAGG_KTAB(h))
		return false;
	*off = seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_KTAB(h);
	*len = PAGG_KTAB_BYTES(h);
	return true;
}

/* The key tables of the segment, read and decoded with the section on first use, kept with the segment: leaf by leaf, NULL where a leaf has none */
const SiftCHistDim *
sift_cseg_ktab(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	uint64		off,
				len;

	if (!seg_ktab_span(seg, &off, &len))
		return NULL;
	if (pv->ktdim == NULL)
	{
		uint8	   *buf = palloc(len);
		SiftCHistDim **dim = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(SiftCHistDim *));

		seg_read(seg, off, len, buf);
		for (uint64 at = 0; at < len;)
		{
			SiftCHistDim *d = MemoryContextAlloc(seg->mcxt, sizeof(SiftCHistDim));
			uint32		n = sift_ktab_decode(seg->schema, buf + at, len - at, seg->mcxt, d);

			if (n == 0 || dim[d->leaf] != NULL)
				corrupt(seg, "The key table at %llu of the section is malformed.", ULL(at));
			dim[d->leaf] = d;
			at += n;
		}
		pfree(buf);
		pv->ktdim = dim;
	}
	return pv->ktdim[col];
}

uint32
sift_cseg_hll_kinds(SiftColSegment *seg, int col)
{
	const SetsLeaf *sl = hll_leaf(seg, col);

	return sl ? sl->flags : 0;
}

/* ---------- sorted runs ---------- */

/* The record of leaf col in the section, read and checked with the whole section on first use; NULL when the leaf has no run in any unit */
const RunsLeaf *
seg_run_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_RUNS)
		return NULL;
	h = seg_pagg_header(seg);
	if (h->version < 5 || !PAGG_RUNS(h))
		return NULL;
	if (pv->runs == NULL)
	{
		pv->runs = blob_records(seg, "sorted runs", RUNS_MAGIC, PAGG_RUNS(h), PAGG_RUNS_BYTES(h), RUNS_ORD | RUNS_IDENT, sift_crun_leaf_ok);
		pv->rundir = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(uint64 *));
	}
	return pv->runs[col].dir_off ? &pv->runs[col] : NULL;
}

bool
sift_cseg_run(SiftColSegment *seg, int col, uint32 u, SiftCRun *r, uint64 *at)
{
	const RunsLeaf *rl = seg_run_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	uint64		base,
				b0,
				b1;
	uint8		hdr[SIFT_CRUN_HDR];

	if (rl == NULL)
		return false;
	base = seg_sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + rl->dir_off;
	if (pv->rundir[col] == NULL)
	{
		uint64	   *dir = MemoryContextAlloc(seg->mcxt, (seg->nsb + 1) * sizeof(uint64));

		seg_read(seg, base, (seg->nsb + 1) * sizeof(uint64), dir);
		for (uint32 i = 0; i < seg->nsb; i++)
			if (dir[i] > dir[i + 1] || dir[i + 1] > rl->bytes)
				corrupt(seg, "The sorted-run directory of leaf %d is out of order at unit %u.", col, i);
		pv->rundir[col] = dir;
	}
	b0 = pv->rundir[col][u];
	b1 = pv->rundir[col][u + 1];
	if (b1 == b0)
		return false;
	*at = base + (seg->nsb + 1) * sizeof(uint64) + b0;
	if (b1 - b0 > UINT32_MAX)
		corrupt(seg, "The sorted run of leaf %d in unit %u is %llu bytes.", col, u, ULL(b1 - b0));
	seg_read(seg, *at, SIFT_CRUN_HDR < b1 - b0 ? SIFT_CRUN_HDR : b1 - b0, hdr);
	if (!sift_crun_head(hdr, (uint32) (b1 - b0), seg->sbdir[u].nrows, r))
		corrupt(seg, "The sorted run of leaf %d in unit %u is malformed.", col, u);
	return true;
}

uint32
sift_cseg_set_kinds(SiftColSegment *seg, int col)
{
	const SetsLeaf *sl = seg_set_leaf(seg, col);

	return sl ? sl->flags : 0;
}

uint32
sift_cseg_run_kinds(SiftColSegment *seg, int col)
{
	const RunsLeaf *rl = seg_run_leaf(seg, col);

	return rl ? rl->flags : 0;
}

void
sift_cseg_run_blob(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint8 *dst)
{
	seg_read(seg, at, sift_crun_total(r), dst);
}

/* The head of the run (r->heads > 0): the entries nlo from the low end, then nhi from the high end, the last of the run first */
void
sift_cseg_run_heads(SiftColSegment *seg, uint64 at, const SiftCRun *r, SiftCHeadEnt *out)
{
	uint32		nlo,
				nhi;

	sift_crun_head_counts(r->heads, r->n, &nlo, &nhi);
	seg_read(seg, at + sift_crun_bytes(r->kind, r->n), (nlo + nhi) * sizeof(SiftCHeadEnt), out);
}

/* The raw entry bytes [pos, pos + cnt) of an ORD run (width bytes each), for the caller's own search */
void
sift_cseg_run_bytes_read(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint32 pos, uint32 cnt, uint8 *dst)
{
	seg_read(seg, at + SIFT_CRUN_HDR + (uint64) pos * r->width, (Size) cnt * r->width, dst);
}

void
sift_cseg_run_read(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint32 pos, uint32 cnt, uint32 *out)
{
	uint8	   *buf = NULL;

	if (r->kind == SIFT_CRUN_ORD)
	{
		buf = palloc((Size) cnt * r->width + 1);
		seg_read(seg, at + SIFT_CRUN_HDR + (uint64) pos * r->width, (Size) cnt * r->width, buf);
	}
	if (!sift_crun_entries(r, buf, pos, cnt, out))
		corrupt(seg, "The sorted run entries %u to %u are not rows of its unit of %u.", pos, pos + cnt, r->n);
	if (buf)
		pfree(buf);
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
	if ((sl = seg_set_leaf(seg, col)) == NULL)
		return;
	/* a filter answers only a key: a range of more keys or a LUT of many matches is left to the zones when the leaf has no exact set */
	if (!(sl->flags & (SETS_EXACT | SETS_PRES)) && ((t->kind == SIFT_CT_RANGE && (uint64) t->hi - (uint64) t->lo >= SIFT_CSET_PROBES) || (t->kind == SIFT_CT_LUT && ncodes < 0)))
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

/* a[0, n) ascending: insertion for a short chunk, else a radix sort of bytes (a callback sort costs ten times the instructions on the codes of a chunk) */
static void
sort_u32(uint32 *a, uint32 n)
{
	uint32		max = 0,
			   *tmp,
			   *from = a,
			   *to;

	if (n <= 48)
	{
		for (uint32 i = 1; i < n; i++)
		{
			uint32		v = a[i],
						j = i;

			for (; j > 0 && a[j - 1] > v; j--)
				a[j] = a[j - 1];
			a[j] = v;
		}
		return;
	}
	for (uint32 i = 0; i < n; i++)
		max |= a[i];
	to = tmp = palloc(n * sizeof(uint32));
	for (int shift = 0; shift < 32 && (max >> shift) != 0; shift += 8)
	{
		uint32		cnt[256] = {0},
					sum = 0,
				   *t;

		for (uint32 i = 0; i < n; i++)
			cnt[(from[i] >> shift) & 255]++;
		for (int d = 0; d < 256; d++)
		{
			uint32		c = cnt[d];

			cnt[d] = sum;
			sum += c;
		}
		for (uint32 i = 0; i < n; i++)
			to[cnt[(from[i] >> shift) & 255]++] = from[i];
		t = from;
		from = to;
		to = t;
	}
	if (from != a)
		memcpy(a, from, n * sizeof(uint32));
	pfree(tmp);
}

/*
 * The codes of the list at word off of the leaf's lists in the coding of sift_cpoint_put_list (heap addresses, or rows of a table of the values of a list): ascending,
 * none past max. NULL, with the count in *cnt, when the list holds more than room codes. *nwords is the words the list takes.
 */
static uint64 *
point_list_read(SiftColSegment *seg, int col, const PointLeaf *pl, uint32 off, uint64 max, uint32 room, uint32 *cnt, uint32 *nwords)
{
	uint32		hdr[2],
				pad;
	uint64		base,
				prev = 0;
	uint8	   *buf;
	const uint8 *p,
			   *end;
	uint64	   *code;

	if (off >= pl->words || pl->words - off < 2)
		corrupt(seg, "The point directory of leaf %d has a list at word %u of %u.", col, off, pl->words);
	base = pl->at + (uint64) pl->nslots * sizeof(uint64) + (uint64) off * sizeof(uint32);
	seg_read(seg, base, sizeof(hdr), hdr);
	if (hdr[0] < 2 || hdr[0] > pl->count || hdr[1] < hdr[0] || hdr[1] > 6 * hdr[0] || (uint64) off + 2 + (hdr[1] + 3) / 4 > pl->words)
		corrupt(seg, "The point directory of leaf %d has a list of %u codes in %u bytes at word %u of %u.", col, hdr[0], hdr[1], off, pl->words);
	*cnt = hdr[0];
	if (nwords)
		*nwords = 2 + (hdr[1] + 3) / 4;
	if (hdr[0] > room)
		return NULL;
	pad = (hdr[1] + 3) / 4 * 4;
	buf = palloc(pad);
	seg_read(seg, base + sizeof(hdr), pad, buf);
	for (uint32 i = hdr[1]; i < pad; i++)
		if (buf[i] != 0)
			corrupt(seg, "The point directory of leaf %d has a list at word %u whose padding is not zero.", col, off);
	code = palloc(hdr[0] * sizeof(uint64));
	p = buf;
	end = buf + hdr[1];
	for (uint32 i = 0; i < hdr[0]; i++)
	{
		uint64		d;

		if ((p = sift_get_varint64_n(p, end, &d)) == NULL || (i > 0 && d == 0) || d > max || prev + d > max)
			corrupt(seg, "The point directory of leaf %d has a list at word %u that does not hold %u ascending codes.", col, off, hdr[0]);
		code[i] = prev += d;
	}
	if (p != end)
		corrupt(seg, "The point directory of leaf %d has a list at word %u that is longer than its %u codes.", col, off, hdr[0]);
	pfree(buf);
	return code;
}

/* The count of the list at word off of the leaf's lists, checked against the lists' words */
static uint32
point_list_count(SiftColSegment *seg, int col, const PointLeaf *pl, uint32 off)
{
	uint32		cnt;

	if (off >= pl->words)
		corrupt(seg, "The point directory of leaf %d has a list at word %u of %u.", col, off, pl->words);
	seg_read(seg, pl->at + (uint64) pl->nslots * sizeof(uint64) + (uint64) off * sizeof(uint32), sizeof(cnt), &cnt);
	if (cnt < 2 || cnt > pl->count || (uint64) off + 1 + cnt > pl->words)
		corrupt(seg, "The point directory of leaf %d has a list of %u rows at word %u of %u.", col, cnt, off, pl->words);
	return cnt;
}

/*
 * The entries of the packed table of leaf pl that may hold key: those of its bucket with its remainder. fn gets them as the slot of a table of the same mode
 * would have (the fingerprint over the row, or over the address) and the index of the entry as the position.
 */
static void
point_probe_packed(SiftColSegment *seg, int col, const PointLeaf *pl, int64 key, uint64 h, void (*fn) (void *, uint64, uint32), void *arg)
{
	const SiftCPackPar *par = &pl->pk;
	uint32		bucket,
				se[2],
				width = par->rbits + par->pbits,
				nent;
	uint64		rem,
				first;
	uint8	   *buf;
	uint64		dir = ((uint64) (par->nbuckets + 1) * 4 + 7) & ~UINT64CONST(7);

	if (!sift_cpoint_pk_locate(par, key, h, &bucket, &rem))
		return;
	seg_read(seg, pl->at + (uint64) bucket * sizeof(uint32), sizeof(se), se);
	if (se[0] > se[1] || se[1] > pl->nslots)
		corrupt(seg, "The packed point directory of leaf %d has the bucket %u from entry %u to %u of %u.", col, bucket, se[0], se[1], pl->nslots);
	if ((nent = se[1] - se[0]) == 0)
		return;
	first = (uint64) se[0] * width / 8;
	buf = palloc((uint64) (((uint64) se[1] * width + 7) / 8 - first) + 8);
	memset(buf + (((uint64) se[1] * width + 7) / 8 - first), 0, 8);
	seg_read(seg, pl->at + dir + first, ((uint64) se[1] * width + 7) / 8 - first, buf);
	for (uint32 j = 0; j < nent; j++)
	{
		uint64		at = (uint64) (se[0] + j) * width - first * 8;

		if (sift_cpoint_pk_get(buf, at, par->rbits) != rem)
			continue;
		{
			uint64		code = sift_cpoint_pk_get(buf, at + par->rbits, par->pbits);

			fn(arg, pl->tids ? (h & 0xFFFF) << 48 | sift_cpoint_pk_tid(par, code) : (h & 0xFFFFFFFF) << 32 | code, se[0] + j);
		}
	}
	pfree(buf);
}

/* Calls fn(slot payload) for each slot of leaf pl whose fingerprint is that of key's hash: the slots from the position of the hash to the first empty one */
static void
point_probe(SiftColSegment *seg, int col, const PointLeaf *pl, int64 key, void (*fn) (void *, uint64, uint32), void *arg)
{
	uint64		h = sift_cpoint_hash(key);
	uint32		p;

	if (pl->packed)
	{
		point_probe_packed(seg, col, pl, key, h, fn, arg);
		return;
	}
	p = sift_cpoint_pos(h, pl->nslots, pl->exact);
	for (uint32 steps = 0;; steps++)
	{
		uint64		win[8];
		uint32		m = Min(8, pl->nslots - p);
		uint32		j;

		if (steps > pl->nslots / 8 + 1)
			corrupt(seg, "The point directory of leaf %d has no empty slot.", col);
		seg_read(seg, pl->at + (uint64) p * sizeof(uint64), m * sizeof(uint64), win);
		for (j = 0; j < m && win[j] != SIFT_CPOINT_EMPTY; j++)
			if (pl->tids ? sift_cpoint_tid_slot_is(win[j], h) : sift_cpoint_slot_is(win[j], h))
				fn(arg, win[j], p + j);
		if (j < m)
			break;
		p = p + m == pl->nslots ? 0 : p + m;
	}
}

typedef struct PointRows
{
	SiftColSegment *seg;
	int			col;
	const PointLeaf *pl;
	uint32		total;
	uint32	   *r;
	uint32		n;
	uint32		cap;
	bool		over;			/* more rows than a point lookup takes */
} PointRows;

static void
point_add_rows(void *arg, uint64 slot, uint32 pos)
{
	PointRows  *pr = arg;
	uint32		payload = (uint32) slot,
				cnt = 1;
	uint64	   *code = NULL;

	if (pr->over)
		return;
	if (payload == SIFT_CPOINT_DENSE)
	{
		pr->over = true;
		return;
	}
	if ((payload & SIFT_CPOINT_LIST) && pr->pl->zrows)
	{
		code = point_list_read(pr->seg, pr->col, pr->pl, payload & ~SIFT_CPOINT_LIST, pr->total - 1, SIFT_CPOINT_MAX_ROWS - pr->n, &cnt, NULL);
		if (code == NULL)
		{
			pr->over = true;
			return;
		}
	}
	else if (payload & SIFT_CPOINT_LIST)
		cnt = point_list_count(pr->seg, pr->col, pr->pl, payload & ~SIFT_CPOINT_LIST);
	if (pr->n + cnt > SIFT_CPOINT_MAX_ROWS)
	{
		pr->over = true;
		return;
	}
	if (pr->n + cnt > pr->cap)
		pr->r = repalloc(pr->r, (pr->cap = Max(pr->cap * 2, pr->n + cnt)) * sizeof(uint32));
	if (code)
	{
		for (uint32 i = 0; i < cnt; i++)
			pr->r[pr->n + i] = (uint32) code[i];
		pfree(code);
	}
	else if (payload & SIFT_CPOINT_LIST)
		seg_read(pr->seg, pr->pl->at + (uint64) pr->pl->nslots * sizeof(uint64) + (uint64) ((payload & ~SIFT_CPOINT_LIST) + 1) * sizeof(uint32), cnt * sizeof(uint32), pr->r + pr->n);
	else
		pr->r[0 + pr->n] = payload;
	for (uint32 i = pr->n; i < pr->n + cnt; i++)
		if (pr->r[i] >= pr->total)
			corrupt(pr->seg, "The point directory of leaf %d lists row %u of %u.", pr->col, pr->r[i], pr->total);
	pr->n += cnt;
}

bool
sift_cseg_point_rows(SiftColSegment *seg, int col, const int64 *keys, int nkeys, uint32 **rows, uint32 *nrows)
{
	const PointLeaf *pl = seg_points_read(seg);
	PointRows	pr = {.seg = seg,.col = col,.cap = 16};
	uint32		u = 0;

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	pr.pl = pl + col;
	pr.total = seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows;
	pr.r = palloc(pr.cap * sizeof(uint32));
	for (int k = 0; k < nkeys && !pr.over; k++)
		point_probe(seg, col, pr.pl, keys[k], point_add_rows, &pr);
	if (pr.over)
	{
		pfree(pr.r);
		return false;
	}
	sort_u32(pr.r, pr.n);
	for (uint32 i = 0; i < pr.n; i++)
		if (u == 0 || pr.r[u - 1] != pr.r[i])
			pr.r[u++] = pr.r[i];
	*rows = pr.r;
	*nrows = u;
	return true;
}

typedef struct PointHas
{
	SiftColSegment *seg;
	int			col;
	const PointLeaf *pl;
	uint32		row;
	bool		found;
	bool		dense;
} PointHas;

static void
point_find_row(void *arg, uint64 slot, uint32 pos)
{
	PointHas   *ph = arg;
	uint32		payload = (uint32) slot,
				lo = 0,
				hi = 1,
				off = payload & ~SIFT_CPOINT_LIST;
	uint64		base = ph->pl->at + (uint64) ph->pl->nslots * sizeof(uint64);

	if (payload == SIFT_CPOINT_DENSE)
	{
		ph->found = ph->dense = true;
		return;
	}
	if (!(payload & SIFT_CPOINT_LIST))
	{
		ph->found |= payload == ph->row;
		return;
	}
	if (ph->pl->zrows)
	{
		uint64	   *code = point_list_read(ph->seg, ph->col, ph->pl, off, SIFT_CPOINT_SEG_ROWS - 1, UINT32_MAX, &hi, NULL);

		for (uint32 i = 0; i < hi; i++)
			ph->found |= code[i] == ph->row;
		pfree(code);
		return;
	}
	hi = point_list_count(ph->seg, ph->col, ph->pl, off);
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2,
					v;

		seg_read(ph->seg, base + (uint64) (off + 1 + mid) * sizeof(uint32), sizeof(v), &v);
		if (v < ph->row)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < point_list_count(ph->seg, ph->col, ph->pl, off))
	{
		uint32		v;

		seg_read(ph->seg, base + (uint64) (off + 1 + lo) * sizeof(uint32), sizeof(v), &v);
		ph->found |= v == ph->row;
	}
}

uint32
sift_cseg_point_ndense(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl ? pl[col].ndense : 0;
}

bool
sift_cseg_point_has(SiftColSegment *seg, int col, int64 key, uint32 row, bool *dense)
{
	const PointLeaf *pl = seg_points_read(seg);
	PointHas	ph = {.seg = seg,.col = col,.row = row};

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	ph.pl = pl + col;
	point_probe(seg, col, ph.pl, key, point_find_row, &ph);
	*dense = ph.dense;
	return ph.found;
}

/* The heap addresses a table of addresses lists (column format minor 16): the payload of a slot is one address, or a list at a word offset */
typedef struct TidAcc
{
	SiftColSegment *seg;
	int			col;
	const PointLeaf *pl;
	SiftKey    *t;
	uint32		n;
	uint32		cap;
	uint32		limit;			/* 0: no limit; else more addresses than this make over */
	bool		over;
	uint32		words;			/* the words of the lists read */
	uint32		slots;			/* the slots whose addresses were taken: one slot's list is ascending and lists no address twice */
	uint32		last_sb;		/* the superblock of the last address and its unit: the addresses of a list ascend, so a unit repeats */
	uint32		last_u;
	bool		seen;			/* last_sb and last_u are those of an address */
	bool		some;			/* keep has a unit */
	const uint64 *keep;			/* the units the caller still reads (NULL: all): the addresses of the others are not kept, and a list ends at the last of these */
	uint32		last_keep;
	uint64	   *slot_seen;		/* NULL, or one bit for each slot of the table: a slot visited before (by a lookup of other keys, one fingerprint) gives its addresses once */
} TidAcc;

#define TID_PAYLOAD(slot)	((slot) & ((UINT64CONST(1) << 48) - 1))

static void
tid_push(TidAcc *a, SiftKey key)
{
	SiftColSegment *seg = a->seg;
	uint32		sb = (uint32) (key >> 16) >> SIFT_SB_SHIFT,
				u;

	if (a->seen && sb == a->last_sb)
		u = a->last_u;
	else
	{
		u = sift_cseg_unit_of_sb(seg, sb);
		if (u >= seg->nsb)
			corrupt(seg, "The point directory of leaf %d lists the address %llx, which is in no unit of the segment.", a->col, (unsigned long long) key);
		a->last_sb = sb;
		a->last_u = u;
		a->seen = true;
	}
	if ((key & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF || (key & 0xFFFF) == 0)
		corrupt(seg, "The point directory of leaf %d lists the address %llx, which is in no unit of the segment.", a->col, (unsigned long long) key);
	if (a->keep && !sift_col_bit_test(a->keep, u))
		return;
	if (a->n == a->cap)
		a->t = a->t ? repalloc_huge(a->t, (a->cap *= 2) * sizeof(SiftKey)) : palloc((a->cap = 16) * sizeof(SiftKey));
	a->t[a->n++] = key;
}

/* Appends the addresses of one slot of a table of addresses to a (ascending in a list), and counts the words of its list */
static void
tid_slot_read(TidAcc *a, uint64 slot)
{
	const PointLeaf *pl = a->pl;
	uint64		pay = TID_PAYLOAD(slot);
	uint32		cnt,
				nwords;
	uint64	   *code;

	a->slots++;
	if ((pay & 0xFFFF) != SIFT_CPOINT_TID_LIST)
	{
		if (a->limit && a->n + 1 > a->limit)
			a->over = true;
		else
			tid_push(a, pay);
		return;
	}
	if (pay >> 48)
		corrupt(a->seg, "The point directory of leaf %d has a list at word %llu of %u.", a->col, (unsigned long long) (pay >> 16), pl->words);
	code = point_list_read(a->seg, a->col, pl, (uint32) (pay >> 16), (UINT64CONST(1) << 41) - 1, a->limit ? a->limit - a->n : UINT32_MAX, &cnt, &nwords);
	a->words += nwords;
	if (code == NULL)
	{
		a->over = true;
		return;
	}
	for (uint32 i = 0; i < cnt; i++)
	{
		tid_push(a, sift_cpoint_tid_key(code[i]));
		/* the units ascend with the addresses: the rest of the list is in units past the last one read */
		if (a->keep && a->last_u > a->last_keep)
			break;
	}
	pfree(code);
}

static void
point_add_tids(void *arg, uint64 slot, uint32 pos)
{
	TidAcc	   *a = arg;

	if (a->slot_seen != NULL)
	{
		if (a->slot_seen[pos / 64] >> (pos % 64) & 1)
			return;
		a->slot_seen[pos / 64] |= UINT64CONST(1) << (pos % 64);
	}
	if (!a->over)
		tid_slot_read(a, slot);
}

bool
sift_cseg_point_is_tid(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl != NULL && pl[col].nslots != 0 && pl[col].tids;
}

static bool
point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, const uint64 *keep, uint32 limit, uint64 *seen, SiftKey **tids, uint32 *ntids)
{
	const PointLeaf *pl = seg_points_read(seg);
	TidAcc		a = {.seg = seg,.col = col,.limit = limit,.keep = keep,.slot_seen = seen};
	uint32		u = 0;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids)
		return false;
	a.pl = pl + col;
	/* the bits past the last unit of the bitmap are not units */
	for (uint32 w = 0; keep && w < (seg->nsb + 63) / 64; w++)
	{
		uint64		bits = w == seg->nsb / 64 ? keep[w] & ((UINT64CONST(1) << (seg->nsb % 64)) - 1) : keep[w];

		if (bits)
		{
			a.last_keep = w * 64 + 63 - __builtin_clzll(bits);
			a.some = true;
		}
	}
	/* no unit to read: no address is needed */
	if (keep && !a.some)
	{
		*tids = NULL;
		*ntids = 0;
		return true;
	}
	for (int k = 0; k < nkeys && !a.over; k++)
		point_probe(seg, col, a.pl, keys[k], point_add_tids, &a);
	if (a.over)
	{
		if (a.t)
			pfree(a.t);
		return false;
	}
	/* the addresses of one slot ascend and are distinct; those of several slots (several keys, or keys that share a fingerprint) are sorted and made so */
	if (a.slots > 1)
	{
		sift_sort_u64(a.t, a.n);
		for (uint32 i = 0; i < a.n; i++)
			if (u == 0 || a.t[u - 1] != a.t[i])
				a.t[u++] = a.t[i];
	}
	else
		u = a.n;
	*tids = a.t;
	*ntids = u;
	return true;
}

bool
sift_cseg_point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, const uint64 *keep, SiftKey **tids, uint32 *ntids)
{
	return point_tids(seg, col, keys, nkeys, keep, SIFT_CPOINT_MAX_ROWS, NULL, tids, ntids);
}

/* ---- the addresses of a lookup read as far as the caller goes: a cursor over each list the keys name, merged ---- */

#define TCUR_BUF		2048		/* the bytes of a list read at a time: a code is at most 10 */

#define TCUR_MAX		8			/* lists a stream merges: more slots are decoded whole (sift_cseg_point_tids) */

typedef struct TidCur
{
	uint64		at;				/* the file position of the next byte of the list not read yet */
	uint32		unread;			/* ... and how many bytes of it there are */
	uint32		remaining;		/* addresses of the list not taken yet */
	uint32		taken;
	uint64		prev;
	SiftKey		head;
	bool		have;
	uint32		off;			/* word of the list, for the messages */
	uint32		bpos;
	uint32		blen;
	uint32		chunk;			/* the bytes of the next read: it doubles, so a scan that stops early reads little and one that goes on reads whole pages */
	uint8		buf[TCUR_BUF + 16];
} TidCur;

struct SiftPointStream
{
	SiftColSegment *seg;
	int			col;
	int			n;				/* the lists */
	TidCur	   *cur[TCUR_MAX];
	SiftKey	   *one;			/* the slots that hold one address, ascending and distinct, from pone */
	uint32		none;
	uint32		pone;
	SiftKey		last;
	bool		any;
	uint64	   *slots;
	int			nslots;
	int			nlists;
	/* a lookup of many keys (SIFT_CPOINT_CHUNK_MIN or more) is read a chunk of keys at a time: the keys not looked up yet, and the addresses of the chunk in hand */
	int64	   *keys;			/* NULL for the places of a dictionary: the keys are the codes at places pfrom + kpos on of its byte order (sift_cseg_point_stream_open_places) */
	uint32		pfrom;
	int			nkeys;
	int			kpos;
	int			kchunk;
	SiftKey    *buf;
	uint32		nbuf;
	uint32		pbuf;
	uint64	   *seen;			/* a bit for each slot of the table that a chunk visited: two keys with one fingerprint list the same slot, and the executor must not get a row twice */
};

static void
stream_slot(void *arg, uint64 slot, uint32 pos)
{
	SiftPointStream *s = arg;

	if ((TID_PAYLOAD(slot) & 0xFFFF) == SIFT_CPOINT_TID_LIST && s->nlists++ == TCUR_MAX)
		return;
	if (s->nslots % 64 == 0)
		s->slots = s->slots ? repalloc(s->slots, (s->nslots + 64) * sizeof(uint64)) : palloc(64 * sizeof(uint64));
	s->slots[s->nslots++] = slot;
}

/* The next address of a list into its head (have: false past its last) */
static void
tcur_advance(SiftPointStream *s, TidCur *t)
{
	uint64		d = 0;
	int			shift = 0;
	uint8		b;

	if (t->remaining == 0)
	{
		if (t->bpos != t->blen || t->unread != 0)
			corrupt(s->seg, "The point directory of leaf %d has a list at word %u that is longer than its addresses.", s->col, t->off);
		t->have = false;
		return;
	}
	if (t->blen - t->bpos < 10 && t->unread > 0)
	{
		uint32		rem = t->blen - t->bpos,
					take = Min(t->unread, Min(t->chunk, TCUR_BUF - rem));

		memmove(t->buf, t->buf + t->bpos, rem);
		seg_read(s->seg, t->at, take, t->buf + rem);
		t->at += take;
		t->unread -= take;
		t->bpos = 0;
		t->blen = rem + take;
		t->chunk = Min(t->chunk * 2, TCUR_BUF);
	}
	do
	{
		if (t->bpos >= t->blen || shift > 63)
			corrupt(s->seg, "The point directory of leaf %d has a list at word %u that does not hold its addresses.", s->col, t->off);
		b = t->buf[t->bpos++];
		d |= (uint64) (b & 0x7F) << shift;
		shift += 7;
	} while (b & 0x80);
	if ((t->taken > 0 && d == 0) || (d >> 41) || ((t->prev + d) >> 41))
		corrupt(s->seg, "The point directory of leaf %d has a list at word %u that does not hold ascending addresses.", s->col, t->off);
	t->prev += d;
	t->head = sift_cpoint_tid_key(t->prev);
	t->have = true;
	t->taken++;
	t->remaining--;
}

/* The addresses of the next chunk of keys of a chunked stream into buf, those not given before; false when every key was looked up */
static bool
stream_chunk(SiftPointStream *s)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(s));

	while (s->kpos < s->nkeys)
	{
		int			k = Min(s->nkeys - s->kpos, s->kchunk);
		SiftKey    *tids;
		uint32		n;
		bool		found;
		int64	   *places = NULL;
		const int64 *kk = s->keys != NULL ? s->keys + s->kpos : NULL;

		if (kk == NULL)
		{
			DictLook   *l = seg_dict_look(s->seg, s->col);
			uint32	   *codes = palloc(k * sizeof(uint32));

			seg_read(s->seg, l->perm_at + ((uint64) s->pfrom + s->kpos) * sizeof(uint32), k * sizeof(uint32), codes);
			/* in the order of the codes, which follows that of the rows: the addresses of the keys come nearly in order of the heap, and the sort of them finds it so */
			sort_u32(codes, k);
			places = palloc(k * sizeof(int64));
			for (int j = 0; j < k; j++)
				places[j] = codes[j];
			pfree(codes);
			kk = places;
		}
		found = point_tids(s->seg, s->col, kk, k, NULL, 0, s->seen, &tids, &n);
		if (places)
			pfree(places);
		SIFT_INVARIANT(found, "sift_col: the directory of heap addresses of leaf %d gave no lookup", s->col);
		s->kpos += k;
		s->kchunk = Min(s->kchunk * 4, SIFT_CPOINT_CHUNK_LAST);
		if (s->buf)
			pfree(s->buf);
		s->buf = tids;
		s->nbuf = n;
		s->pbuf = 0;
		if (n > 0)
		{
			MemoryContextSwitchTo(old);
			return true;
		}
	}
	MemoryContextSwitchTo(old);
	return false;
}

SiftPointStream *
sift_cseg_point_stream_open(SiftColSegment *seg, int col, const int64 *keys, int nkeys)
{
	const PointLeaf *pl = seg_points_read(seg);
	SiftPointStream *s;
	uint64		rows = 0;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids)
		return NULL;
	s = palloc0(sizeof(SiftPointStream));
	/*
	 * A list of many keys is looked up a chunk at a time (a LIMIT stops after the first): the first chunk's keys cost what they cost, the others wait. The cap on
	 * the rows of the segment bounds what a key can list, as a lookup that finds more rows than SIFT_CPOINT_MAX_ROWS cannot be refused halfway.
	 */
	if (nkeys >= SIFT_CPOINT_CHUNK_MIN && (uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows <= SIFT_CPOINT_CHUNK_ROWS)
	{
		s->seg = seg;
		s->col = col;
		s->keys = palloc(nkeys * sizeof(int64));
		memcpy(s->keys, keys, nkeys * sizeof(int64));
		s->nkeys = nkeys;
		s->kchunk = SIFT_CPOINT_CHUNK_FIRST;
		s->seen = palloc0(((Size) pl[col].nslots + 63) / 64 * sizeof(uint64));
		return s;
	}
	s->seg = seg;
	s->col = col;
	for (int k = 0; k < nkeys && s->nlists <= TCUR_MAX; k++)
		point_probe(seg, col, pl + col, keys[k], stream_slot, s);
	/* more lists than the stream merges are decoded whole (sift_cseg_point_tids) */
	if (s->nlists > TCUR_MAX)
		return NULL;
	s->one = palloc(Max(s->nslots, 1) * sizeof(SiftKey));
	for (int i = 0; i < s->nslots; i++)
	{
		uint64		pay = TID_PAYLOAD(s->slots[i]);
		TidCur	   *t;
		uint32		hdr[2];
		uint32		off;
		uint64		base;

		if ((pay & 0xFFFF) != SIFT_CPOINT_TID_LIST)
		{
			s->one[s->none++] = pay;
			rows++;
			continue;
		}
		off = (uint32) (pay >> 16);
		base = pl[col].at + (uint64) pl[col].nslots * sizeof(uint64) + (uint64) off * sizeof(uint32);
		if (pay >> 48 || off >= pl[col].words || pl[col].words - off < 2)
			corrupt(seg, "The point directory of leaf %d has a list at word %llu of %u.", col, (unsigned long long) (pay >> 16), pl[col].words);
		seg_read(seg, base, sizeof(hdr), hdr);
		if (hdr[0] < 2 || hdr[0] > pl[col].count || hdr[1] < hdr[0] || hdr[1] > 6 * hdr[0] || (uint64) off + 2 + (hdr[1] + 3) / 4 > pl[col].words)
			corrupt(seg, "The point directory of leaf %d has a list of %u addresses in %u bytes at word %u of %u.", col, hdr[0], hdr[1], off, pl[col].words);
		t = palloc(sizeof(TidCur));
		/* the fields one by one: a compound literal would clear and copy the buffer */
		t->at = base + sizeof(hdr);
		t->unread = hdr[1];
		t->remaining = hdr[0];
		t->taken = 0;
		t->prev = 0;
		t->have = false;
		t->off = off;
		t->bpos = t->blen = 0;
		t->chunk = 256;
		s->cur[s->n++] = t;
		rows += hdr[0];
	}
	/* a lookup that finds more rows is not a point lookup: the units answer it */
	if (rows > SIFT_CPOINT_MAX_ROWS)
		return NULL;
	if (s->none > 1)
	{
		uint32		u = 1;

		sift_sort_u64(s->one, s->none);
		for (uint32 i = 1; i < s->none; i++)
			if (s->one[u - 1] != s->one[i])
				s->one[u++] = s->one[i];
		s->none = u;
	}
	for (int i = 0; i < s->n; i++)
		tcur_advance(s, s->cur[i]);
	return s;
}

/*
 * The stream of the lookup of the codes at places [from, to) of the byte order of a dictionary read in place (a prefix or a range of a text leaf), looked up a chunk at a time
 * without making the keys: NULL when the leaf has no addresses or the segment is too large for chunks (the caller makes the keys)
 */
SiftPointStream *
sift_cseg_point_stream_open_places(SiftColSegment *seg, int col, uint32 from, uint32 to)
{
	const PointLeaf *pl = seg_points_read(seg);
	SiftPointStream *s;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids || (uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows > SIFT_CPOINT_CHUNK_ROWS)
		return NULL;
	s = palloc0(sizeof(SiftPointStream));
	s->seg = seg;
	s->col = col;
	s->pfrom = from;
	s->nkeys = to - from;
	s->kchunk = SIFT_CPOINT_CHUNK_FIRST;
	s->seen = palloc0(((Size) pl[col].nslots + 63) / 64 * sizeof(uint64));
	return s;
}

/* The codes at places [from, to) of the byte order of a dictionary read in place into keys, to - from of them */
void
sift_cseg_dict_codes(SiftColSegment *seg, int col, uint32 from, uint32 to, int64 *keys)
{
	DictLook   *l = seg_dict_look(seg, col);
	uint32	   *codes = palloc((Size) (to - from) * sizeof(uint32));

	seg_read(seg, l->perm_at + (uint64) from * sizeof(uint32), (Size) (to - from) * sizeof(uint32), codes);
	for (uint32 i = 0; i < to - from; i++)
		keys[i] = codes[i];
	pfree(codes);
}

uint32
sift_cseg_point_stream_next(SiftPointStream *s, SiftKey *out, uint32 max)
{
	uint32		n = 0;

	if (s->nkeys > 0)
	{
		while (n < max && (s->pbuf < s->nbuf || stream_chunk(s)))
		{
			uint32		take = Min(max - n, s->nbuf - s->pbuf);

			memcpy(out + n, s->buf + s->pbuf, take * sizeof(SiftKey));
			s->pbuf += take;
			n += take;
		}
		return n;
	}
	while (n < max)
	{
		TidCur	   *best = NULL;
		SiftKey		head = s->pone < s->none ? s->one[s->pone] : PG_UINT64_MAX;

		for (int i = 0; i < s->n; i++)
			if (s->cur[i]->have && s->cur[i]->head < head)
			{
				best = s->cur[i];
				head = best->head;
			}
		if (best == NULL && s->pone == s->none)
			break;
		if (!s->any || head != s->last)
			out[n++] = head;
		s->last = head;
		s->any = true;
		if (best)
			tcur_advance(s, best);
		else
			s->pone++;
	}
	return n;
}

typedef struct TidKey
{
	int64		key;
	SiftKey		tid;
} TidKey;

static int
tid_key_cmp(const void *a, const void *b)
{
	const TidKey *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return (x->tid > y->tid) - (x->tid < y->tid);
}

bool
sift_cseg_point_check_tids(SiftColSegment *seg, int col, const int64 *key, const uint64 *nulls, const SiftKey *tid, uint32 n, uint64 *listed)
{
	const PointLeaf *pl = seg_points_read(seg);
	TidKey	   *e = palloc(Max(n, 1) * sizeof(TidKey));
	uint32		m = 0;
	bool		ok = true;

	Assert(pl != NULL && pl[col].tids);
	for (uint32 i = 0; i < n; i++)
		if (!(nulls && (nulls[i / 64] >> (i % 64) & 1)))
			e[m++] = (TidKey) {key[i], tid[i]};
	qsort(e, m, sizeof(TidKey), tid_key_cmp);
	for (uint32 i = 0, j; i < m && ok; i = j)
	{
		TidAcc		a = {.seg = seg,.col = col,.pl = pl + col};
		uint32		k = 0;

		for (j = i; j < m && e[j].key == e[i].key; j++)
			;
		point_probe(seg, col, a.pl, e[i].key, point_add_tids, &a);
		if (a.n > 0)
			sift_sort_u64(a.t, a.n);
		for (uint32 r = i; r < j && ok; r++)
		{
			while (k < a.n && a.t[k] < e[r].tid)
				k++;
			ok = k < a.n && a.t[k] == e[r].tid;
		}
		if (a.t)
			pfree(a.t);
	}
	pfree(e);
	*listed += m;
	return ok;
}

uint32
sift_cseg_point_count(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl ? pl[col].count : 0;
}

/* the distinct keys of the leaf's directory */
uint32
sift_cseg_point_nkeys(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl ? pl[col].nkeys : 0;
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

					tid_slot_read(&a, win[j]);
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

					pfree(point_list_read(seg, c, &pl[c], payload & ~SIFT_CPOINT_LIST, total - 1, UINT32_MAX, &cnt, &nw));
					words += nw;
				}
				else if (payload & SIFT_CPOINT_LIST)
				{
					uint32		prev = 0;

					cnt = point_list_count(seg, c, &pl[c], payload & ~SIFT_CPOINT_LIST);
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
