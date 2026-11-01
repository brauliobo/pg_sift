/*
 * col/seg/gorder.c - the global order of a segment: the sorted keys of a column across its
 * units, the strings of a text order, the ranges and seeks a scan makes
 * in it and the addresses a range lists
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/seg_int.h"
#include "col/gquant.h"

/* ---- the global order of a leaf (col/gorder.h) ---- */

static bool gord_probe_ok(SiftColSegment *seg, GOrdLeaf *g, int col);

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

		/* a directory of a few pages follows the header on the page the header was read from: it is read whole here, once, in place of its last entry and then again by the first seek (a page read twice is two blocks) */
		if (g->ents == NULL && (Size) h.chunks * SIFT_CGORD_DIRENT <= SIFT_CGORD_DIR_EAGER)
		{
			g->ents = MemoryContextAlloc(seg->mcxt, (Size) h.chunks * SIFT_CGORD_DIRENT);
			seg_read(seg, g->at + SIFT_CGORD_HDR, (Size) h.chunks * SIFT_CGORD_DIRENT, g->ents);
		}
		if (g->ents != NULL)
			memcpy(raw + SIFT_CGORD_HDR, g->ents + (uint64) (h.chunks - 1) * SIFT_CGORD_DIRENT, SIFT_CGORD_DIRENT);
		else
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
void
seg_gord_dir_read(SiftColSegment *seg, GOrdLeaf *g)
{
	if (g->ents == NULL)
	{
		Size		len = (Size) g->hdr.chunks * SIFT_CGORD_DIRENT;

		g->ents = MemoryContextAlloc(seg->mcxt, Max(len, 1));
		seg_read(seg, g->at + SIFT_CGORD_HDR, len, g->ents);
	}
}

/* the directory entry of chunk c, checked against the block */
SiftCGOrdDir
seg_gord_dirent(SiftColSegment *seg, const GOrdLeaf *g, int col, uint32 c)
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
uint64
seg_gord_strs(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	uint64		seg_sec = g->at + g->bytes - g->hdr.strbytes;

	if (g->sbno == 0 && g->nstr == 0)
	{
		uint8		head[16];
		uint32		nblk;

		seg_read(seg, seg_sec, sizeof(head), head);
		if (g->hdr.strperm ? !sift_cgord_perm_head(head, &g->hdr, &g->nstr, &g->pbits) : !sift_cgord_str_head(head, &g->hdr, &g->nstr, &nblk))
			corrupt(seg, "The strings of the global order of leaf %d are malformed.", col);
		g->sbno = UINT32_MAX;
	}
	return seg_sec;
}

/* The dictionary code of the string of rank r in a block of version 5 */
uint32
seg_gord_perm(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 r)
{
	uint64		bit = (uint64) r * g->pbits;
	uint8		buf[8];
	Size		nb = (Size) (((bit & 7) + g->pbits + 7) / 8);

	seg_read(seg, seg_gord_strs(seg, g, col) + 16 + bit / 8, nb, buf);
	return sift_ckeyed_get(buf, nb, bit & 7, g->pbits);
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
	seg_sec = seg_gord_strs(seg, g, col);
	if (r >= g->nstr)
		corrupt(seg, "The global order of leaf %d has no string %u.", col, r);
	if (g->hdr.strperm)
	{
		uint32		code = seg_gord_perm(seg, g, col, r);

		if (code >= g->nstr)
			corrupt(seg, "The global order of leaf %d names the code %u of %u strings.", col, code, g->nstr);
		/* the strings of a few ranks are read in place; the dictionary is parsed, and kept, once the backend's reads of it have cost about what the parse does */
		if (g->sdict == NULL)
			g->sdict = sift_cseg_dict_peek(seg, col);
		if (g->sdict == NULL && sift_cseg_dict_spent(seg, col, 0) >= (uint64) g->nstr * DICT_PARSE_COST && !sift_cseg_dict_nokeep(seg, col))
			g->sdict = sift_cseg_dict(seg, col);
		if (g->sdict != NULL)
			return sift_cdict_str(g->sdict, code, len);
		if (g->sblk == NULL)
			g->sblk = MemoryContextAlloc(seg->mcxt, SIFT_CGORD_STR_MAX);
		sift_cseg_dict_spent(seg, col, DICT_ZSTD_COST + DICT_READ_COST);
		return sift_cseg_dict_get(seg, col, code, len, (char *) g->sblk);
	}
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
	(void) seg_gord_strs(seg, g, col);
	hi = g->nstr;
	SIFT_BISECT(uint32, mid, lo, hi, after ? gord_str_vs(seg, g, col, mid, s, slen) <= 0 : gord_str_vs(seg, g, col, mid, s, slen) < 0);
	return lo;
}

/* The slot of the block of leaf col to read: the one sorted by group (the second slot, ncols + col; the first when it is grouped, as the user's order_array_group writes it) or the plain one; -1 when the segment has none */
int
sift_cseg_gorder_slot(SiftColSegment *seg, int col, bool grouped)
{
	GOrdLeaf   *g = grouped ? seg_gord_leaf(seg, seg->schema->ncols + col) : NULL;

	if (g != NULL)
		return seg->schema->ncols + col;
	g = seg_gord_leaf(seg, col);
	return g != NULL && (g->hdr.gcol >= 0) == grouped && gord_probe_ok(seg, g, col) ? col : -1;
}

/* The host's reader of the heap rows the compact blocks take their exact keys from (col/seg/probe.c); unset on a host with no heap: such a block is not used and its scans read the units */
bool		(*sift_gprobe_ok) (SiftColSegment *seg, GOrdLeaf *g, int col) = NULL;
void	   *(*sift_gprobe_open) (SiftColSegment *seg, GOrdLeaf *g, int col) = NULL;
void		(*sift_gprobe_close) (void *probe) = NULL;
bool		(*sift_gprobe_key) (void *probe, uint64 addr, int64 *key) = NULL;

/* Whether the keys a compact block does not store can be read from the heap (asked once for a block); a block that cannot is not used */
static bool
gord_probe_ok(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	if (g->hdr.quant == 0)
		return true;
	if (g->probe == 0)
		g->probe = sift_gprobe_ok != NULL && sift_gprobe_ok(seg, g, col) ? 1 : -1;
	return g->probe > 0;
}

/* The heap rows a window of keys is read from (opened for the window and closed with it) */
typedef struct GOrdProbe
{
	void	   *h;
} GOrdProbe;

static void
probe_open(SiftColSegment *seg, GOrdLeaf *g, int col, GOrdProbe *pr)
{
	pr->h = g->hdr.quant != 0 && gord_probe_ok(seg, g, col) ? sift_gprobe_open(seg, g, col) : NULL;
}

static void
probe_close(GOrdProbe *pr)
{
	if (pr->h != NULL)
		sift_gprobe_close(pr->h);
	pr->h = NULL;
}

/* The key of the row at addr, from its heap tuple; false when the heap has none (a dead row) */
static bool
probe_key(const GOrdProbe *pr, uint64 addr, int64 *key)
{
	return pr->h != NULL && sift_gprobe_key(pr->h, addr, key);
}

/*
 * The exact key of entry i of the compact chunk in buf, from the heap row, clamped to its bucket (entry 0 and a bucket of one key are answered by the bucket's first key); false when
 * the heap cannot answer, which is a dead row: the slot is pruned or vacuumed, or has no value. The executor drops a dead row wherever it stands, so what its entry is given only
 * has to leave the live entries around it in order, and any reader of the keys of a compact chunk (the seek, the cursors, a decoder that fills them) keeps this rule:
 * - a seek passes over it to the next entry that can be read and lets that one decide (the first key of its bucket is below the key the row had, and would put the live entries of
 *   the same key before the bound);
 * - a cursor gives it the key of the entry before it: not below any live entry before it and not above any after it, whichever way the cursor reads.
 */
static bool
compact_probe(const GOrdProbe *pr, SiftCGOrdBuf *buf, uint32 i, int64 *key)
{
	int64		lo = buf->kmin[i],
				hi = i == 0 ? lo : sift_gq_kmax(lo, buf->sh),
				k;

	*key = lo;
	if (lo == hi)
		return true;
	if (!probe_key(pr, buf->addr[i], &k))
		return false;
	*key = Max(lo, Min(hi, k));
	return true;
}

/*
 * The key of entry i of the compact chunk in buf for the order of a cursor (kept in key[i], with its exact bit). Where the heap has no answer (a dead row) it is the key of the entry
 * before, so that it neither passes the live entries after it nor hides them when the cursor reads backwards.
 */
static int64
compact_key(const GOrdProbe *pr, SiftCGOrdBuf *buf, uint32 i)
{
	if (!(buf->exact[i / 64] >> (i % 64) & 1))
	{
		int64		k;

		if (!compact_probe(pr, buf, i, &k))
			k = i == 0 ? buf->kmin[0] : Max(buf->kmin[i], compact_key(pr, buf, i - 1));
		buf->key[i] = k;
		buf->exact[i / 64] |= UINT64CONST(1) << (i % 64);
	}
	return buf->key[i];
}

/* The chunk c of a compact block into buf: its addresses and, with keys, the buckets of its entries; no exact key yet */
static void
compact_load(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, bool keys, SiftCGOrdBuf *buf)
{
	uint32		m = sift_cgord_chunk_rows(&g->hdr, c);
	uint8	   *raw = palloc(ALIGN8((Size) d->klen) + ALIGN8((Size) d->alen) + SIFT_COL_PAD);

	seg_read(seg, g->at + d->off, ALIGN8((Size) d->klen) + ALIGN8((Size) d->alen), raw);
	if (!sift_cgord_decode_compact(seg->mcxt, raw, d->klen, raw + ALIGN8((Size) d->klen), d->alen, d->pad, m, d->first, m, keys ? buf->kmin : NULL, &buf->sh, buf->addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	pfree(raw);
	buf->at = c * g->hdr.crows;
	buf->rows = buf->cm = m;
	buf->compact = true;
	buf->kmin_ok = keys;
	buf->kfrom = buf->kn = 0;
	memset(buf->exact, 0, sizeof(buf->exact));
}

/* The exact key of entry i of the compact chunk in buf into key[i] (the entry alone becomes the window of keys) */
static void
compact_resolve(SiftColSegment *seg, GOrdLeaf *g, int col, SiftCGOrdBuf *buf, uint32 i)
{
	if (!(buf->exact[i / 64] >> (i % 64) & 1))
	{
		GOrdProbe	pr;

		probe_open(seg, g, col, &pr);
		compact_key(&pr, buf, i);
		probe_close(&pr);
	}
	buf->kfrom = i;
	buf->kn = 1;
}

/* Every key of the compact chunk c: verify and the listing of a block read them */
static void
compact_all(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, SiftCGOrdBuf *buf)
{
	GOrdProbe	pr;

	compact_load(seg, g, col, c, d, true, buf);
	probe_open(seg, g, col, &pr);
	for (uint32 i = 0; i < buf->rows; i++)
		compact_key(&pr, buf, i);
	probe_close(&pr);
	buf->kfrom = 0;
	buf->kn = buf->rows;
}

/* A complete block: the entries and values it holds. False for a segment without one and for one that keeps the ends only (sift_cseg_gorder_ends): nothing counts or lists a range from those */
bool
sift_cseg_gorder(SiftColSegment *seg, int col, uint32 *entries, uint32 *values)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL || g->hdr.vtotal != 0 || !gord_probe_ok(seg, g, col))
		return false;
	*entries = g->hdr.entries;
	*values = g->hdr.values;
	return true;
}

/* A block of the ends only: its entries, the values it holds ([0, head) the first end, the rest the last), the values the segment has and its rows with no value; false when the block is complete or not there */
bool
sift_cseg_gorder_ends(SiftColSegment *seg, int col, uint32 *entries, uint32 *values, uint32 *head, uint32 *vtotal)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL || g->hdr.vtotal == 0 || g->hdr.gcol >= 0 || g->hdr.strbytes != 0)
		return false;
	*entries = g->hdr.entries;
	*values = g->hdr.values;
	*head = g->hdr.head;
	*vtotal = g->hdr.vtotal;
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
	d = seg_gord_dirent(seg, g, col, c);
	m = sift_cgord_chunk_rows(&g->hdr, c);
	if (d.pad & SIFT_CGORD_COMPACT)
	{
		SiftCGOrdBuf *all = palloc(sizeof(SiftCGOrdBuf));

		compact_all(seg, g, col, c, &d, all);
		w = Min(w, m);
		memcpy(key, all->key, w * sizeof(int64));
		memcpy(addr, all->addr, w * sizeof(uint64));
		pfree(all);
		return w;
	}
	buf = palloc(ALIGN8((Size) d.klen) + ALIGN8((Size) d.alen) + SIFT_COL_PAD);
	seg_read(seg, g->at + d.off, ALIGN8((Size) d.klen) + ALIGN8((Size) d.alen), buf);
	if (!sift_cgord_decode(seg->mcxt, buf, d.klen, buf + ALIGN8((Size) d.klen), d.alen, d.pad, m, w, key, addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	pfree(buf);
	return Min(w, m);
}

/*
 * The first max(w, up to p) entries of the chunk that holds the entry p, into buf (SiftCGOrdBuf: its first entry, the entries of the chunk, the count decoded). A compact chunk is
 * decoded once (every address and the buckets) and gives the exact key of p alone; keys false: the addresses only.
 */
static void
gord_fill(SiftColSegment *seg, int col, uint32 p, uint32 w, bool keys, SiftCGOrdBuf *buf)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	SiftCGOrdDir d;
	uint32		c;

	if (g == NULL)
		corrupt(seg, "The global order of leaf %d is not there.", col);
	c = p / g->hdr.crows;
	d = seg_gord_dirent(seg, g, col, c);
	if (d.pad & SIFT_CGORD_COMPACT)
	{
		if (buf->at != c * g->hdr.crows || !buf->compact || (keys && !buf->kmin_ok))
			compact_load(seg, g, col, c, &d, keys, buf);
		if (keys && p - buf->at - buf->kfrom >= buf->kn)
			compact_resolve(seg, g, col, buf, p - buf->at);
		return;
	}
	buf->at = c * g->hdr.crows;
	buf->rows = sift_cgord_chunk_rows(&g->hdr, c);
	buf->cm = sift_cseg_gorder_head(seg, col, c, Max(w, p - buf->at + 1), buf->key, buf->addr);
	buf->compact = false;
	buf->kfrom = 0;
	buf->kn = buf->cm;
}

void
sift_cseg_gorder_fill(SiftColSegment *seg, int col, uint32 p, uint32 w, SiftCGOrdBuf *buf)
{
	gord_fill(seg, col, p, w, w != UINT32_MAX, buf);
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
		gord_fill(seg, col, at, UINT32_MAX, false, memo);
	return memo->cm;
}

/*
 * The first entry of [from, to) in the compact chunk c whose key is not before key (after: is after it); to when there is none. The buckets say which entries are certainly before
 * key (their last key is) and which certainly are not (their first is): the entries between, in the bucket that holds key, are searched by the keys of their heap rows (0.7 of them a seek
 * for keys 4 buckets to a mean gap). The chunk is left in memo with its addresses.
 */
/* The first entry i0 of the compact chunk in buf that is not certainly before key (its last key is not: before(k) is k < key, or k <= key for after) and the first i1 that certainly is not (its first key is not before) */
static void
compact_bounds(const SiftCGOrdBuf *buf, uint32 rows, int64 key, bool after, uint32 *i0, uint32 *i1)
{
	uint32		lo = 0,
				hi = rows;

	SIFT_BISECT(uint32, mid, lo, hi, after ? (mid == 0 ? buf->kmin[0] : sift_gq_kmax(buf->kmin[mid], buf->sh)) <= key : (mid == 0 ? buf->kmin[0] : sift_gq_kmax(buf->kmin[mid], buf->sh)) < key);
	*i0 = lo;
	hi = rows;
	SIFT_BISECT(uint32, mid, lo, hi, after ? buf->kmin[mid] <= key : buf->kmin[mid] < key);
	*i1 = lo;
}

static uint32
compact_seek(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, uint32 from, uint32 to, int64 key, bool after, SiftCGOrdBuf *memo)
{
	GOrdProbe	pr;
	uint32		at = c * g->hdr.crows,
				lo,
				hi,
				i0,
				i1;
	SiftCGOrdDir d = seg_gord_dirent(seg, g, col, c);

	if (memo->at != at || !memo->compact || !memo->kmin_ok)
		compact_load(seg, g, col, c, &d, true, memo);
	compact_bounds(memo, to - at, key, after, &i0, &i1);
	lo = Max(i0, from - at);
	hi = Min(i1, to - at);
	probe_open(seg, g, col, &pr);
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2,
					j = mid;
		int64		k;

		/* an entry the heap cannot answer for is a dead row (the executor drops it wherever it stands): the next one that can be read decides, and when none can, the rest are dead */
		while (j < hi && !compact_probe(&pr, memo, j, &k))
			j++;
		if (j < hi && (after ? k <= key : k < key))
			lo = j + 1;
		else
			hi = mid;
	}
	probe_close(&pr);
	return Min(Max(at + lo, from), to);
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
	seg_gord_dir_read(seg, g);
	SIFT_BISECT(uint32, mid, lo, hi, after ? seg_gord_dirent(seg, g, col, mid).first <= key : seg_gord_dirent(seg, g, col, mid).first < key);
	lo--;
	if (seg_gord_dirent(seg, g, col, lo).pad & SIFT_CGORD_COMPACT)
		return compact_seek(seg, g, col, lo, Max(vs, lo * crows), Min(ve, (lo + 1) * crows), key, after, memo);
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

/*
 * The entries [*a, *b) of the global order of leaf col (ungrouped) whose key lies in [lo, hi]; false when the segment has no such block. The keys of a text block are
 * ranks of the segment's strings (str: those of sift_cseg_gorder_strkeys), not the codes of its dictionary that a range of the scan is in.
 */
static bool
gorder_span(SiftColSegment *seg, int col, bool str, int64 lo, int64 hi, uint32 *a, uint32 *b, SiftCGOrdBuf *memo)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL || g->hdr.gcol >= 0 || (g->hdr.strbytes != 0) != str || g->hdr.vtotal != 0 || !gord_probe_ok(seg, g, col))
		return false;
	*a = lo > hi ? 0 : gorder_seek(seg, col, 0, g->hdr.values, lo, false, memo);
	*b = lo > hi ? 0 : gorder_seek(seg, col, *a, g->hdr.values, hi, true, memo);
	return true;
}

/*
 * The ranks of the strings of a text block that lie between two bounds, as the keys [*klo, *khi] of its entries (empty: *klo > *khi): the rows of the strings from lo (after
 * it when lo_excl) to hi (before it when hi_excl); a NULL bound is none. False when the leaf has no text block of the collation of the comparison.
 */
bool
sift_cseg_gorder_strkeys(SiftColSegment *seg, int col, uint32 collid, const char *lo, uint32 lolen, bool lo_excl, const char *hi, uint32 hilen, bool hi_excl, int64 *klo, int64 *khi)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL || g->hdr.strbytes == 0 || g->hdr.collid != collid)
		return false;
	*klo = lo == NULL ? 0 : sift_cseg_gorder_strrank(seg, col, lo, lolen, lo_excl);
	*khi = hi == NULL ? PG_INT64_MAX : (int64) sift_cseg_gorder_strrank(seg, col, hi, hilen, !hi_excl) - 1;
	return true;
}

/* How many rows of the segment have a key in [lo, hi], by the global order of the leaf; UINT32_MAX when it has none */
uint32
sift_cseg_gorder_count(SiftColSegment *seg, int col, bool str, int64 lo, int64 hi)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b;

	memo.at = UINT32_MAX;
	return gorder_span(seg, col, str, lo, hi, &a, &b, &memo) ? b - a : UINT32_MAX;
}

/* The addresses of the entries [a, b) of leaf col, appended to t at *n */
static void
gorder_addrs(SiftColSegment *seg, int col, uint32 a, uint32 b, SiftCGOrdBuf *memo, SiftKey *t, uint32 *n)
{
	uint32		crows = seg_gord_leaf(seg, col)->hdr.crows;

	for (uint32 c = a / crows; a < b && c <= (b - 1) / crows; c++)
	{
		uint32		from = Max(a, c * crows),
					to = Min(b, (c + 1) * crows);

		(void) gorder_chunk_memo(seg, col, c, memo);
		for (uint32 i = from; i < to; i++)
			t[(*n)++] = memo->addr[i - c * crows];
	}
}

/*
 * The heap addresses of the rows whose key lies in [lo, hi], from the global order of leaf col (ungrouped), ascending: the two bounds are sought in the directory and
 * one chunk each, and the chunks between are decoded. False when the segment has no such block or the range holds more than limit entries (the caller reads the units).
 */
bool
sift_cseg_gorder_tids(SiftColSegment *seg, int col, bool str, int64 lo, int64 hi, uint32 limit, SiftKey **tids, uint32 *ntids)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b,
				n = 0;
	SiftKey    *t;

	memo.at = UINT32_MAX;
	if (!gorder_span(seg, col, str, lo, hi, &a, &b, &memo) || b - a > limit)
		return false;
	t = palloc(Max(b - a, 1) * sizeof(SiftKey));
	gorder_addrs(seg, col, a, b, &memo, t, &n);
	if (n > 1)
		sift_sort_u64(t, n);
	*tids = t;
	*ntids = n;
	return true;
}

/* The same for the rows whose key is one of keys (distinct, ascending): a seek for each, the chunk of one key shared with the next when they meet in it. Not past limit entries in all. */
bool
sift_cseg_gorder_tids_keys(SiftColSegment *seg, int col, const int64 *keys, uint32 nkeys, uint32 limit, SiftKey **tids, uint32 *ntids)
{
	SiftCGOrdBuf memo;
	uint32		n = 0,
				cap = Min(limit, Max(nkeys, 16));
	SiftKey    *t = palloc(cap * sizeof(SiftKey));

	memo.at = UINT32_MAX;
	for (uint32 k = 0; k < nkeys; k++)
	{
		uint32		a,
					b;

		if (!gorder_span(seg, col, false, keys[k], keys[k], &a, &b, &memo) || b - a > limit - n)
		{
			pfree(t);
			return false;
		}
		if (n + (b - a) > cap)
		{
			cap = Min(limit, Max(2 * cap, n + (b - a)));
			t = repalloc(t, cap * sizeof(SiftKey));
		}
		gorder_addrs(seg, col, a, b, &memo, t, &n);
	}
	if (n > 1)
		sift_sort_u64(t, n);
	*tids = t;
	*ntids = n;
	return true;
}

/*
 * Calls fn with the heap address of each entry of the global order of leaf col (ungrouped) whose key lies in [lo, hi], in the order of the keys, a chunk at a time: the memory is
 * one chunk, whatever the range. False when the segment has no such block or the range holds more than limit entries (the caller reads the units).
 */
bool
sift_cseg_gorder_each(SiftColSegment *seg, int col, bool str, int64 lo, int64 hi, uint32 limit, void (*fn) (void *arg, SiftKey tid), void *arg)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b,
				crows;

	memo.at = UINT32_MAX;
	if (!gorder_span(seg, col, str, lo, hi, &a, &b, &memo) || b - a > limit)
		return false;
	crows = seg_gord_leaf(seg, col)->hdr.crows;
	for (uint32 c = a / crows; a < b && c <= (b - 1) / crows; c++)
	{
		uint32		from = Max(a, c * crows),
					to = Min(b, (c + 1) * crows);

		(void) gorder_chunk_memo(seg, col, c, &memo);
		for (uint32 i = from; i < to; i++)
			fn(arg, memo.addr[i - c * crows]);
	}
	return true;
}

/* The keys of chunk c of leaf col (the buckets of a compact one) into memo */
static void
gord_chunk_keys(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, SiftCGOrdBuf *memo)
{
	SiftCGOrdDir d = seg_gord_dirent(seg, g, col, c);
	uint32		at = c * g->hdr.crows;

	if (d.pad & SIFT_CGORD_COMPACT)
	{
		if (memo->at != at || !memo->compact || !memo->kmin_ok)
			compact_load(seg, g, col, c, &d, true, memo);
	}
	else if (memo->at != at || memo->compact || memo->cm < sift_cgord_chunk_rows(&g->hdr, c))
		gord_fill(seg, col, at, UINT32_MAX, false, memo);
}

/*
 * Of the entries [vs, ve) (value entries ascending by key), how many come first in a scan that reaches v (ascending: the keys below v; desc: those above it), counted from the
 * end the scan starts at: *clo of them certainly do (their bucket does), *chi may (the bucket of one more starts before v). A complete source answers exactly (clo == chi); the chunk
 * that holds v is left in memo. False when the segment has no complete block of the leaf.
 */
bool
sift_cseg_gorder_before(SiftColSegment *seg, int col, uint32 vs, uint32 ve, int64 v, bool desc, uint32 *clo, uint32 *chi, SiftCGOrdBuf *memo)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		crows,
				lo,
				hi,
				cs,
				base,
				m,
				pc,
				pp;

	if (g == NULL || g->hdr.gcol >= 0 || g->hdr.strbytes != 0 || g->hdr.vtotal != 0 || !gord_probe_ok(seg, g, col) || ve > g->hdr.values)
		return false;
	*clo = *chi = 0;
	if (vs >= ve)
		return true;
	crows = g->hdr.crows;
	lo = vs / crows + 1;
	hi = (ve - 1) / crows + 1;
	seg_gord_dir_read(seg, g);
	if (!desc)
	{
		SIFT_BISECT(uint32, mid, lo, hi, seg_gord_dirent(seg, g, col, mid).first < v);
		cs = lo - 1;
		if (seg_gord_dirent(seg, g, col, cs).first >= v)
			return true;
	}
	else
	{
		SIFT_BISECT(uint32, mid, lo, hi, seg_gord_dirent(seg, g, col, mid).first <= v);
		cs = lo - 1;
		if (seg_gord_dirent(seg, g, col, cs).first > v)
		{
			*clo = *chi = ve - vs;
			return true;
		}
	}
	gord_chunk_keys(seg, g, col, cs, memo);
	base = cs * crows;
	/* the NULL rows that end the last chunk of values are keyed 0 */
	m = Min(memo->rows, g->hdr.values - base);
	if (!memo->compact)
	{
		lo = 0;
		hi = m;
		SIFT_BISECT(uint32, mid, lo, hi, desc ? memo->key[mid] <= v : memo->key[mid] < v);
		pc = pp = base + lo;
	}
	else if (!desc)
	{
		uint32		i0,
					i1;

		compact_bounds(memo, m, v, false, &i0, &i1);
		pc = base + i0;
		pp = base + i1;
	}
	else
	{
		uint32		i0,
					i1;

		/* the entries certainly above v start at the first whose first key is, those that may be at the first whose last key is */
		compact_bounds(memo, m, v, true, &i0, &i1);
		pc = base + i1;
		pp = base + i0;
	}
	if (!desc)
	{
		*clo = Min(Max(pc, vs), ve) - vs;
		*chi = Min(Max(pp, vs), ve) - vs;
	}
	else
	{
		*clo = ve - Min(Max(pc, vs), ve);
		*chi = ve - Min(Max(pp, vs), ve);
	}
	return true;
}
