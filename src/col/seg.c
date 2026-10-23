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

/*
 * The point directories at the end of the DICT section, found by the footer in its last 16 bytes: the tables of the leaves, read on
 * first use. A section without them ends with its last dictionary. The frame is checked whole, so that no byte of it can change what is read without a
 * refusal: the reserved words are 0, the records ascend by leaf, and the tables follow the records one after the other and end at the footer (a record
 * dropped, a leaf changed or a table moved shows as a gap or an overlap). The bytes of a table are the check of the segment's (sift_cseg_verify_points).
 */
static bool
point_version_ok(uint32 v)
{
	return v == SIFT_CPOINT_VERSION_ROWS || v == SIFT_CPOINT_VERSION;
}

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
	if (hdr[0] != SIFT_CPOINT_MAGIC || hdr[1] != tail[1] || hdr[2] > (uint32) seg->schema->ncols || hdr[3] != 0 ||
		foot[0] + sizeof(hdr) + (uint64) hdr[2] * SIFT_CPOINT_RECLEN > s->len - sizeof(foot))
		corrupt(seg, "The point directories of the DICT section have the header 0x%08x, version %u, %u leaves.", hdr[0], hdr[1], hdr[2]);
	next = foot[0] + sizeof(hdr) + (uint64) hdr[2] * SIFT_CPOINT_RECLEN;
	pv->pt = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(PointLeaf));
	for (uint32 i = 0; i < hdr[2]; i++)
	{
		uint32		rec[6];
		uint64		at;
		uint64		rat = s->off + foot[0] + sizeof(hdr) + (uint64) i * SIFT_CPOINT_RECLEN;
		uint64		size;

		seg_read(seg, rat, sizeof(rec), rec);
		seg_read(seg, rat + sizeof(rec), sizeof(at), &at);
		size = ALIGN8((uint64) rec[1] * sizeof(uint64) + (uint64) rec[4] * sizeof(uint32));
		if (rec[0] >= (uint32) seg->schema->ncols || (int64) rec[0] <= prev || rec[5] > (hdr[1] == SIFT_CPOINT_VERSION ? SIFT_CPOINT_MODE_TIDS : SIFT_CPOINT_MODE_ROWS) ||
			rec[1] < 16 || (rec[1] & (rec[1] - 1)) != 0 || rec[3] == 0 ||
			rec[3] > rec[1] || rec[2] < rec[3] || rec[2] >= SIFT_CPOINT_SEG_ROWS || (uint64) rec[4] > (rec[5] == SIFT_CPOINT_MODE_TIDS ? 3 * (uint64) rec[2] : (uint64) rec[2] + rec[3]) ||
			at != next || size > s->len - sizeof(foot) - next)
			corrupt(seg, "The point directory %u of the DICT section is malformed.", i);
		pv->pt[rec[0]] = (PointLeaf) {s->off + at, rec[1], rec[3], rec[2], rec[4], rec[5] == SIFT_CPOINT_MODE_TIDS};
		prev = rec[0];
		next += size;
	}
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
	else if (l->blkz[0] != SIFT_CC_ALG_ZSTD || !sift_colz_available(l->blkz[0]))
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

static uint32
look_perm(const DictLook *l, uint32 pos)
{
	uint32		code;

	seg_read(l->seg, l->perm_at + (uint64) pos * sizeof(uint32), sizeof(code), &code);
	if (code >= l->n)
		corrupt(l->seg, "The order of the dictionary of leaf %d names code %u of %u.", l->col, code, l->n);
	return code;
}

/* A string of the order by position; the buffer is the one of the search (a bisection holds one string at a time) */
static const char *
look_sorted_str(const void *ctx, uint32 pos, uint32 *len)
{
	const DictLook *l = ctx;

	return look_code_str((DictLook *) l, look_perm(l, pos), len, ((DictLook *) l)->buf);
}

static int64
look_find(const SiftCDictLook *look, const char *s, uint32 len)
{
	const DictLook *l = (const DictLook *) look;
	SiftCSorted so = {l->n, look_sorted_str, l};
	SiftColValue c = {.v.s = s,.len = len};
	uint32		pos = sift_csorted_bound(&so, &c, false);

	return pos < l->n && sift_csorted_cmp(&so, pos, &c) == 0 ? (int64) look_perm(l, pos) : -1;
}

static uint32
look_match(const SiftCDictLook *look, SiftColOp op, const SiftColValue *c, uint64 *lut)
{
	const DictLook *l = (const DictLook *) look;
	SiftCSorted so = {l->n, look_sorted_str, l};
	uint32		from,
				to;

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

	if (pv->look_checked[col])
		return pv->look[col];
	pv->look_checked[col] = true;
	dict_region(seg, col, &off, &len);
	off += sec(seg, SIFT_SEC_DICT)->off;
	if (len < DICT_HEAD)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	seg_read(seg, off, DICT_HEAD, head);
	if (head[0] != DICT_LOOKUP && head[0] != DICT_BLOCKS && head[0] != DICT_PLAIN)
		return NULL;
	l = MemoryContextAllocZero(seg->mcxt, sizeof(DictLook));
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
		if (head[0] == DICT_LOOKUP)
		{
			l->perm_at = (l->blk_at + blen + 3) / 4 * 4;
			if (l->perm_at + (uint64) l->n * sizeof(uint32) > off + len)
				corrupt(seg, "The dictionary of leaf %d is malformed.", col);
			l->look = (SiftCDictLook) {look_find, look_match};
			l->stub.n = l->n;
			l->stub.look = &l->look;
			l->searchable = true;
		}
		else if (l->blk_at + blen > off + len)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	}
	pv->look[col] = l;
	return l;
}

SiftCDict *
sift_cseg_dict_search(SiftColSegment *seg, int col)
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
	/* a dictionary the backend tests again and again is worth its parse, and then its hash table and sorted order */
	if (l && l->searchable && sift_cseg_dict_asked(seg, col) <= DICT_PARSE_AFTER)
		return &l->stub;
	return sift_cseg_dict(seg, col);
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

const char *
sift_cseg_dict_get(SiftColSegment *seg, int col, uint32 code, uint32 *len, char *buf)
{
	DictLook   *l = dict_look(seg, col);

	if (l == NULL || PRIV(seg)->dict[col] != NULL)
		return sift_cdict_str(sift_cseg_dict(seg, col), code, len);
	return look_code_str(l, code, len, buf);
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
	if (z->any_ovf)
		sample_overflow(seg, col, z, acc);
	if (def->store == SIFT_COLS_STR)
	{
		acc->inl |= z->any_inl;
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
		if (ver > 3 && PAGG_SUPERDIR(h) &&
			(PAGG_SUPERDIR(h) % 8 != 0 || !within(s->len - end, PAGG_SUPERDIR(h), PAGG_SUPER_BYTES(h), 0) || PAGG_SUPER_BYTES(h) < sizeof(SuperHeader)))
			corrupt(seg, "The superunit block (%llu bytes at %llu) lies outside the section.", ULL(PAGG_SUPER_BYTES(h)), ULL(PAGG_SUPERDIR(h)));
		if (ver > 3 && !PAGG_SUPERDIR(h) && PAGG_SUPER_BYTES(h))
			corrupt(seg, "The superunit block has %llu bytes and no offset.", ULL(PAGG_SUPER_BYTES(h)));
		if (ver > 4 && PAGG_RUNS(h) &&
			(PAGG_RUNS(h) % 8 != 0 || PAGG_RUNS_BYTES(h) < sizeof(RunsHeader) + sizeof(RunsLeaf) || !within(s->len - end, PAGG_RUNS(h), PAGG_RUNS_BYTES(h), 0)))
			corrupt(seg, "The sorted runs (%llu bytes at %llu) lie outside the section.", ULL(PAGG_RUNS_BYTES(h)), ULL(PAGG_RUNS(h)));
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
sift_cseg_preagg_bytes(SiftColSegment *seg)
{
	pagg_header(seg);
	return sec(seg, SIFT_SEC_DICTIDX)->len - PRIV(seg)->pagg_end - sift_cseg_set_bytes(seg) - sift_cseg_run_bytes(seg);
}

/* ---------- small-set summaries ---------- */

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

			if (r->leaf >= (uint32) ncols || (int) r->leaf <= prev || r->flags == 0 || (r->flags & ~(SETS_EXACT | SETS_BLOOM | SETS_PRES)) || !sift_cset_leaf_ok(seg->schema, &seg->schema->cols[r->leaf]))
				corrupt(seg, "The small-set record %u names leaf %u, which cannot have one.", i, r->leaf);
			if (r->dir_off < lowest || r->dir_off % 8 != 0 ||
				!within(room, rel, (uint64) (seg->nsb + 1) * sizeof(uint64), r->bytes))
				corrupt(seg, "The small-sets of leaf %u (%llu bytes at %llu) lie outside the section.", r->leaf, ULL(r->bytes), ULL(r->dir_off));
			lowest = ALIGN8(r->dir_off + (uint64) (seg->nsb + 1) * sizeof(uint64) + r->bytes);
			prev = r->leaf;
			sets[r->leaf] = *r;
		}
		/* the records are all there are: one fewer would leave the last leaf's summaries unread and unnoticed */
		if (lowest != PAGG_SETS(h) + room)
			corrupt(seg, "The small-set summaries end at %llu, not at the %llu the section has.", ULL(lowest), ULL(PAGG_SETS(h) + room));
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
	if (s.kind == SIFT_CSET_PLIST)
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

/* ---------- sorted runs ---------- */

/* The record of leaf col in the section, read and checked with the whole section on first use; NULL when the leaf has no run in any unit */
static const RunsLeaf *
run_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;
	int			ncols = seg->schema->ncols;

	if (seg->entry.format < SIFT_COL_FORMAT_RUNS)
		return NULL;
	h = pagg_header(seg);
	if (h->version < 5 || !PAGG_RUNS(h))
		return NULL;
	if (pv->runs == NULL)
	{
		const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);
		uint64		at = s->off + pv->pagg_end + PAGG_RUNS(h),
					room = PAGG_RUNS_BYTES(h),
					lowest;
		RunsHeader	rh;
		RunsLeaf   *rec,
				   *runs;
		int			prev = -1;

		seg_read(seg, at, sizeof(rh), &rh);
		if (rh.magic != RUNS_MAGIC || rh.version != 1 || rh.nsb != seg->nsb || rh.nleaves < 1 || rh.nleaves > (uint32) ncols ||
			sizeof(rh) + (uint64) rh.nleaves * sizeof(RunsLeaf) > room)
			corrupt(seg, "The sorted runs start with 0x%08x, version %u, %u leaves, %u units.", rh.magic, rh.version, rh.nleaves, rh.nsb);
		rec = palloc(rh.nleaves * sizeof(RunsLeaf));
		seg_read(seg, at + sizeof(rh), rh.nleaves * sizeof(RunsLeaf), rec);
		lowest = PAGG_RUNS(h) + sizeof(rh) + rh.nleaves * sizeof(RunsLeaf);
		runs = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(RunsLeaf));
		for (uint32 i = 0; i < rh.nleaves; i++)
		{
			const RunsLeaf *r = &rec[i];
			uint64		rel = r->dir_off - PAGG_RUNS(h);

			if (r->leaf >= (uint32) ncols || (int) r->leaf <= prev || r->flags == 0 || (r->flags & ~(RUNS_ORD | RUNS_IDENT)) || !sift_crun_leaf_ok(seg->schema, &seg->schema->cols[r->leaf]))
				corrupt(seg, "The sorted-run record %u names leaf %u, which cannot have one.", i, r->leaf);
			if (r->dir_off < lowest || r->dir_off % 8 != 0 ||
				!within(room, rel, (uint64) (seg->nsb + 1) * sizeof(uint64), r->bytes))
				corrupt(seg, "The sorted runs of leaf %u (%llu bytes at %llu) lie outside the section.", r->leaf, ULL(r->bytes), ULL(r->dir_off));
			lowest = ALIGN8(r->dir_off + (uint64) (seg->nsb + 1) * sizeof(uint64) + r->bytes);
			prev = r->leaf;
			runs[r->leaf] = *r;
		}
		pfree(rec);
		pv->rundir = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(uint64 *));
		pv->runs = runs;
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
	seg_read(seg, at, sift_crun_bytes(r->kind, r->n), dst);
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

static int
row_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return (x > y) - (x < y);
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
point_probe(SiftColSegment *seg, int col, const PointLeaf *pl, uint64 h, void (*fn) (void *, uint64), void *arg)
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
				fn(arg, win[j]);
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
point_add_rows(void *arg, uint64 slot)
{
	PointRows  *pr = arg;
	uint32		payload = (uint32) slot,
				cnt = 1;

	if (pr->over)
		return;
	if (payload & SIFT_CPOINT_LIST)
		cnt = point_list_count(pr->seg, pr->col, pr->pl, payload & ~SIFT_CPOINT_LIST);
	if (pr->n + cnt > SIFT_CPOINT_MAX_ROWS)
	{
		pr->over = true;
		return;
	}
	if (pr->n + cnt > pr->cap)
		pr->r = repalloc(pr->r, (pr->cap = Max(pr->cap * 2, pr->n + cnt)) * sizeof(uint32));
	if (payload & SIFT_CPOINT_LIST)
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
	qsort(pr.r, pr.n, sizeof(uint32), row_cmp);
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
} PointHas;

static void
point_find_row(void *arg, uint64 slot)
{
	PointHas   *ph = arg;
	uint32		payload = (uint32) slot,
				lo = 0,
				hi = 1,
				off = payload & ~SIFT_CPOINT_LIST;
	uint64		base = ph->pl->at + (uint64) ph->pl->nslots * sizeof(uint64);

	if (!(payload & SIFT_CPOINT_LIST))
	{
		ph->found |= payload == ph->row;
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

bool
sift_cseg_point_has(SiftColSegment *seg, int col, int64 key, uint32 row)
{
	const PointLeaf *pl = points_read(seg);
	PointHas	ph = {.seg = seg,.col = col,.row = row};

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	ph.pl = pl + col;
	point_probe(seg, col, ph.pl, sift_cpoint_hash(key), point_find_row, &ph);
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
} TidAcc;

#define TID_PAYLOAD(slot)	((slot) & ((UINT64CONST(1) << 48) - 1))

static void
tid_push(TidAcc *a, SiftKey key)
{
	SiftColSegment *seg = a->seg;
	uint32		sb = (uint32) (key >> 16) >> SIFT_SB_SHIFT,
				u = sift_cseg_find_unit(seg, sb);

	if ((key & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF || (key & 0xFFFF) == 0 || u >= seg->nsb || seg->sbdir[u].sb != sb)
		corrupt(seg, "The point directory of leaf %d lists the address %llx, which is in no unit of the segment.", a->col, (unsigned long long) key);
	if (a->n == a->cap)
		a->t = a->t ? repalloc_huge(a->t, (a->cap *= 2) * sizeof(SiftKey)) : palloc((a->cap = 16) * sizeof(SiftKey));
	a->t[a->n++] = key;
}

/* Appends the addresses of one slot of a table of addresses to a (ascending in a list), and counts the words of its list */
static void
tid_slot_read(TidAcc *a, uint64 slot)
{
	const PointLeaf *pl = a->pl;
	uint64		pay = TID_PAYLOAD(slot),
				base,
				prev = 0;
	uint32		off,
				hdr[2],
				pad;
	uint8	   *buf;
	const uint8 *p,
			   *end;

	if ((pay & 0xFFFF) != SIFT_CPOINT_TID_LIST)
	{
		if (a->limit && a->n + 1 > a->limit)
			a->over = true;
		else
			tid_push(a, pay);
		return;
	}
	off = (uint32) (pay >> 16);
	if (pay >> 48 || off >= pl->words || pl->words - off < 2)
		corrupt(a->seg, "The point directory of leaf %d has a list at word %llu of %u.", a->col, (unsigned long long) (pay >> 16), pl->words);
	base = pl->at + (uint64) pl->nslots * sizeof(uint64) + (uint64) off * sizeof(uint32);
	seg_read(a->seg, base, sizeof(hdr), hdr);
	if (hdr[0] < 2 || hdr[0] > pl->count || hdr[1] < hdr[0] || hdr[1] > 6 * hdr[0] || (uint64) off + 2 + (hdr[1] + 3) / 4 > pl->words)
		corrupt(a->seg, "The point directory of leaf %d has a list of %u addresses in %u bytes at word %u of %u.", a->col, hdr[0], hdr[1], off, pl->words);
	a->words += 2 + (hdr[1] + 3) / 4;
	if (a->limit && a->n + hdr[0] > a->limit)
	{
		a->over = true;
		return;
	}
	pad = (hdr[1] + 3) / 4 * 4;
	buf = palloc(pad);
	seg_read(a->seg, base + sizeof(hdr), pad, buf);
	for (uint32 i = hdr[1]; i < pad; i++)
		if (buf[i] != 0)
			corrupt(a->seg, "The point directory of leaf %d has a list at word %u whose padding is not zero.", a->col, off);
	p = buf;
	end = buf + hdr[1];
	for (uint32 i = 0; i < hdr[0]; i++)
	{
		uint64		d;

		if (!sift_cpoint_get_varint(&p, end, &d) || (i > 0 && d == 0) || (d >> 41) || ((prev + d) >> 41))
			corrupt(a->seg, "The point directory of leaf %d has a list at word %u that does not hold %u ascending addresses.", a->col, off, hdr[0]);
		prev += d;
		tid_push(a, sift_cpoint_tid_key(prev));
	}
	if (p != end)
		corrupt(a->seg, "The point directory of leaf %d has a list at word %u that is longer than its %u addresses.", a->col, off, hdr[0]);
	pfree(buf);
}

static void
point_add_tids(void *arg, uint64 slot)
{
	TidAcc	   *a = arg;

	if (!a->over)
		tid_slot_read(a, slot);
}

static int
tid_cmp(const void *a, const void *b)
{
	SiftKey		x = *(const SiftKey *) a,
				y = *(const SiftKey *) b;

	return (x > y) - (x < y);
}

bool
sift_cseg_point_is_tid(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = points_read(seg);

	return pl != NULL && pl[col].nslots != 0 && pl[col].tids;
}

bool
sift_cseg_point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, SiftKey **tids, uint32 *ntids)
{
	const PointLeaf *pl = points_read(seg);
	TidAcc		a = {.seg = seg,.col = col,.limit = SIFT_CPOINT_MAX_ROWS};
	uint32		u = 0;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids)
		return false;
	a.pl = pl + col;
	for (int k = 0; k < nkeys && !a.over; k++)
		point_probe(seg, col, a.pl, sift_cpoint_hash(keys[k]), point_add_tids, &a);
	if (a.over)
	{
		if (a.t)
			pfree(a.t);
		return false;
	}
	qsort(a.t, a.n, sizeof(SiftKey), tid_cmp);
	for (uint32 i = 0; i < a.n; i++)
		if (u == 0 || a.t[u - 1] != a.t[i])
			a.t[u++] = a.t[i];
	*tids = a.t;
	*ntids = u;
	return true;
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
			qsort(a.t, a.n, sizeof(SiftKey), tid_cmp);
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

/* does the sorted set hold a key in [lo, hi]? */
static bool
set_meets(const int64 *set, uint32 n, int64 lo, int64 hi)
{
	uint32		a = 0,
				b = n;

	SIFT_BISECT(uint32, m, a, b, set[m] < lo);
	return a < n && set[a] <= hi;
}

/*
 * Narrows keep for the units [ulo, uhi) to those whose zone may pass t. The passes run over the words of keep that hold the range (the units of those words
 * outside it are narrowed too, which is exact), so that a begin of two superblocks of a segment of thousands tests those only: an aggregate begins for every
 * morsel, and a begin that tested the whole segment made the scan quadratic in its units.
 */
void
sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	uint32		lo = ulo / 64 * 64,
				hi,
				n,
				nw;
	uint64	   *kw = keep + lo / 64;
	uint64	   *tmp;
	uint64	   *save = NULL;

	uhi = Min(uhi, seg->nsb);
	if (ulo >= uhi)
		return;
	hi = Min(seg->nsb, (uhi + 63) / 64 * 64);
	n = hi - lo;
	nw = (n + 63) / 64;
	tmp = palloc(SIFT_COL_NWORDS(n) * sizeof(uint64));
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
					if ((keep[u / 64] >> (u % 64) & 1) && z->nnull[u] < z->nvals[u] && !set_meets(t->set, t->nset, z->min[u], z->max[u]))
						keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set + lo, n, t->fold, kw);
			sift_cseg_prune_sets(seg, col, t, keep, ulo, uhi, setcut);
			break;
		case SIFT_CT_LUT:
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set + lo, n, t->fold, kw);
			sift_cseg_prune_sets(seg, col, t, keep, ulo, uhi, setcut);
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
