/*
 * col/seg.c - immutable column segments: writer and reader over the page store.
 *
 * Stream: UNITS (the unit records, 8-byte aligned), DICTS (a directory, then the serialized dictionary of each STR leaf), ZONES (a
 * directory, then per leaf the zone arrays of all units), then SCHEMA and SBDIR. The header page is a SiftSegHeaderData whose sections
 * are, in order of the stream: POSTINGS = UNITS, DICT = DICTS, DICTIDX = ZONES, DOCMAP = SCHEMA + SBDIR (dictroot_off = the schema's bytes,
 * dictroot_n = the units).
 */

#include "core/host.h"

#include "col/compress.h"
#include "col/hll.h"
#include "col/ktab.h"
#include "col/point.h"
#include "col/preagg.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/runs.h"
#include "col/setsum.h"
#include "col/ver.h"
#include "core/live.h"
#include "core/popcount.h"
#include "core/cmp.h"

#ifdef SIFT_TESTING
uint64		sift_test_seg_flip_at = UINT64_MAX;
uint8		sift_test_seg_flip_mask;
#endif

static void
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

static const SiftSectionRef *
sec(const SiftColSegment *seg, SiftSection s)
{
	return &seg->hdr->sections[s];
}

/* The head is not kept while a test flips a byte of what is read: the flipped one must not be found by the next open, nor the next one skip the read */
#ifdef SIFT_TESTING
#define HEAD_CACHED()	(sift_test_seg_flip_at == UINT64_MAX)
#else
#define HEAD_CACHED()	true
#endif

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

		if (r->sb < h->sb_lo || r->sb > h->sb_hi || (i > 0 && r->sb <= dir[i - 1].sb) || r->off % 8 != 0 ||
			r->nrows == 0 || r->nrows > SIFT_COL_UNIT_MAX_ROWS || r->rank_base != rank ||
			r->off + r->len > s[SIFT_SEC_POSTINGS].len || (i > 0 && r->off < dir[i - 1].off + dir[i - 1].len))
			corrupt(seg, "SBDIR entry %u (SB %u at %llu, %u rows, rank %llu) is out of order.", i, r->sb,
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
sift_cseg_find_unit(const SiftColSegment *seg, uint32 sb)
{
	uint32		lo = 0,
				hi = seg->nsb,
				g,
				step = 1;

	if (seg->nsb == 0 || sb <= seg->sbdir[0].sb)
		return 0;
	if (sb > seg->sbdir[seg->nsb - 1].sb)
		return seg->nsb;
	/*
	 * The superblocks of a segment are nearly evenly spaced (all of them, or every other one where two workers built it): the unit at the same share of the
	 * directory is near, and a gallop from it brackets the answer in the steps of its distance, not of the directory.
	 */
	g = (uint32) ((uint64) (sb - seg->sbdir[0].sb) * (seg->nsb - 1) / (seg->sbdir[seg->nsb - 1].sb - seg->sbdir[0].sb));
	if (seg->sbdir[g].sb < sb)
	{
		for (lo = g + 1; lo + step < seg->nsb && seg->sbdir[lo + step - 1].sb < sb; step <<= 1)
			lo += step;
		hi = Min(lo + step, seg->nsb);
	}
	else
	{
		for (hi = g; hi >= step && seg->sbdir[hi - step].sb >= sb; step <<= 1)
			hi -= step;
		lo = hi >= step ? hi - step + 1 : 0;
	}
	SIFT_BISECT(uint32, mid, lo, hi, seg->sbdir[mid].sb < sb);
	return lo;
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
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, r->len, pv->unitbuf);
	memset(pv->unitbuf + r->len, 0, SIFT_COL_PAD);
	*len = r->len;
	return pv->unitbuf;
}

uint64
sift_cseg_units_len(const SiftColSegment *seg)
{
	return sec(seg, SIFT_SEC_POSTINGS)->len;
}

void
sift_cseg_unit_head_span(const SiftColSegment *seg, uint32 u, uint64 *off, uint64 *len)
{
	*off = sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off;
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
	/* the directory of a key-cut unit of many rows ends beyond the head, and the chunk table follows it */
	if (*avail < r->len)
	{
		uint32		need = sift_cunit_head_need(pv->unitbuf, *avail, r->len, seg->schema->ncols);

		if (need > 0)
		{
			sift_cseg_unit_fill(seg, u, *avail, need - *avail);
			*avail = need;
		}
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
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off + off, len, pv->unitbuf + off);
	memset(pv->unitbuf + seg->sbdir[u].len, 0, SIFT_COL_PAD);
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

static GOrdLeaf *gord_leaf(SiftColSegment *seg, int col);

static const PointLeaf *
points_read(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint64		foot[2],
				next;
	int64		prev = -1;
	uint32		hdr[4],
				tail[2];
	char	   *recs;

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
		if (foot[0] % 8 == 0 && foot[0] + sizeof(hdr) <= s->len - sizeof(foot))
		{
			seg_read(seg, s->off + foot[0], sizeof(hdr), hdr);
			if (hdr[0] == SIFT_CPOINT_MAGIC && point_version_ok(hdr[1]))
				corrupt(seg, "The footer of the point directories of the DICT section has the magic 0x%08x, version %u.", tail[0], tail[1]);
		}
		return NULL;
	}
	if (foot[0] % 8 != 0 || foot[0] + sizeof(hdr) > s->len - sizeof(foot))
		corrupt(seg, "The point directories of the DICT section start at %llu.", (unsigned long long) foot[0]);
	seg_read(seg, s->off + foot[0], sizeof(hdr), hdr);
	if (hdr[0] != SIFT_CPOINT_MAGIC || hdr[1] != tail[1] || hdr[2] > (uint32) seg->schema->ncols || hdr[3] > (hdr[1] == SIFT_CPOINT_VERSION_ORD ? 2 * (uint32) seg->schema->ncols : 0) ||
		foot[0] + sizeof(hdr) + (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN > s->len - sizeof(foot))
		corrupt(seg, "The point directories of the DICT section have the header 0x%08x, version %u, %u leaves.", hdr[0], hdr[1], hdr[2]);
	next = foot[0] + sizeof(hdr) + (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN;
	pv->pt = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(PointLeaf));
	pv->go = MemoryContextAllocZero(seg->mcxt, 2 * seg->schema->ncols * sizeof(GOrdLeaf));
	recs = palloc((hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN);
	seg_read(seg, s->off + foot[0] + sizeof(hdr), (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN, recs);
	for (uint32 i = 0; i < hdr[2] + hdr[3]; i++)
	{
		uint32		rec[6];
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
				at != next || size > s->len - sizeof(foot) - next)
				corrupt(seg, "The global order %u of the DICT section is malformed.", i - hdr[2]);
			pv->go[rec[0]] = (GOrdLeaf) {s->off + at, size, {rec[2], rec[3], rec[1], rec[1] == (rec[2] + SIFT_CGORD_ROWS - 1) / SIFT_CGORD_ROWS ? SIFT_CGORD_ROWS : SIFT_CGORD_ROWS_FINE, 0, -1}, NULL, 0};
			prev = rec[0];
			next += size;
			continue;
		}
		size = ALIGN8((uint64) rec[1] * sizeof(uint64) + (uint64) rec[4] * sizeof(uint32));
		if (rec[0] >= (uint32) seg->schema->ncols || (int64) rec[0] <= prev || rec[5] > (hdr[1] == SIFT_CPOINT_VERSION || hdr[1] == SIFT_CPOINT_VERSION_ORD ? SIFT_CPOINT_MODE_ZROWS : SIFT_CPOINT_MODE_ROWS) ||
			rec[1] < 16 || (rec[1] & (rec[1] - 1)) != 0 || rec[3] == 0 ||
			rec[3] > rec[1] || rec[2] < rec[3] || rec[2] >= SIFT_CPOINT_SEG_ROWS || (uint64) rec[4] > (rec[5] == SIFT_CPOINT_MODE_TIDS || rec[5] == SIFT_CPOINT_MODE_ZROWS ? 3 * (uint64) rec[2] : (uint64) rec[2] + rec[3]) ||
			at != next || size > s->len - sizeof(foot) - next)
			corrupt(seg, "The point directory %u of the DICT section is malformed.", i);
		/* the units of a key-cut segment are not block ranges: an address names no unit, so such a segment lists rows */
		if (rec[5] == SIFT_CPOINT_MODE_TIDS && sift_cseg_keyed(&seg->entry))
			corrupt(seg, "The point directory of leaf %u lists heap addresses, which a key-cut segment cannot; REINDEX the index.", rec[0]);
		pv->pt[rec[0]] = (PointLeaf) {s->off + at, rec[1], rec[3], rec[2], rec[4], rec[5] == SIFT_CPOINT_MODE_TIDS, rec[5] == SIFT_CPOINT_MODE_ZROWS};
		prev = rec[0];
		next += size;
	}
	pfree(recs);
	if (next != s->len - sizeof(foot))
		corrupt(seg, "The point directories of the DICT section end at %llu of %llu.", (unsigned long long) next, (unsigned long long) (s->len - sizeof(foot)));
	pv->dict_end = foot[0];
	return pv->pt;
}

/* Where the dictionary of leaf col lies in the stream: offset and length (up to the next dictionary or the end of the section) */
static void
dict_region(SiftColSegment *seg, int col, uint64 *off, uint64 *len)
{
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2];
	uint64		next,
				tab[SIFT_COL_MAX_COLS];

	points_read(seg);
	next = PRIV(seg)->dict_end;

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
	/* the plain form is read into the arrays of the dictionary, not into a buffer that is copied (a scan of a leaf reads it whole once for each backend) */
	if (len >= DICT_HEAD)
	{
		uint32		head[2];
		uint64		at = sec(seg, SIFT_SEC_DICT)->off + off;

		seg_read(seg, at, sizeof(head), head);
		if (head[0] == DICT_PLAIN && (len - DICT_HEAD) / sizeof(uint32) >= (uint64) head[1] + 1)
		{
			d = sift_cdict_plain_begin(mcxt, head[1]);
			seg_read(seg, at + DICT_HEAD, ((Size) head[1] + 1) * sizeof(uint32), d->off);
			if (!sift_cdict_plain_bytes(d, mcxt, len - DICT_HEAD - ((uint64) head[1] + 1) * sizeof(uint32)))
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			seg_read(seg, at + DICT_HEAD + ((uint64) head[1] + 1) * sizeof(uint32), d->nbytes, d->bytes);
			if (!sift_cdict_plain_valid(d))
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			return d;
		}
	}
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

/* Block b of a dictionary of the searched form, decompressed into l->blkraw with the offsets of its strings in l->blkoff */
static void
look_load_block(DictLook *l, uint32 b)
{
	uint32		o[2],
				cnt = Min(DICT_BLOCK, l->n - b * DICT_BLOCK),
				rawlen,
				pos = 0;

	seg_read(l->seg, l->boff_at + (uint64) b * sizeof(uint32), sizeof(o), o);
	if (o[1] < o[0] + DICT_BLOCK_HEAD || o[1] - o[0] > sizeof(l->blkz) || o[1] > l->blk_len)
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	seg_read(l->seg, l->blk_at + o[0], o[1] - o[0], l->blkz);
	rawlen = l->blkz[2] | (uint32) l->blkz[3] << 8;
	if (rawlen > DICT_BLOCK_RAW_MAX)
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	if (l->blkz[0] == 0)
	{
		if (o[1] - o[0] - DICT_BLOCK_HEAD != rawlen)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		memcpy(l->blkraw, l->blkz + DICT_BLOCK_HEAD, rawlen);
	}
	else if ((l->blkz[0] != SIFT_CC_ALG_ZSTD && l->blkz[0] != SIFT_CC_ALG_LZ4) || !sift_colz_available(l->blkz[0]))
		corrupt(l->seg, "The dictionary of leaf %d is compressed with an algorithm this build lacks.", l->col);
	else if (!sift_colz_decompress(l->blkz[0], l->blkz + DICT_BLOCK_HEAD, o[1] - o[0] - DICT_BLOCK_HEAD, l->blkraw, rawlen))
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	if (l->fixed)
	{
		if (rawlen != cnt * l->fixed)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		for (uint32 i = 0; i <= cnt; i++)
			l->blkoff[i] = i * l->fixed;
	}
	else
	{
		if (rawlen < cnt)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		for (uint32 i = 0; i < cnt; i++)
		{
			l->blkoff[i] = cnt + pos;
			pos += l->blkraw[i];
		}
		l->blkoff[cnt] = cnt + pos;
		if (cnt + pos != rawlen)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	}
	l->blkno = b;
	l->loads++;
	if (l->bc != NULL)
		l->bc->spent += l->blkz[0] == 0 ? DICT_READ_COST : l->blkz[0] == SIFT_CC_ALG_LZ4 ? DICT_LZ4_COST : DICT_ZSTD_COST;
}

/* The string of a code of a dictionary in the stream, into buf (SIFT_COL_DICT_MAX_ENTRY bytes) */
static const char *
look_code_str(DictLook *l, uint32 code, uint32 *len, char *buf)
{
	uint64		at;

	if (code >= l->n)
		corrupt(l->seg, "Code %u is beyond the %u strings of the dictionary of leaf %d.", code, l->n, l->col);
	if (l->blocked)
	{
		uint32		i = code % DICT_BLOCK;

		if (l->blkno != code / DICT_BLOCK)
			look_load_block(l, code / DICT_BLOCK);
		*len = l->blkoff[i + 1] - l->blkoff[i];
		memcpy(buf, l->blkraw + l->blkoff[i], *len);
		return buf;
	}
	if (l->fixed)
	{
		*len = l->fixed;
		at = l->bytes_at + (uint64) code * l->fixed;
	}
	else
	{
		uint32		o[2];

		seg_read(l->seg, l->offs_at + (uint64) code * sizeof(uint32), sizeof(o), o);
		if (o[1] < o[0] || o[1] > l->nbytes || o[1] - o[0] > SIFT_COL_DICT_MAX_ENTRY)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		*len = o[1] - o[0];
		at = l->bytes_at + o[0];
	}
	seg_read(l->seg, at, *len, buf);
	return buf;
}

static inline BisectSlot *
look_slot(const DictLook *l, uint32 pos)
{
	return l->bc ? &l->bc->slot[(pos * 0x9E3779B1u) >> 8 & l->bc->mask] : NULL;
}

static uint32
look_perm(const DictLook *l, uint32 pos)
{
	const BisectSlot *s = look_slot(l, pos);
	uint32		code;

	if (s != NULL && s->pos1 == pos + 1)
		return s->code;
	if (l->bc != NULL)
		l->bc->spent += DICT_READ_COST;
	seg_read(l->seg, l->perm_at + (uint64) pos * sizeof(uint32), sizeof(code), &code);
	if (code >= l->n)
		corrupt(l->seg, "The order of the dictionary of leaf %d names code %u of %u.", l->col, code, l->n);
	return code;
}

/* A string of the order by position; the buffer is the one of the search (a bisection holds one string at a time) */
static const char *
look_sorted_str(const void *ctx, uint32 pos, uint32 *len)
{
	DictLook   *l = (DictLook *) ctx;
	BisectSlot *s = look_slot(l, pos);
	uint32		code;
	const char *str;

	if (s != NULL && s->pos1 == pos + 1)
	{
		if (s->hits < UINT8_MAX)
			s->hits++;
		if (s->len != BISECT_NOSTR)
		{
			*len = s->len;
			return s->str;
		}
		return look_code_str(l, s->code, len, l->buf);
	}
	code = look_perm(l, pos);
	str = look_code_str(l, code, len, l->buf);
	/* a miss takes over an empty slot or one nothing has used since the last miss on it, and otherwise ages it: what every search shares (the top of the bisection) stays */
	if (s != NULL && (s->pos1 == 0 || s->hits == 0))
	{
		s->pos1 = pos + 1;
		s->code = code;
		s->hits = 0;
		s->len = *len <= BISECT_STR ? (uint16) *len : BISECT_NOSTR;
		if (s->len != BISECT_NOSTR)
			memcpy(s->str, str, *len);
	}
	else if (s != NULL)
		s->hits--;
	return str;
}

/* The cache of the positions of a dictionary of many strings, once per search */
static void
look_cache(DictLook *l)
{
	if (!l->bc_tried)
	{
		l->bc_tried = true;
		if (l->n >= BISECT_MIN && HEAD_CACHED())
			l->bc = sift_cseg_bisect(l->seg, l->col, l->n);
	}
}

#define HASH_WINDOW	32				/* slots read at a time */

/*
 * The code of a string by the hash of the dictionary (DICT_HASHED): the slots from its position are read a window at a time up to an empty one; those with its fingerprint name a
 * code, whose string is read (a block) and compared. A string the dictionary lacks costs the read of a window and no block, almost always.
 */
static int64
look_find_hash(DictLook *l, const char *s, uint32 len)
{
	uint64		h = sift_cdict_hash(s, len);
	uint32		ns = SIFT_CDICT_HASH_SLOTS(l->n),
				cb = sift_cdict_hash_cb(l->n),
				fp = (uint32) h & (UINT32_MAX >> cb),
				i = sift_cdict_hash_pos(h, ns),
				win[HASH_WINDOW];

	for (uint32 probed = 0; probed < ns;)
	{
		uint32		k = Min(HASH_WINDOW, ns - i);

		seg_read(l->seg, l->hash_at + (uint64) i * sizeof(uint32), k * sizeof(uint32), win);
		for (uint32 j = 0; j < k; j++, probed++)
		{
			uint32		code,
						slen,
						loads;
			const char *str;

			if (win[j] == 0)
				return -1;
			if (win[j] >> cb != fp)
				continue;
			code = (win[j] & (UINT32_MAX >> (32 - cb))) - 1;
			if (code >= l->n)
				corrupt(l->seg, "The hash of the dictionary of leaf %d names code %u of %u.", l->col, code, l->n);
			loads = l->loads;
			str = look_code_str(l, code, &slen, l->buf);
			sift_cseg_dict_spent(l->seg, l->col, l->loads != loads ? DICT_ZSTD_COST : DICT_READ_COST);
			if (slen == len && memcmp(str, s, len) == 0)
				return code;
		}
		i = i + k == ns ? 0 : i + k;
	}
	return -1;
}

static int64
look_find(const SiftCDictLook *look, const char *s, uint32 len)
{
	const DictLook *l = (const DictLook *) look;
	SiftCSorted so = {l->n, look_sorted_str, l};
	SiftColValue c = {.v.s = s,.len = len};
	uint32		pos;

	if (l->hash_at != 0)
		return look_find_hash((DictLook *) l, s, len);
	look_cache((DictLook *) l);
	pos = sift_csorted_bound(&so, &c, false);

	return pos < l->n && sift_csorted_cmp(&so, pos, &c) == 0 ? (int64) look_perm(l, pos) : -1;
}

/*
 * The strings that hold c, block by block in code order: a block of DICT_BLOCK strings is a word of lut, and its bytes and offsets are what str_contains takes.
 * With the trigram signatures of the blocks (column format minor 29) a literal of three bytes or more reads, decompresses and scans only the blocks whose
 * signature has the bit of each of its trigrams; *passed is how many that were.
 */
#define SIG_CHUNK	(64 * 1024)

static uint32
look_contains(DictLook *l, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut, uint32 *passed)
{
	uint32		count = 0,
				nbits = l->sig_at != 0 && op == SIFT_COLOP_CONTAINS && c->len >= 3 ? c->len - 2 : 0,
				sigbytes = l->sig_at != 0 ? (1u << l->sig_lg) / 8 : 0,
				per = nbits > 0 ? Max(SIG_CHUNK / sigbytes, 1) : 1,
				got = 0,
				first = 0;
	uint32	   *bits = NULL;
	uint8	   *sig = NULL;

	if (nbits > 0)
	{
		bits = palloc(nbits * sizeof(uint32));
		for (uint32 j = 0; j < nbits; j++)
			bits[j] = sift_tsig_bit((const uint8 *) c->v.s + j, l->sig_lg);
		sig = palloc((Size) per * sigbytes);
	}
	*passed = 0;
	for (uint32 b = 0; b * DICT_BLOCK < l->n; b++)
	{
		uint32		cnt = Min(DICT_BLOCK, l->n - b * DICT_BLOCK),
					off[DICT_BLOCK + 1];

		if (nbits > 0)
		{
			const uint8 *s;
			bool		may = true;

			if (b >= first + got)
			{
				first = b;
				got = Min(per, (l->n + DICT_BLOCK - 1) / DICT_BLOCK - b);
				seg_read(l->seg, l->sig_at + (uint64) b * sigbytes, (Size) got * sigbytes, sig);
			}
			s = sig + (Size) (b - first) * sigbytes;
			for (uint32 j = 0; j < nbits && may; j++)
				may = s[bits[j] >> 3] >> (bits[j] & 7) & 1;
			if (!may)
				continue;
		}
		(*passed)++;
		if (l->blkno != b)
			look_load_block(l, b);
		for (uint32 i = 0; i <= cnt; i++)
			off[i] = l->blkoff[i];
		if (op == SIFT_COLOP_SUFFIX)
		{
			lut[b] = 0;
			for (uint32 i = 0; i < cnt; i++)
				if (sift_cstr_matches(op, (const char *) l->blkraw + off[i], off[i + 1] - off[i], c))
					lut[b] |= UINT64CONST(1) << i;
		}
		else
			sift_cstrs_contains(lut + b, (const char *) l->blkraw, off, cnt, op, c, nvals);
		count += pg_popcount64(lut[b]);
	}
	if (bits)
	{
		pfree(bits);
		pfree(sig);
	}
	/* a signature that leaves over half the blocks (a short part, a frequent one) saves little: the next search parses the dictionary and keeps it if the cache holds it, as for one without signatures */
	if (l->sig_at != 0 && *passed * 2 > (l->n + DICT_BLOCK - 1) / DICT_BLOCK)
		sift_cseg_dict_heavy(l->seg, l->col, (uint64) l->n * DICT_PARSE_COST);
	return count;
}

static uint32
look_match(const SiftCDictLook *look, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut)
{
	const DictLook *l = (const DictLook *) look;
	SiftCSorted so = {l->n, look_sorted_str, l};
	uint32		from,
				to;

	if (SIFT_COLOP_SCANS(op))
		return look_contains((DictLook *) l, op, c, nvals, lut, &(uint32) {0});
	look_cache((DictLook *) l);
	sift_csorted_range(&so, op, c, &from, &to);
	for (uint32 pos = from; pos < to;)
	{
		uint32		codes[1024],
					k = Min(to - pos, lengthof(codes));

		seg_read(l->seg, l->perm_at + (uint64) pos * sizeof(uint32), k * sizeof(uint32), codes);
		for (uint32 i = 0; i < k; i++)
		{
			if (codes[i] >= l->n)
				corrupt(l->seg, "The order of the dictionary of leaf %d names code %u of %u.", l->col, codes[i], l->n);
			lut[codes[i] >> 6] |= UINT64CONST(1) << (codes[i] & 63);
		}
		pos += k;
	}
	return to - from;
}

/* A dictionary of blocks and a hash without an order: CONTAINS reads the blocks, every other test needs the strings in order, so the dictionary is parsed (and kept, as for a dictionary read whole) */
static uint32
look_match_blocks(const SiftCDictLook *look, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut)
{
	const DictLook *l = (const DictLook *) look;

	if (op == SIFT_COLOP_CONTAINS || op == SIFT_COLOP_ICONTAINS)
		return look_contains((DictLook *) l, op, c, nvals, lut, &(uint32) {0});
	return sift_cdict_match(sift_cseg_dict(l->seg, l->col), op, c, nvals, lut);
}

/* The blocks and the string buffer of a DictLook are written before they are read: only what comes before them is cleared (clearing them costs 34,000 instructions a query and segment) */
static DictLook *
look_alloc(SiftColSegment *seg)
{
	DictLook   *l = MemoryContextAlloc(seg->mcxt, sizeof(DictLook));

	memset(l, 0, offsetof(DictLook, blkoff));
	return l;
}

/* Where the trigram signatures of the blocks lie, when the dictionary has them: after the form, 8-aligned (the dictionary starts 8-aligned), end being where the form ends */
static void
look_sig(DictLook *l, uint64 off, uint64 len, uint64 end)
{
	uint64		at = off + (end - off + 7) / 8 * 8;
	uint32		hdr[2];
	uint64		nblk = ((uint64) l->n + DICT_BLOCK - 1) / DICT_BLOCK;

	if (at + DICT_TSIG_HEAD > off + len)
		return;
	seg_read(l->seg, at, sizeof(hdr), hdr);
	if (hdr[0] != DICT_TSIG_MAGIC)
		return;
	if (hdr[1] < DICT_TSIG_LO_LG || hdr[1] > DICT_TSIG_HI_LG || at + DICT_TSIG_HEAD + nblk * ((1u << hdr[1]) / 8) > off + len)
		corrupt(l->seg, "The signatures of the dictionary of leaf %d are malformed.", l->col);
	l->sig_at = at + DICT_TSIG_HEAD;
	l->sig_lg = hdr[1];
}

/* A searchable dictionary (form 2) from where its parts lie, which the backend kept from the query that read them */
static DictLook *
look_from_head(SiftColSegment *seg, int col, SiftCBisect *bc)
{
	DictLook   *l = look_alloc(seg);

	l->seg = seg;
	l->col = col;
	l->n = bc->head.n;
	l->fixed = bc->head.fixed;
	l->nbytes = bc->head.nbytes;
	l->boff_at = bc->head.boff_at;
	l->blk_at = bc->head.blk_at;
	l->blk_len = bc->head.blk_len;
	l->perm_at = bc->head.perm_at;
	l->cum_at = bc->head.cum_at;
	l->hash_at = bc->head.hash_at;
	l->blocked = true;
	l->blkno = UINT32_MAX;
	l->sig_at = bc->head.sig_at;
	l->sig_lg = bc->head.sig_lg;
	l->look = (SiftCDictLook) {look_find, look_match};
	l->stub.n = l->n;
	l->stub.look = &l->look;
	l->searchable = true;
	l->bc = bc;
	l->bc_tried = true;
	return l;
}

/* Where the dictionary of leaf col lies when it is stored plain or in blocks (forms 0, 2 and 3); NULL for the compact form, which is read whole */
static DictLook *
dict_look(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	uint64		off,
				len;
	uint32		head[4];
	DictLook   *l;
	uint64		nblk;
	uint32		blen;
	uint64		end = 0;
	SiftCBisect *bc;
	bool		hashed;

	if (pv->look_checked[col])
		return pv->look[col];
	pv->look_checked[col] = true;
	if (HEAD_CACHED() && (bc = sift_cseg_bisect_find(seg, col)) != NULL && bc->have_head)
		return pv->look[col] = look_from_head(seg, col, bc);
	dict_region(seg, col, &off, &len);
	off += sec(seg, SIFT_SEC_DICT)->off;
	if (len < DICT_HEAD)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	seg_read(seg, off, DICT_HEAD, head);
	hashed = (head[0] & DICT_HASHED) != 0;
	head[0] &= ~DICT_HASHED;
	if (head[0] != DICT_LOOKUP && head[0] != DICT_LOOKUP_COUNT && head[0] != DICT_BLOCKS && head[0] != DICT_PLAIN)
		return NULL;
	l = look_alloc(seg);
	l->seg = seg;
	l->col = col;
	l->n = head[1];
	if (head[0] == DICT_PLAIN)
	{
		if (len - DICT_HEAD < ((uint64) l->n + 1) * sizeof(uint32))
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		l->offs_at = off + DICT_HEAD;
		seg_read(seg, l->offs_at + (uint64) l->n * sizeof(uint32), sizeof(l->nbytes), &l->nbytes);
		l->bytes_at = l->offs_at + ((uint64) l->n + 1) * sizeof(uint32);
		if (len - DICT_HEAD - ((uint64) l->n + 1) * sizeof(uint32) < l->nbytes)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	}
	else
	{
		if (len < DICT_HEAD_LOOKUP)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		seg_read(seg, off, DICT_HEAD_LOOKUP, head);
		head[0] &= ~DICT_HASHED;
		l->fixed = head[2];
		l->nbytes = head[3];
		l->blocked = true;
		l->blkno = UINT32_MAX;
		nblk = ((uint64) l->n + DICT_BLOCK - 1) / DICT_BLOCK;
		l->boff_at = off + DICT_HEAD_LOOKUP;
		l->blk_at = l->boff_at + (nblk + 1) * sizeof(uint32);
		if (l->n == 0 || l->fixed > SIFT_COL_DICT_MAX_ENTRY || (l->fixed != 0 && (uint64) l->n * l->fixed != l->nbytes) ||
			l->blk_at > off + len)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		seg_read(seg, l->boff_at + nblk * sizeof(uint32), sizeof(uint32), &blen);
		l->blk_len = blen;
		end = l->blk_at + blen;
		if (head[0] == DICT_LOOKUP || head[0] == DICT_LOOKUP_COUNT)
		{
			l->perm_at = (l->blk_at + blen + 3) / 4 * 4;
			if (l->perm_at + (uint64) l->n * sizeof(uint32) > off + len)
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			end = l->perm_at + (uint64) l->n * sizeof(uint32);
			if (head[0] == DICT_LOOKUP_COUNT)
			{
				uint32		last;

				l->cum_at = l->perm_at + (uint64) l->n * sizeof(uint32);
				if (l->cum_at + DICT_COUNT_HEAD(l->n) > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				seg_read(seg, l->cum_at + (uint64) 8 * DICT_COUNT_NB(l->n) + sizeof(uint32), sizeof(last), &last);
				if (l->cum_at + DICT_COUNT_HEAD(l->n) + last > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				end = l->cum_at + DICT_COUNT_HEAD(l->n) + last;
			}
			if (hashed)
			{
				l->hash_at = head[0] == DICT_LOOKUP_COUNT ? l->cum_at + DICT_COUNT_HEAD(l->n) + (uint64) 0 : l->perm_at + (uint64) l->n * sizeof(uint32);
				if (head[0] == DICT_LOOKUP_COUNT)
				{
					uint32		last;

					seg_read(seg, l->cum_at + (uint64) 8 * DICT_COUNT_NB(l->n) + sizeof(uint32), sizeof(last), &last);
					l->hash_at += last;
				}
				if (l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32) > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				end = l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32);
			}
			look_sig(l, off, len, end);
			l->look = (SiftCDictLook) {look_find, look_match};
			l->stub.n = l->n;
			l->stub.look = &l->look;
			l->searchable = true;
			if (l->n >= BISECT_MIN && HEAD_CACHED() && (bc = sift_cseg_bisect(seg, col, l->n)) != NULL)
			{
				bc->head = (DictLookHead) {l->n, l->fixed, l->nbytes, l->offs_at, l->bytes_at, l->boff_at, l->blk_at, l->blk_len, l->perm_at, l->cum_at, l->hash_at, l->sig_at, l->sig_lg};
				bc->have_head = true;
				l->bc = bc;
				l->bc_tried = true;
			}
		}
		else if (l->blk_at + blen > off + len)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		else
		{
			if (hashed)
			{
				/* blocks, then the hash, no order: an equality finds its code in place, any other test parses the dictionary (look_match_blocks) */
				l->hash_at = (l->blk_at + blen + 3) / 4 * 4;
				if (l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32) > off + len)
					corrupt(seg, "The dictionary of leaf %d is malformed.", col);
				end = l->hash_at + (uint64) SIFT_CDICT_HASH_SLOTS(l->n) * sizeof(uint32);
			}
			look_sig(l, off, len, end);
			/* without the hash, form 3 is searched only by a substring test (sift_cseg_dict_search): that stub has no find */
			l->look = (SiftCDictLook) {hashed ? look_find : NULL, hashed ? look_match_blocks : look_match};
			l->stub.n = l->n;
			l->stub.look = &l->look;
		}
	}
	pv->look[col] = l;
	return l;
}

/* Whether the dictionary of leaf col holds row counts (sift_cseg_dict_count) */
bool
sift_cseg_dict_counted(SiftColSegment *seg, int col)
{
	DictLook   *l;

	return seg->schema->cols[col].store == SIFT_COLS_STR && (l = dict_look(seg, col)) != NULL && l->cum_at != 0;
}

bool
sift_cseg_dict_hashed(SiftColSegment *seg, int col)
{
	DictLook   *l;

	return seg->schema->cols[col].store == SIFT_COLS_STR && (l = dict_look(seg, col)) != NULL && l->hash_at != 0;
}

/* Every string of the dictionary is reachable from its position in the hash, and the hash holds nothing else */
void
sift_cseg_dict_verify_hash(SiftColSegment *seg, int col, SiftCDict *d)
{
	DictLook   *l = dict_look(seg, col);
	uint32		ns = SIFT_CDICT_HASH_SLOTS(d->n),
				cb = sift_cdict_hash_cb(d->n),
				used = 0;
	uint32	   *slot = palloc(ns * sizeof(uint32));

	if (l == NULL || l->hash_at == 0)
		corrupt(seg, "The dictionary of leaf %d has no hash.", col);
	seg_read(seg, l->hash_at, (Size) ns * sizeof(uint32), slot);
	for (uint32 i = 0; i < ns; i++)
		used += slot[i] != 0;
	if (used != d->n)
		corrupt(seg, "The hash of the dictionary of leaf %d holds %u strings, the dictionary %u.", col, used, d->n);
	for (uint32 c = 0; c < d->n; c++)
	{
		uint32		len;
		const char *str = sift_cdict_str(d, c, &len);
		uint64		h = sift_cdict_hash(str, len);
		uint32		i = sift_cdict_hash_pos(h, ns);

		while (slot[i] != 0 && slot[i] != sift_cdict_hash_slot(h, cb, c))
			i = i + 1 == ns ? 0 : i + 1;
		if (slot[i] == 0)
			corrupt(seg, "The hash of the dictionary of leaf %d does not hold code %u.", col, c);
	}
	pfree(slot);
}

/* The values of one block of the row counts of a dictionary: the rows of the codes at the places of block b, DICT_BLOCK of them (the places past n are 0) */
#define COUNT_PACKED_MAX	(DICT_BLOCK * 32 / 8)

static void
count_block(DictLook *l, uint32 b, uint32 *base, uint32 *vals)
{
	SiftColSegment *seg = l->seg;
	uint32		rec[4],
				bytes;
	uint8		cmin,
				buf[COUNT_PACKED_MAX + 8] = {0};

	seg_read(seg, l->cum_at + (uint64) b * 8, sizeof(rec), rec);
	seg_read(seg, l->cum_at + (uint64) 8 * (DICT_COUNT_NB(l->n) + 1) + b, 1, &cmin);
	bytes = rec[3] - rec[1];
	if (rec[3] < rec[1] || bytes % 8 || bytes > COUNT_PACKED_MAX || rec[2] < rec[0])
		corrupt(seg, "The row counts of the dictionary of leaf %d are malformed at block %u.", l->col, b);
	if (bytes > 0)
		seg_read(seg, l->cum_at + DICT_COUNT_HEAD(l->n) + rec[1], bytes, buf);
	*base = rec[0];
	for (uint32 i = 0; i < DICT_BLOCK; i++)
		vals[i] = cmin;
	for (uint32 i = 0, w = bytes / 8; w > 0 && i < DICT_BLOCK; i++)
	{
		uint64		x = 0;
		uint32		bit = i * w;

		memcpy(&x, buf + bit / 8, sizeof(x));
		vals[i] += (uint32) (x >> (bit % 8)) & (uint32) ((UINT64CONST(1) << w) - 1);
	}
}

/* The rows of the codes at the places below pos (0 to n) of the order of a dictionary that counts them */
static uint32
look_cum(DictLook *l, uint32 pos)
{
	uint32		base,
				vals[DICT_BLOCK],
				b = pos / DICT_BLOCK;

	if (pos % DICT_BLOCK == 0)
	{
		seg_read(l->seg, l->cum_at + (uint64) b * 8, sizeof(base), &base);
		return base;
	}
	count_block(l, b, &base, vals);
	for (uint32 i = 0; i < pos % DICT_BLOCK; i++)
		base += vals[i];
	return base;
}

/* sift.col_verify: the counts of the dictionary of leaf col are, at every place of its order, the rows of the code there that the units hold (rows[code]), from zero */
void
sift_cseg_dict_count_check(SiftColSegment *seg, int col, const uint32 *rows)
{
	DictLook   *l = dict_look(seg, col);
	uint32		nb,
				cum = 0,
				last[2];

	Assert(l != NULL && l->cum_at != 0);
	nb = DICT_COUNT_NB(l->n);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		base,
					vals[DICT_BLOCK],
					codes[DICT_BLOCK],
					lo = b * DICT_BLOCK,
					k = Min(l->n - lo, DICT_BLOCK);

		count_block(l, b, &base, vals);
		if (base != cum)
			corrupt(seg, "The row counts of the dictionary of leaf %d start block %u at %u rows, not the %u before it.", col, b, base, cum);
		seg_read(seg, l->perm_at + (uint64) lo * sizeof(uint32), k * sizeof(uint32), codes);
		for (uint32 i = 0; i < DICT_BLOCK; i++)
		{
			if (i >= k)
				break;
			if (codes[i] >= l->n || vals[i] != rows[codes[i]])
				corrupt(seg, "The row count of the string at place %u of the dictionary of leaf %d is not the rows of its code.", lo + i, col);
			cum += vals[i];
		}
		CHECK_FOR_INTERRUPTS();
	}
	seg_read(seg, l->cum_at + (uint64) nb * 8, sizeof(last), last);
	if (last[0] != cum)
		corrupt(seg, "The row counts of the dictionary of leaf %d end at %u rows, not the %u they add up to.", col, last[0], cum);
}

/* The places [from, to) of the byte order of a dictionary read in place whose strings pass op (EQ, LT, LE, GT, GE or PREFIX, bytewise) against c: two bisections */
static void
dict_places(DictLook *l, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to)
{
	SiftCSorted so = {l->n, look_sorted_str, l};

	look_cache(l);
	if (op == SIFT_COLOP_EQ)
	{
		*from = sift_csorted_bound(&so, c, false);
		*to = *from < l->n && sift_csorted_cmp(&so, *from, c) == 0 ? *from + 1 : *from;
	}
	else
		sift_csorted_range(&so, op, c, from, to);
}

/* dict_places for a leaf whose dictionary is searchable in place (false: it is not) */
bool
sift_cseg_dict_places(SiftColSegment *seg, int col, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to)
{
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR || (l = dict_look(seg, col)) == NULL || !l->searchable)
		return false;
	dict_places(l, op, c, from, to);
	return true;
}

/*
 * The rows of the segment whose string of leaf col passes op (EQ, LT, LE, GT, GE or PREFIX, bytewise) against c, from the cumulative counts that a dictionary
 * of the option count_columns stores with its order: two bisections and two reads, whatever the rows. false: the leaf has no counts in this segment.
 */
bool
sift_cseg_dict_count(SiftColSegment *seg, int col, SiftColOp op, const SiftColValue *c, uint64 *rows)
{
	DictLook   *l;
	uint32		from,
				to,
				cum[2];

	if (seg->schema->cols[col].store != SIFT_COLS_STR || (l = dict_look(seg, col)) == NULL || l->cum_at == 0)
		return false;
	dict_places(l, op, c, &from, &to);
	cum[0] = look_cum(l, from);
	cum[1] = look_cum(l, to);
	if (cum[1] < cum[0] || cum[1] > seg->entry.ndocs)
		corrupt(seg, "The row counts of the dictionary of leaf %d are not ascending.", col);
	*rows = cum[1] - cum[0];
	return true;
}

SiftCDict *
sift_cseg_dict_search(SiftColSegment *seg, int col, SiftColOp op)
{
	SegPriv    *pv = PRIV(seg);
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_peek(seg, col);
	if (pv->dict[col] != NULL)
		return pv->dict[col];
	l = dict_look(seg, col);
	/* a dictionary the backend tests again and again is worth its parse, and then its hash table and sorted order, unless the cache cannot hold them: then every parse is lost; a substring test of a blocked one reads its blocks (those the signatures leave) until then, and for good where the parse is lost */
	if (l && (l->searchable || l->hash_at != 0 || (SIFT_COLOP_SCANS(op) && l->blocked && (l->sig_at != 0 || sift_cseg_dict_nokeep(seg, col)))) &&
		((l->hash_at != 0 ? sift_cseg_dict_spent(seg, col, 0) < (uint64) l->n * DICT_PARSE_COST :
		  l->bc != NULL ? l->bc->spent < (uint64) l->n * DICT_PARSE_COST : sift_cseg_dict_asked(seg, col) <= DICT_PARSE_AFTER) || sift_cseg_dict_nokeep(seg, col)))
		return &l->stub;
	return sift_cseg_dict(seg, col);
}

void
sift_cseg_dict_tsig_check(SiftColSegment *seg, int col, const SiftCDict *d)
{
	DictLook   *l = dict_look(seg, col);
	Size		bytes;
	uint8	   *want,
			   *have;

	if (l == NULL || l->sig_at == 0)
		return;
	if (seg->entry.format < SIFT_COL_FORMAT_TSIG)
		corrupt(seg, "The dictionary of leaf %d has trigram signatures in a segment of column format 1.%d.", col, seg->entry.format);
	bytes = (Size) ((l->n + DICT_BLOCK - 1) / DICT_BLOCK) * ((1u << l->sig_lg) / 8);
	want = palloc(bytes);
	have = palloc(bytes);
	sift_cdict_tsig_fill(d, l->sig_lg, want);
	seg_read(seg, l->sig_at, bytes, have);
	if (memcmp(want, have, bytes) != 0)
		corrupt(seg, "The trigram signatures of the dictionary of leaf %d are not those of its strings.", col);
	pfree(want);
	pfree(have);
}

bool
sift_cseg_dict_tsig(SiftColSegment *seg, int col, const char *needle, uint32 len, uint32 *passed, uint32 *total)
{
	DictLook   *l = seg->schema->cols[col].store == SIFT_COLS_STR ? dict_look(seg, col) : NULL;
	SiftColValue c = {.v.s = needle,.len = len};
	uint64	   *lut;

	if (l == NULL || l->sig_at == 0)
		return false;
	lut = palloc0(SIFT_COL_NWORDS(l->n) * sizeof(uint64));
	look_contains(l, SIFT_COLOP_CONTAINS, &c, 1, lut, passed);
	*total = (l->n + DICT_BLOCK - 1) / DICT_BLOCK;
	pfree(lut);
	return true;
}

bool
sift_cseg_dict_lazy(SiftColSegment *seg, int col, uint32 want)
{
	SegPriv    *pv = PRIV(seg);
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return false;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_peek(seg, col);
	if (pv->dict[col] != NULL || (l = dict_look(seg, col)) == NULL)
		return false;
	/* the strings of a few codes cost less than the parse of all of them; the reads of a segment add up over its units */
	if (((uint64) l->reads + want) * DICT_LAZY_RATIO >= l->n)
		return false;
	l->reads += want;
	return true;
}

/* The loads that pay for the whole dictionary: its blocks decompressed one by one cost the same as the parse when they are a DICT_LAZY_LOADS-th of them */
static bool
look_lazy_left(const DictLook *l)
{
	return l->blocked && (uint64) l->loads * DICT_LAZY_LOADS < ((uint64) l->n + DICT_BLOCK - 1) / DICT_BLOCK;
}

static const char *
lazy_get(const void *gctx, uint32 code, uint32 *len, char *buf)
{
	DictLook   *l = (DictLook *) gctx;

	if (!look_lazy_left(l))
		return sift_cdict_str(sift_cseg_dict(l->seg, l->col), code, len);
	return look_code_str(l, code, len, buf);
}

void
sift_col_dict_full(const void *gctx, SiftColDict *self)
{
	const DictLook *l = gctx;
	uint64		id = self->id;

	sift_cdict_view(sift_cseg_dict(l->seg, l->col), id, self);
}

bool
sift_cseg_dict_view_lazy(SiftColSegment *seg, int col, SiftColDict *view)
{
	SegPriv    *pv = PRIV(seg);
	DictLook   *l;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return false;
	if (pv->dict[col] == NULL)
		pv->dict[col] = sift_cseg_dict_peek(seg, col);
	if (pv->dict[col] != NULL || (l = dict_look(seg, col)) == NULL || !look_lazy_left(l))
		return false;
	view->n = l->n;
	view->off = NULL;
	view->bytes = NULL;
	view->order = NULL;
	view->octx = NULL;
	view->get = lazy_get;
	view->gctx = l;
	return true;
}

const char *
sift_cseg_dict_get(SiftColSegment *seg, int col, uint32 code, uint32 *len, char *buf)
{
	DictLook   *l = dict_look(seg, col);

	if (l == NULL || PRIV(seg)->dict[col] != NULL)
		return sift_cdict_str(sift_cseg_dict(seg, col), code, len);
	return look_code_str(l, code, len, buf);
}

/* The smallest minimum and largest maximum of each word of 64 units, and the words with a unit whose zone does not describe its values (overflow, inline strings) */
static void
zone_words(MemoryContext cx, ZoneArrays *z, uint32 nsb)
{
	uint32		nw = (nsb + 63) / 64;
	int64	   *wmin = MemoryContextAlloc(cx, 2 * nw * sizeof(int64)),
			   *wmax = wmin + nw;
	uint64	   *wbad = MemoryContextAllocZero(cx, ((nw + 63) / 64) * sizeof(uint64));

	for (uint32 w = 0; w < nw; w++)
	{
		int64		mn = PG_INT64_MAX,
					mx = PG_INT64_MIN;

		for (uint32 u = w * 64; u < Min(nsb, (w + 1) * 64); u++)
		{
			mn = Min(mn, z->min[u]);
			mx = Max(mx, z->max[u]);
			if (z->flags[u] & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE))
				wbad[w / 64] |= UINT64CONST(1) << (w % 64);
		}
		wmin[w] = mn;
		wmax[w] = mx;
	}
	z->wmin = wmin;
	z->wmax = wmax;
	z->wbad = wbad;
}

const ZoneArrays *
zone_arrays(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);

	if (pv->zones[col] == NULL && HEAD_CACHED())
		pv->zones[col] = sift_cseg_zones_find(seg, col);
	if (pv->zones[col] == NULL)
	{
		const SiftColDef *def = &seg->schema->cols[col];
		uint32		hdr[2];
		uint64		tab[SIFT_COL_MAX_COLS],
					off;
		Size		sz = zone_block_size(def, seg->nsb);
		char	   *blk;
		MemoryContext cx = AllocSetContextCreate(seg->mcxt, "sift cache segment zones", ALLOCSET_SMALL_SIZES);
		ZoneArrays *z = MemoryContextAllocZero(cx, sizeof(ZoneArrays));

		seg_read(seg, s->off, sizeof(hdr), hdr);
		if (hdr[0] != ZONES_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
			corrupt(seg, "ZONES section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
		seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
		off = tab[col];
		if (off % 8 != 0 || off + sz > s->len)
			corrupt(seg, "The zones of leaf %d (%llu, %zu bytes) lie outside the section.", col, ULL(off), sz);
		blk = MemoryContextAlloc(cx, sz);
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
		if (def->store != SIFT_COLS_STR && seg->nsb > 64)
			zone_words(cx, z, seg->nsb);
		pv->zones[col] = z;
		if (HEAD_CACHED())
			sift_cseg_zones_keep(seg, col, cx, z);
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
kinds_exact_at(int format, const SiftColDef *def, uint32 nnull, uint32 nvals)
{
	if (format >= SIFT_COL_FORMAT_KINDS || !sift_col_def_has_kinds(def))
		return true;
	return nnull == 0 && !((def->flags & SIFT_COLD_ASTEXT) && nvals > 0);
}

static bool
leaf_kinds_exact(const SiftColSegment *seg, const SiftColDef *def, uint32 nnull, uint32 nvals)
{
	return kinds_exact_at(seg->entry.format, def, nnull, nvals);
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
#define OVF_STATS_UNITS	4				/* units flagged with an overflow value of a leaf, of a segment, whose overflow slots the statistics count */
static const SetsLeaf *set_leaf(SiftColSegment *seg, int col);

/*
 * The overflow slots among those of an even sample of the units the zones flag (a dead row's value counts until the segment is rewritten): the rows
 * a scan that reads them again from the heap has to fetch. One chunk of each unit is read, once for the statistics the planner keeps.
 */
static void
sample_overflow(SiftColSegment *seg, int col, const ZoneArrays *z, SiftColLeafStats *acc)
{
	uint32		flagged = 0,
				stride,
				k = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
		flagged += (z->flags[u] & SIFT_COLZ_OVERFLOW) != 0;
	stride = Max(flagged / OVF_STATS_UNITS, 1);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCUnitView v;
		SiftCChunk	c;
		const uint8 *rec;
		uint32		len,
					avail;

		if (!(z->flags[u] & SIFT_COLZ_OVERFLOW) || k++ % stride != 0)
			continue;
		rec = sift_cseg_unit_head(seg, u, &len, &avail);
		if (!sift_cunit_parse_head(rec, avail, len, seg->schema, &v) || v.chunk_len[col] == 0)
			continue;
		sift_cseg_unit_fill(seg, u, v.chunk_off[col], v.chunk_len[col]);
		if (!sift_cunit_chunk(&v, col, &c))
			continue;
		acc->ovf_vals += c.n;
		for (uint32 i = 0; c.ovf != NULL && i < (c.n + 63) / 64; i++)
			acc->ovf_slots += sift_pc64(i == c.n / 64 ? c.ovf[i] & ((UINT64CONST(1) << (c.n % 64)) - 1) : c.ovf[i]);
	}
}

/* What the zones of the units of one leaf add to acc: the statistics the planner reads, one unit after the other (the writer keeps them per segment, LstLeaf) */
static void
zone_stats(const SiftColDef *def, int format, uint32 nsb, const ZoneArrays *z, SiftColLeafStats *acc)
{
	for (uint32 u = 0; u < nsb; u++)
	{
		bool		any = z->nnull[u] < z->nvals[u];

		acc->nunits++;
		acc->nvals += z->nvals[u];
		acc->nnull += z->nnull[u];
		acc->nokinds |= !kinds_exact_at(format, def, z->nnull[u], z->nvals[u]);
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
}

/* The leaves whose zones the sample holds: the integer scalars (SiftColZones, zones_new) */
bool
sift_lst_sampled(const SiftColDef *def)
{
	return def->store == SIFT_COLS_INT && def->kind != SIFT_COLK_GROUP;
}

/* The zone of unit u of a sampled leaf as the sample holds it: an overflowed one is the whole range, one without a value the empty range */
static void
sample_zone(const ZoneArrays *z, uint32 u, int64 *lo, int64 *hi)
{
	if (z->flags[u] & SIFT_COLZ_OVERFLOW)
		*lo = PG_INT64_MIN, *hi = PG_INT64_MAX;
	else if (z->nnull[u] >= z->nvals[u])
		*lo = PG_INT64_MAX, *hi = PG_INT64_MIN;
	else
		*lo = z->min[u], *hi = z->max[u];
}

/* A segment of up to SIFT_COL_ZONE_SAMPLE units samples all of them (what the zones gave before the block); a larger one every stride-th */
static uint32
lst_stride(uint32 nsb)
{
	return Max(1, (nsb + SIFT_COL_ZONE_SAMPLE - 1) / SIFT_COL_ZONE_SAMPLE);
}

uint32
sift_lst_nsample(uint32 nsb)
{
	return (nsb + lst_stride(nsb) - 1) / lst_stride(nsb);
}

void
sift_lst_leaf(const SiftColDef *def, int format, uint32 nsb, const ZoneArrays *z, LstLeaf *out, int64 *lo, int64 *hi)
{
	SiftColLeafStats a;

	sift_col_leaf_stats_init(&a);
	zone_stats(def, format, nsb, z, &a);
	memset(out, 0, sizeof(*out));
	out->nvals = a.nvals;
	out->nnull = a.nnull;
	out->sum = a.sum;
	out->span = a.span;
	out->nzone = a.nzone;
	out->novf = a.novf;
	out->min = a.min;
	out->max = a.max;
	out->flags = (a.has_range ? LST_HAS_RANGE : 0) | (a.has_sum ? LST_HAS_SUM : 0) | (a.has_ovf ? LST_HAS_OVF : 0) | (a.has_jnull ? LST_HAS_JNULL : 0) |
		(a.nokinds ? LST_NOKINDS : 0) | (z->any_inl ? LST_ANY_INL : 0) | (z->any_ovf ? LST_ANY_OVF : 0);
	if (lo != NULL)
		for (uint32 k = 0; k < sift_lst_nsample(nsb); k++)
			sample_zone(z, k * lst_stride(nsb), &lo[k], &hi[k]);
}

/*
 * The leaf statistics block of the segment, or NULL: a segment of 64 units or more, written by 3.5, has after its DOCMAP section what a plan
 * adds up over the zones of every unit of every leaf, so that the first plan of a backend reads a few pages for the segment, not
 * its zone arrays. An older segment, or a shorter one, has none (the flag of its directory entry) and is read as before.
 */
static const uint8 *
lst_block(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (!pv->lst_read)
	{
		const SiftSectionRef *d = sec(seg, SIFT_SEC_DOCMAP);
		uint64		at = ALIGN8(d->off + d->len),
					size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;
		LstHeader	h;

		pv->lst_read = true;
		if (seg->entry.flags & SIFT_SEG_LSTATS)
		{
			uint8	   *blk;

			if (at + sizeof(h) > size)
				corrupt(seg, "The directory entry says the segment has leaf statistics, the stream ends at %llu.", ULL(size));
			seg_read(seg, at, sizeof(h), &h);
			if (h.magic != LST_MAGIC || h.ncols != (uint32) seg->schema->ncols || h.nsb != seg->nsb || h.nsample != sift_lst_nsample(seg->nsb) ||
				h.len < sizeof(h) + h.ncols * sizeof(LstLeaf) || at + h.len > size)
				corrupt(seg, "The leaf statistics (0x%08x, %u leaves, %u units, %llu bytes) do not fit the segment.", h.magic, h.ncols, h.nsb, ULL(h.len));
			blk = MemoryContextAlloc(seg->mcxt, h.len);
			seg_read(seg, at, h.len, blk);
			pv->lst = blk;
		}
	}
	return pv->lst;
}

static const LstLeaf *
lst_leaf(const uint8 *blk, int col)
{
	return (const LstLeaf *) (blk + sizeof(LstHeader)) + col;
}

/* The sample of the leaf: nsample lows, then nsample highs (NULL: the leaf has none, or the block is damaged) */
static const int64 *
lst_sample(const uint8 *blk, int col)
{
	const LstHeader *h = (const LstHeader *) blk;
	uint64		off = lst_leaf(blk, col)->sample_off;

	if (off == 0 || off % 8 != 0 || off + 2 * (uint64) h->nsample * sizeof(int64) > h->len)
		return NULL;
	return (const int64 *) (blk + off);
}

/* The statistics block of the segment is what its zone arrays add up to (sift.col_verify) */
void
sift_cseg_verify_lst(SiftColSegment *seg)
{
	const uint8 *blk = lst_block(seg);

	if (blk == NULL)
		return;
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const SiftColDef *def = &seg->schema->cols[c];
		const LstLeaf *have = lst_leaf(blk, c);
		const int64 *smp = lst_sample(blk, c);
		uint32		ns = ((const LstHeader *) blk)->nsample;
		LstLeaf		want;
		int64	   *lo = NULL;

		if (sift_lst_sampled(def) != (have->sample_off != 0) || (have->sample_off != 0 && smp == NULL))
			corrupt(seg, "The leaf statistics of leaf %d have %s sample.", c, have->sample_off != 0 ? "a" : "no");
		if (smp != NULL)
			lo = palloc(2 * (Size) ns * sizeof(int64));
		sift_lst_leaf(def, seg->entry.format, seg->nsb, zone_arrays(seg, c), &want, lo, lo != NULL ? lo + ns : NULL);
		want.sample_off = have->sample_off;
		if (memcmp(&want, have, sizeof(want)) != 0 || (lo != NULL && memcmp(lo, smp, 2 * (Size) ns * sizeof(int64)) != 0))
			corrupt(seg, "The leaf statistics of leaf %d differ from the zones of its units.", c);
		if (lo != NULL)
			pfree(lo);
	}
}

void
sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc)
{
	const SiftColDef *def = &seg->schema->cols[col];
	const uint8 *blk = lst_block(seg);
	const LstLeaf *l = blk != NULL ? lst_leaf(blk, col) : NULL;
	const ZoneArrays *z = NULL;
	bool		inl;

	if (l != NULL && !(l->flags & LST_ANY_OVF))
	{
		/* the sum of the segments' sums is the sum of the units': counts are whole numbers, the spans the estimate they are */
		acc->nunits += seg->nsb;
		acc->nvals += l->nvals;
		acc->nnull += l->nnull;
		acc->nokinds |= (l->flags & LST_NOKINDS) != 0;
		acc->has_jnull |= (l->flags & LST_HAS_JNULL) != 0;
		acc->has_range &= (l->flags & LST_HAS_RANGE) != 0;
		acc->has_sum &= (l->flags & LST_HAS_SUM) != 0;
		acc->has_ovf |= (l->flags & LST_HAS_OVF) != 0;
		acc->novf += l->novf;
		acc->min = Min(acc->min, l->min);
		acc->max = Max(acc->max, l->max);
		acc->span += l->span;
		acc->nzone += l->nzone;
		acc->sum += l->sum;
		inl = (l->flags & LST_ANY_INL) != 0;
	}
	else
	{
		z = zone_arrays(seg, col);
		zone_stats(def, seg->entry.format, seg->nsb, z, acc);
		if (z->any_ovf)
			sample_overflow(seg, col, z, acc);
		inl = z->any_inl;
	}
	if (def->store == SIFT_COLS_STR)
	{
		acc->inl |= inl;
		acc->dict_n = Max(acc->dict_n, dict_count(seg, col));
	}
	{
		const PointLeaf *pl = points_read(seg);

		if (pl != NULL && pl[col].nslots != 0)
		{
			acc->point_units += seg->nsb;
			if (pl[col].tids)
				acc->point_tid_units += seg->nsb;
			acc->point_keys += pl[col].nkeys;
			acc->point_rows += pl[col].count;
		}
	}
	if (seg->entry.format >= SIFT_COL_FORMAT_HEADS && sift_cseg_run_kinds(seg, col) != 0)
	{
		/* a unit has the head of its run or not (a key-cut segment's, a unit with a NULL or an overflowed row): the share of an even sample */
		uint32		stride = Max(seg->nsb / SET_STATS_UNITS, 1),
					seen = 0,
					with = 0;
		SiftCRun	r;
		uint64		at;

		for (uint32 u = 0; u < seg->nsb; u += stride, seen++)
			with += sift_cseg_run(seg, col, u, &r, &at) && r.heads > 0;
		acc->head_units += (double) seg->nsb * with / Max(seen, 1);
	}
	{
		for (int k = 0; k < 2; k++)
		{
			const GOrdLeaf *g = gord_leaf(seg, col + k * seg->schema->ncols);

			if (g != NULL)
			{
				acc->gord_rows += g->hdr.entries;
				acc->gord_bytes += g->bytes;
				if (g->hdr.gcol >= 0)
				{
					acc->gord_grp_rows += g->hdr.entries;
					acc->gord_gcol = g->hdr.gcol;
				}
			}
		}
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

/* The next candidate of the sample: kept when it is the stride's turn, the sample thinned to every other entry (and the stride doubled) when it is full */
static bool
sample_offer(SiftColZones *zs)
{
	uint64		at = zs->seen++;

	if (at % zs->stride != 0)
		return false;
	if (zs->n == SIFT_COL_ZONE_SAMPLE)
	{
		/* the units kept are the even ones: every other sample goes, and one candidate in twice as many is sampled from here on */
		for (int c = 0; c < zs->ncols; c++)
			if (zs->lo[c] != NULL)
				for (uint32 k = 0; k < SIFT_COL_ZONE_SAMPLE / 2; k++)
				{
					zs->lo[c][k] = zs->lo[c][2 * k];
					zs->hi[c][k] = zs->hi[c][2 * k];
				}
		zs->n /= 2;
		zs->stride *= 2;
		if (at % zs->stride != 0)
			return false;
	}
	return true;
}

void
sift_cseg_zone_sample(SiftColSegment *seg, SiftColZones *zs)
{
	const ZoneArrays *z[SIFT_COL_MAX_COLS] = {NULL};
	const uint8 *blk = lst_block(seg);
	const int64 *smp[SIFT_COL_MAX_COLS] = {NULL};
	bool		fast = blk != NULL;

	for (int c = 0; c < seg->schema->ncols && c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
		{
			if (fast && (smp[c] = lst_sample(blk, c)) == NULL)
				fast = false;
		}
	if (fast)
	{
		/* the sampled units, from the block: the zone arrays of the leaves are not read */
		uint32		ns = ((const LstHeader *) blk)->nsample;

		for (uint32 k = 0; k < ns; k++)
		{
			if (!sample_offer(zs))
				continue;
			for (int c = 0; c < zs->ncols; c++)
				if (smp[c] != NULL)
				{
					zs->lo[c][zs->n] = smp[c][k];
					zs->hi[c][zs->n] = smp[c][ns + k];
				}
			zs->n++;
		}
		zs->units += seg->nsb;
		return;
	}
	for (int c = 0; c < seg->schema->ncols && c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
			z[c] = zone_arrays(seg, c);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		if (!sample_offer(zs))
			continue;
		for (int c = 0; c < zs->ncols; c++)
			if (z[c] != NULL)
				sample_zone(z[c], u, &zs->lo[c][zs->n], &zs->hi[c][zs->n]);
		zs->n++;
	}
	zs->units += seg->nsb;
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
		if (ver > 3 && PAGG_SUPERDIR(h) &&
			(PAGG_SUPERDIR(h) % 8 != 0 || !within(s->len - end, PAGG_SUPERDIR(h), PAGG_SUPER_BYTES(h), 0) || PAGG_SUPER_BYTES(h) < sizeof(SuperHeader)))
			corrupt(seg, "The superunit block (%llu bytes at %llu) lies outside the section.", ULL(PAGG_SUPER_BYTES(h)), ULL(PAGG_SUPERDIR(h)));
		if (ver > 3 && !PAGG_SUPERDIR(h) && PAGG_SUPER_BYTES(h))
			corrupt(seg, "The superunit block has %llu bytes and no offset.", ULL(PAGG_SUPER_BYTES(h)));
		if (ver > 4 && PAGG_RUNS(h) &&
			(PAGG_RUNS(h) % 8 != 0 || PAGG_RUNS_BYTES(h) < sizeof(RunsHeader) + sizeof(RunsLeaf) || !within(s->len - end, PAGG_RUNS(h), PAGG_RUNS_BYTES(h), 0)))
			corrupt(seg, "The sorted runs (%llu bytes at %llu) lie outside the section.", ULL(PAGG_RUNS_BYTES(h)), ULL(PAGG_RUNS(h)));
		if (ver > 5 && PAGG_XCUBEDIR(h) &&
			(PAGG_XCUBEDIR(h) % 8 != 0 || !within(s->len - end, PAGG_XCUBEDIR(h), (seg->nsb + 1) * sizeof(uint64), PAGG_XCUBE_BYTES(h))))
			corrupt(seg, "The exception cubes (%llu bytes at %llu) lie outside the section.", ULL(PAGG_XCUBE_BYTES(h)), ULL(PAGG_XCUBEDIR(h)));
		if (ver > 5 && PAGG_XECUBEDIR(h) &&
			(PAGG_XECUBEDIR(h) % 8 != 0 || !within(s->len - end, PAGG_XECUBEDIR(h), (seg->nsb + 1) * sizeof(uint64), PAGG_XECUBE_BYTES(h))))
			corrupt(seg, "The exception element blobs (%llu bytes at %llu) lie outside the section.", ULL(PAGG_XECUBE_BYTES(h)), ULL(PAGG_XECUBEDIR(h)));
		if (ver > 6 && PAGG_HLL(h) &&
			(PAGG_HLL(h) % 8 != 0 || PAGG_HLL_BYTES(h) < sizeof(SetsHeader) + sizeof(SetsLeaf) || !within(s->len - end, PAGG_HLL(h), PAGG_HLL_BYTES(h), 0)))
			corrupt(seg, "The sketches (%llu bytes at %llu) lie outside the section.", ULL(PAGG_HLL_BYTES(h)), ULL(PAGG_HLL(h)));
		if (ver > 7 && PAGG_KTAB(h) &&
			(PAGG_KTAB(h) % 8 != 0 || PAGG_KTAB_BYTES(h) < 8 || !within(s->len - end, PAGG_KTAB(h), PAGG_KTAB_BYTES(h), 0)))
			corrupt(seg, "The key tables (%llu bytes at %llu) lie outside the section.", ULL(PAGG_KTAB_BYTES(h)), ULL(PAGG_KTAB(h)));
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
unit_blob(SiftColSegment *seg, uint32 n, uint32 u, uint64 dir_off, uint64 bytes, uint64 **dir, uint8 **buf, Size *cap, uint32 *len)
{
	uint64		at = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + dir_off,
				b0,
				b1;

	*len = 0;
	if (!dir_off)
		return NULL;
	if (*dir == NULL)
	{
		*dir = MemoryContextAlloc(seg->mcxt, (n + 1) * sizeof(uint64));
		seg_read(seg, at, (n + 1) * sizeof(uint64), *dir);
		for (uint32 i = 0; i < n; i++)
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
	seg_read(seg, at + (n + 1) * sizeof(uint64) + b0, b1 - b0, *buf);
	*len = b1 - b0;
	return *buf;
}

const uint8 *
sift_cseg_cube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	return unit_blob(seg, seg->nsb, u, h->cubedir_off, h->cube_bytes, &pv->cubedir, &pv->cubebuf, &pv->cubecap, len);
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
	return unit_blob(seg, seg->nsb, u, PAGG_ECUBEDIR(h), PAGG_ECUBE_BYTES(h), &pv->ecubedir, &pv->ecubebuf, &pv->ecubecap, len);
}

/* The exception cube of unit u (docs/COL-PREAGG.md 11): the cube of the rows where the unit's exception leaf is not NULL, with the dims of the unit's own cube */
const uint8 *
sift_cseg_xcube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	if (h->version < 6)
	{
		*len = 0;
		return NULL;
	}
	return unit_blob(seg, seg->nsb, u, PAGG_XCUBEDIR(h), PAGG_XCUBE_BYTES(h), &pv->xcubedir, &pv->xcubebuf, &pv->xcubecap, len);
}

/* The same for the element blob */
const uint8 *
sift_cseg_xecube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	if (h->version < 6)
	{
		*len = 0;
		return NULL;
	}
	return unit_blob(seg, seg->nsb, u, PAGG_XECUBEDIR(h), PAGG_XECUBE_BYTES(h), &pv->xecubedir, &pv->xecubebuf, &pv->xecubecap, len);
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

/* The superunit block's header, checked against the segment; NULL when the segment has none */
static const SuperHeader *
super_header(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (!pv->super_checked)
	{
		const PaggHeader *ph = pagg_header(seg);

		if (ph->version > 3 && PAGG_SUPERDIR(ph))
		{
			SuperHeader *h = MemoryContextAlloc(seg->mcxt, sizeof(SuperHeader));
			uint64		bytes = PAGG_SUPER_BYTES(ph),
						need;
			uint32		nleaf = 0;

			seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + PAGG_SUPERDIR(ph), sizeof(SuperHeader), h);
			for (int c = 0; c < seg->schema->ncols; c++)
				nleaf += super_leaf_ok(&seg->schema->cols[c]);
			if (h->magic != SUPER_MAGIC || h->fan < SIFT_SUPER_MIN_FAN || h->fan > SIFT_SUPER_MAX_FAN || h->nsu != (seg->nsb + h->fan - 1) / h->fan ||
				(h->nsu == 1 && h->fan != seg->nsb) || h->nleaf != nleaf)
				corrupt(seg, "The superunit block starts with 0x%08x, groups of %u units, %u groups and %u leaves for %u units and %u leaves.", h->magic,
						h->fan, h->nsu, h->nleaf, seg->nsb, nleaf);
			need = sizeof(SuperHeader) + (uint64) seg->nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64) + (uint64) nleaf * h->nsu * sizeof(SiftCSuperLeaf);
			if (h->cubedir_off == 0 ? (h->cube_bytes != 0 || bytes != need) :
				(h->cubedir_off != need || h->cube_bytes > bytes || bytes != need + (h->nsu + 1) * sizeof(uint64) + h->cube_bytes))
				corrupt(seg, "The superunit block holds %llu bytes, its sections say %llu and a cube of %llu bytes at %llu.", ULL(bytes), ULL(need),
						ULL(h->cube_bytes), ULL(h->cubedir_off));
			pv->super = h;
			pv->sleaf = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(SiftCSuperLeaf *));
		}
		pv->super_checked = true;
	}
	return pv->super;
}

uint32
sift_cseg_super_fan(SiftColSegment *seg)
{
	const SuperHeader *h = super_header(seg);

	return h ? h->fan : 0;
}

void
sift_cseg_super_pagemask(SiftColSegment *seg, uint32 k, uint32 n, uint64 *pm)
{
	const SuperHeader *h = super_header(seg);

	if (h == NULL || k >= h->nsu || n > h->fan || (uint64) k * h->fan + n > seg->nsb)
		corrupt(seg, "The page masks of superunit %u were asked for %u units.", k, n);
	seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SUPERDIR(PRIV(seg)->pagg) + sizeof(SuperHeader) +
			 (uint64) k * h->fan * SIFT_PAGEMASK_WORDS * sizeof(uint64), (Size) n * SIFT_PAGEMASK_WORDS * sizeof(uint64), pm);
}

/* The pages the rows of unit u are on, from the superunit summary (4 words), without reading the unit; false when the segment has no summary */
bool
sift_cseg_unit_pagemask(SiftColSegment *seg, uint32 u, uint64 *pm)
{
	const SuperHeader *h = super_header(seg);

	if (h == NULL || u >= seg->nsb)
		return false;
	seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SUPERDIR(PRIV(seg)->pagg) + sizeof(SuperHeader) +
			 (uint64) u * SIFT_PAGEMASK_WORDS * sizeof(uint64), SIFT_PAGEMASK_WORDS * sizeof(uint64), pm);
	return true;
}

static int
sb_span_cmp(const void *a, const void *b)
{
	uint32		x = ((const SiftCSbSpan *) a)->lo,
				y = ((const SiftCSbSpan *) b)->lo;

	return x < y ? -1 : x > y;
}

uint32
sift_cseg_sb_spans(SiftColSegment *seg, const SiftCSbSpan **spans)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->spans == NULL)
	{
		SiftCSbSpan *r = MemoryContextAlloc(seg->mcxt, Max(seg->nsb, 1) * sizeof(SiftCSbSpan));
		bool		keyed = sift_cseg_keyed(&seg->entry);
		uint32		n = 0;

		for (uint32 u = 0; u < seg->nsb; u++)
		{
			uint64		pm[SIFT_PAGEMASK_WORDS];

			if (!keyed)
				r[u] = (SiftCSbSpan) {seg->sbdir[u].sb, seg->sbdir[u].sb};
			else if (sift_cseg_unit_pagemask(seg, u, pm))
				r[u] = (SiftCSbSpan) {(uint32) pm[0] / SIFT_SB_PAGES, (uint32) (pm[0] >> 32) / SIFT_SB_PAGES};
			else
				r[u] = (SiftCSbSpan) {0, PG_UINT32_MAX};
		}
		if (keyed)
			qsort(r, seg->nsb, sizeof(SiftCSbSpan), sb_span_cmp);
		for (uint32 u = 0; u < seg->nsb; u++)
			if (n > 0 && r[u].lo <= (uint64) r[n - 1].hi + 1)
				r[n - 1].hi = Max(r[n - 1].hi, r[u].hi);
			else
				r[n++] = r[u];
		pv->spans = r;
		pv->nspans = n;
	}
	*spans = pv->spans;
	return pv->nspans;
}

bool
sift_cseg_sb_in_spans(SiftColSegment *seg, uint32 sb)
{
	const SiftCSbSpan *r;
	uint32		lo = 0,
				hi = sift_cseg_sb_spans(seg, &r),
				n = hi;

	SIFT_BISECT(uint32, mid, lo, hi, r[mid].hi < sb);
	return lo < n && r[lo].lo <= sb;
}

bool
sift_cseg_super_agg(SiftColSegment *seg, int col, uint32 k, SiftCAgg *a)
{
	SegPriv    *pv = PRIV(seg);
	const SuperHeader *h = super_header(seg);
	const SiftCSuperLeaf *r;
	int			slot = 0;

	if (h == NULL || k >= h->nsu || !super_leaf_ok(&seg->schema->cols[col]))
		return false;
	if (pv->sleaf[col] == NULL)
	{
		SiftCSuperLeaf *blk = MemoryContextAlloc(seg->mcxt, (Size) h->nsu * sizeof(SiftCSuperLeaf));

		for (int c = 0; c < col; c++)
			slot += super_leaf_ok(&seg->schema->cols[c]);
		seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + PAGG_SUPERDIR(pv->pagg) + sizeof(SuperHeader) +
				 (uint64) seg->nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64) + (uint64) slot * h->nsu * sizeof(SiftCSuperLeaf),
				 (Size) h->nsu * sizeof(SiftCSuperLeaf), blk);
		for (uint32 i = 0; i < h->nsu; i++)
		{
			const SiftCSuperLeaf *l = &blk[i];

			if ((l->flags & ~(SIFT_CAGG_SUMSQ_OVF | SIFT_SUPER_LEAF_OK)) || (l->zflags & ~SIFT_COLZ_CONST) ||
				((l->flags & SIFT_SUPER_LEAF_OK) ? (l->nnull > l->nvals || (l->nnull < l->nvals && l->min > l->max) ||
													((l->flags & SIFT_CAGG_SUMSQ_OVF) && (l->sumsq_lo || l->sumsq_hi)) ||
													(seg->schema->cols[col].store == SIFT_COLS_STR && (l->sum_lo || l->sum_hi || l->sumsq_lo || l->sumsq_hi || l->flags != SIFT_SUPER_LEAF_OK))) :
				 (l->flags || l->zflags || l->nvals || l->nnull || l->sum_lo || l->sum_hi)))
				corrupt(seg, "The summary of leaf %d in superunit %u is malformed.", col, i);
		}
		pv->sleaf[col] = blk;
	}
	r = &pv->sleaf[col][k];
	if (!(r->flags & SIFT_SUPER_LEAF_OK))
		return false;
	memset(a, 0, sizeof(*a));
	a->nvals = r->nvals;
	a->nnull = r->nnull;
	a->min = r->min;
	a->max = r->max;
	a->zflags = r->zflags;
	a->flags = r->flags & SIFT_CAGG_SUMSQ_OVF;
	a->sum = (SiftCI128) (((unsigned __int128) r->sum_hi << 64) | r->sum_lo);
	a->sumsq = (SiftCI128) (((unsigned __int128) r->sumsq_hi << 64) | r->sumsq_lo);
	a->has_sums = seg->schema->cols[col].store == SIFT_COLS_INT;
	return true;
}

const uint8 *
sift_cseg_super_cube(SiftColSegment *seg, uint32 k, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const SuperHeader *h = super_header(seg);

	*len = 0;
	if (h == NULL || h->cubedir_off == 0 || k >= h->nsu)
		return NULL;
	return unit_blob(seg, h->nsu, k, PAGG_SUPERDIR(pv->pagg) + h->cubedir_off, h->cube_bytes, &pv->supdir, &pv->supbuf, &pv->supcap, len);
}

#ifdef SIFT_TESTING
bool
sift_cseg_test_super_span(SiftColSegment *seg, uint64 *off, uint64 *len)
{
	const PaggHeader *h = pagg_header(seg);

	if (h->version < 4 || !PAGG_SUPERDIR(h))
		return false;
	*off = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SUPERDIR(h);
	*len = PAGG_SUPER_BYTES(h);
	return true;
}
#endif

uint64
sift_cseg_set_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = pagg_header(seg);

	return h->version > 2 ? PAGG_SETS_BYTES(h) : 0;
}

uint64
sift_cseg_run_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = pagg_header(seg);

	return h->version > 4 ? PAGG_RUNS_BYTES(h) : 0;
}

uint64
sift_cseg_hll_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = pagg_header(seg);

	return h->version > 6 ? PAGG_HLL_BYTES(h) : 0;
}

uint64
sift_cseg_preagg_bytes(SiftColSegment *seg)
{
	pagg_header(seg);
	return sec(seg, SIFT_SEC_DICTIDX)->len - PRIV(seg)->pagg_end - sift_cseg_set_bytes(seg) - sift_cseg_run_bytes(seg) - sift_cseg_hll_bytes(seg);
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
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);
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
			!within(room, rel, (uint64) (seg->nsb + 1) * sizeof(uint64), r->bytes))
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
static const SetsLeaf *
set_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_SETS)
		return NULL;
	h = pagg_header(seg);
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
	const SetsLeaf *sl = set_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	const uint8 *blob;

	*len = 0;
	if (sl == NULL)
		return NULL;
	blob = unit_blob(seg, seg->nsb, u, sl->dir_off, sl->bytes, &pv->setdir[col], &pv->setbuf, &pv->setcap, len);
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
	h = pagg_header(seg);
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
	blob = unit_blob(seg, seg->nsb, u, sl->dir_off, sl->bytes, &pv->hlldir[col], &pv->hllbuf, &pv->hllcap, len);
	if (pv->hlldir[col][seg->nsb] != sl->bytes)
		corrupt(seg, "The directory of the sketches of leaf %d ends at %llu, not at %llu.", col, ULL(pv->hlldir[col][seg->nsb]), ULL(sl->bytes));
	if (blob != NULL && !sift_hll_check(blob, *len))
		corrupt(seg, "The sketch of leaf %d in unit %u is malformed.", col, u);
	return blob;
}

/* The key tables' bytes at *off, from the file start; false when the segment has none */
static bool
ktab_span(SiftColSegment *seg, uint64 *off, uint64 *len)
{
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_KTAB || (h = pagg_header(seg))->version < 8 || !PAGG_KTAB(h))
		return false;
	*off = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_KTAB(h);
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

	if (!ktab_span(seg, &off, &len))
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
static const RunsLeaf *
run_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_RUNS)
		return NULL;
	h = pagg_header(seg);
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
	const RunsLeaf *rl = run_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	uint64		base,
				b0,
				b1;
	uint8		hdr[SIFT_CRUN_HDR];

	if (rl == NULL)
		return false;
	base = sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + rl->dir_off;
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
	const SetsLeaf *sl = set_leaf(seg, col);

	return sl ? sl->flags : 0;
}

uint32
sift_cseg_run_kinds(SiftColSegment *seg, int col)
{
	const RunsLeaf *rl = run_leaf(seg, col);

	return rl ? rl->flags : 0;
}

#ifdef SIFT_TESTING
/* The span of the sorted runs in the stream and, for each byte of it, 2 when it is structure (header, records, directories, the head of a run), 1 when it is an entry of a run and 0 for the padding between blocks */
bool
sift_cseg_test_run_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	const PaggHeader *h = pagg_header(seg);
	RunsHeader	rh;

	if (seg->entry.format < SIFT_COL_FORMAT_RUNS || h->version < 5 || !PAGG_RUNS(h))
		return false;
	*off = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_RUNS(h);
	*len = PAGG_RUNS_BYTES(h);
	seg_read(seg, *off, sizeof(rh), &rh);
	*used = palloc0(*len);
	memset(*used, 2, sizeof(rh) + (uint64) rh.nleaves * sizeof(RunsLeaf));
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const RunsLeaf *rl = run_leaf(seg, c);
		uint64		dir = rl ? rl->dir_off - PAGG_RUNS(h) : 0;

		if (rl == NULL)
			continue;
		memset(*used + dir, 2, (uint64) (seg->nsb + 1) * sizeof(uint64));
		memset(*used + dir + (uint64) (seg->nsb + 1) * sizeof(uint64), 1, rl->bytes);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCRun	r;
			uint64		at;

			if (sift_cseg_run(seg, c, u, &r, &at))
				memset(*used + (at - *off), 2, SIFT_CRUN_HDR);
		}
	}
	return true;
}

/* The same for the small-set summaries: 2 for the header, the records, the directories and the head of a blob, 1 for the rest of a blob */
bool
sift_cseg_test_set_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	const PaggHeader *h = pagg_header(seg);
	SetsHeader	sh;

	if (seg->entry.format < SIFT_COL_FORMAT_SETS || h->version < 3 || !PAGG_SETS(h))
		return false;
	*off = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SETS(h);
	*len = PAGG_SETS_BYTES(h);
	seg_read(seg, *off, sizeof(sh), &sh);
	*used = palloc0(*len);
	memset(*used, 2, sizeof(sh) + (uint64) sh.nleaves * sizeof(SetsLeaf));
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const SetsLeaf *sl = set_leaf(seg, c);
		uint64		dir = sl ? sl->dir_off - PAGG_SETS(h) : 0;

		if (sl == NULL)
			continue;
		memset(*used + dir, 2, (uint64) (seg->nsb + 1) * sizeof(uint64));
		memset(*used + dir + (uint64) (seg->nsb + 1) * sizeof(uint64), 1, sl->bytes);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			uint32		blen;
			const uint8 *blob = sift_cseg_set(seg, c, u, &blen);
			uint64		start = dir + (uint64) (seg->nsb + 1) * sizeof(uint64);
			uint64		o;

			if (blob == NULL)
				continue;
			seg_read(seg, *off + dir + (uint64) u * sizeof(uint64), sizeof(o), &o);
			memset(*used + start + o, 2, SIFT_CSET_HDR);
		}
	}
	return true;
}

/* The same for the key tables: every byte is used */
bool
sift_cseg_test_ktab_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	if (!ktab_span(seg, off, len))
		return false;
	*used = palloc(*len);
	memset(*used, 1, *len);
	{
		MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "key table damage map", ALLOCSET_SMALL_SIZES);
		uint8 *buf = MemoryContextAlloc(cx, *len);

		seg_read(seg, *off, *len, buf);
		for (uint64 at = 0; at < *len;)
		{
			SiftCHistDim d;
			uint32 n = sift_ktab_decode(seg->schema, buf + at, *len - at, cx, &d);

			if (n == 0)
				corrupt(seg, "The key table at %llu of the section is malformed.", ULL(at));
			memset(*used + at, 2, SIFT_KTAB_HEADER_BYTES);
			at += n;
		}
		MemoryContextDelete(cx);
	}
	return true;
}

/* The same for the point directories: 2 for the header, the records of the leaves and the footer, 1 for the tables of slots and the lists */
bool
sift_cseg_test_point_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	const PointLeaf *pl = points_read(seg);
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint32		hdr[4];

	if (pl == NULL)
		return false;
	*off = s->off + PRIV(seg)->dict_end;
	*len = s->len - PRIV(seg)->dict_end;
	seg_read(seg, *off, sizeof(hdr), hdr);
	*used = palloc0(*len);
	memset(*used, 2, sizeof(hdr) + (uint64) hdr[2] * SIFT_CPOINT_RECLEN);
	memset(*used + *len - 16, 2, 16);
	for (int c = 0; c < seg->schema->ncols; c++)
		if (pl[c].nslots > 0)
			memset(*used + (pl[c].at - *off), 1, (uint64) pl[c].nslots * sizeof(uint64) + (uint64) pl[c].words * sizeof(uint32));
	return true;
}
#endif

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
	if ((sl = set_leaf(seg, col)) == NULL)
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

/* Calls fn(slot payload) for each slot of leaf pl whose fingerprint is that of key's hash: the slots from the position of the hash to the first empty one */
static void
point_probe(SiftColSegment *seg, int col, const PointLeaf *pl, uint64 h, void (*fn) (void *, uint64, uint32), void *arg)
{
	uint32		p = sift_cpoint_pos(h, pl->nslots);

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
		p = (p + m) & (pl->nslots - 1);
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
	const PointLeaf *pl = points_read(seg);
	PointRows	pr = {.seg = seg,.col = col,.cap = 16};
	uint32		u = 0;

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	pr.pl = pl + col;
	pr.total = seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows;
	pr.r = palloc(pr.cap * sizeof(uint32));
	for (int k = 0; k < nkeys && !pr.over; k++)
		point_probe(seg, col, pr.pl, sift_cpoint_hash(keys[k]), point_add_rows, &pr);
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
	const PointLeaf *pl = points_read(seg);

	return pl ? pl[col].ndense : 0;
}

bool
sift_cseg_point_has(SiftColSegment *seg, int col, int64 key, uint32 row, bool *dense)
{
	const PointLeaf *pl = points_read(seg);
	PointHas	ph = {.seg = seg,.col = col,.row = row};

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	ph.pl = pl + col;
	point_probe(seg, col, ph.pl, sift_cpoint_hash(key), point_find_row, &ph);
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
		u = sift_cseg_find_unit(seg, sb);
		if (u >= seg->nsb || seg->sbdir[u].sb != sb)
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
	const PointLeaf *pl = points_read(seg);

	return pl != NULL && pl[col].nslots != 0 && pl[col].tids;
}

static bool
point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, const uint64 *keep, uint32 limit, uint64 *seen, SiftKey **tids, uint32 *ntids)
{
	const PointLeaf *pl = points_read(seg);
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
		point_probe(seg, col, a.pl, sift_cpoint_hash(keys[k]), point_add_tids, &a);
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
			DictLook   *l = dict_look(s->seg, s->col);
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
	const PointLeaf *pl = points_read(seg);
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
		point_probe(seg, col, pl + col, sift_cpoint_hash(keys[k]), stream_slot, s);
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
	const PointLeaf *pl = points_read(seg);
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
	DictLook   *l = dict_look(seg, col);
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
	const PointLeaf *pl = points_read(seg);
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
		point_probe(seg, col, a.pl, sift_cpoint_hash(e[i].key), point_add_tids, &a);
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
	const PointLeaf *pl = points_read(seg);

	return pl ? pl[col].count : 0;
}

/* the distinct keys of the leaf's directory */
uint32
sift_cseg_point_nkeys(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = points_read(seg);

	return pl ? pl[col].nkeys : 0;
}

/* ---- the global order of a leaf (col/gorder.h) ---- */

/* The block of leaf col with its header read (the directory entries are read and checked when they are used, the whole directory by verify), NULL when the segment has none */
static GOrdLeaf *
gord_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	GOrdLeaf   *g;
	SiftCGOrdHdr h;
	Size		front;
	uint8	   *raw;

	points_read(seg);
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
	if (g->gat + (uint64) h.ngroups * SIFT_CGORD_GRPENT != g->bytes)
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

/* The slot of the block of leaf col to read: the one sorted by group (the second slot, ncols + col; the first when it is grouped, as the user's order_array_group writes it) or the plain one; -1 when the segment has none */
int
sift_cseg_gorder_slot(SiftColSegment *seg, int col, bool grouped)
{
	const GOrdLeaf *g = grouped ? gord_leaf(seg, seg->schema->ncols + col) : NULL;

	if (g != NULL)
		return seg->schema->ncols + col;
	g = gord_leaf(seg, col);
	return g != NULL && (g->hdr.gcol >= 0) == grouped ? col : -1;
}

bool
sift_cseg_gorder(SiftColSegment *seg, int col, uint32 *entries, uint32 *values)
{
	GOrdLeaf   *g = gord_leaf(seg, col);

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
	GOrdLeaf   *g = gord_leaf(seg, col);
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
	GOrdLeaf   *g = gord_leaf(seg, col);
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
	GOrdLeaf   *g = gord_leaf(seg, col);

	return g == NULL ? -1 : g->hdr.gcol;
}

/*
 * The entries of a group of a grouped block: the value entries [*vs, *ve) and the NULL ones [*ns, *ne); empty ranges when the group has none. A block that is not
 * grouped has one group of all its entries.
 */
void
sift_cseg_gorder_range(SiftColSegment *seg, int col, int64 group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	GOrdLeaf   *g = gord_leaf(seg, col);
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
	uint32		at = c * gord_leaf(seg, col)->hdr.crows;

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
	GOrdLeaf   *g = gord_leaf(seg, col);
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
	GOrdLeaf   *g = gord_leaf(seg, col);

	if (g == NULL || g->hdr.gcol >= 0)
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
	crows = gord_leaf(seg, col)->hdr.crows;
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
	GOrdLeaf   *g = gord_leaf(seg, col);
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
	uint32		have = UINT32_MAX;
	int64		pk = 0;
	uint64		pa = 0;
	uint32		nranges;
	SiftCGOrdGrp *grp = NULL;

	if (g == NULL)
		return;
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
			if (p % 4096 == 0)
				CHECK_FOR_INTERRUPTS();
		}
	}
}

void
sift_cseg_verify_points(SiftColSegment *seg)
{
	const PointLeaf *pl = points_read(seg);
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
