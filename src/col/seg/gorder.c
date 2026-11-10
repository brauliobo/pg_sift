/*
 * col/seg/gorder.c - the global order of a segment: the sorted keys of a column across its
 * units, the ranges and seeks a scan makes in it and the addresses a range lists
 */
#include "core/cmp.h"
#include "col/seg_int.h"
#include "col/gquant.h"

/* ---- the global order of a leaf (col/gorder.h) ---- */

/* The slot of the block of leaf col to read: the one sorted by group (the second slot, ncols + col; the first when it is grouped, as the user's order_array_group writes it) or the plain one; -1 when the segment has none */
int
sift_cseg_gorder_slot(SiftColSegment *seg, int col, bool grouped)
{
	GOrdLeaf   *g = grouped ? seg_gord_leaf(seg, seg->schema->ncols + col) : NULL;

	if (g != NULL)
		return seg_gord_probe_ok(seg, g, seg->schema->ncols + col) ? seg->schema->ncols + col : -1;
	g = seg_gord_leaf(seg, col);
	return g != NULL && (g->hdr.gcol >= 0) == grouped && seg_gord_probe_ok(seg, g, col) ? col : -1;
}

/* The host's reader of the heap rows the compact blocks take their exact keys from (col/seg/probe.c); unset on a host with no heap: such a block is not used and its scans read the units */
bool		(*sift_gprobe_ok) (SiftColSegment *seg, GOrdLeaf *g, int col) = NULL;
void	   *(*sift_gprobe_open) (SiftColSegment *seg, GOrdLeaf *g, int col) = NULL;
void		(*sift_gprobe_close) (void *probe) = NULL;
bool		(*sift_gprobe_key) (void *probe, uint64 addr, int64 *key) = NULL;

/* Whether the keys a compact block does not store can be read from the heap (asked once for a block); a block that cannot is not used */
bool
seg_gord_probe_ok(SiftColSegment *seg, GOrdLeaf *g, int col)
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
	pr->h = g->hdr.quant != 0 && seg_gord_probe_ok(seg, g, col) ? sift_gprobe_open(seg, g, col) : NULL;
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

/* The chunk c of a compact block into buf: its addresses and, with keys, the buckets of its entries; no exact key yet but the first entry of a group */
static void
compact_load(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, bool keys, SiftCGOrdBuf *buf)
{
	uint32		m = sift_cgord_chunk_rows(&g->hdr, c),
				nan = SIFT_CGORD_ANCHORS(d->pad);
	Size		front = ALIGN8((Size) d->klen) + ALIGN8((Size) d->alen);
	uint8	   *raw = palloc(front + SIFT_CGORD_ANCHOR_BYTES(nan) + SIFT_COL_PAD);
	bool		ok;

	seg_read(seg, g->at + d->off, front + SIFT_CGORD_ANCHOR_BYTES(nan), raw);
	ok = sift_cgord_decode_compact(seg->mcxt, raw, d->klen, raw + ALIGN8((Size) d->klen), d->alen, d->pad, m, d->first, m, keys ? buf->kmin : NULL, &buf->sh, buf->addr);
	memset(buf->exact, 0, sizeof(buf->exact));
	if (!ok || (keys && nan > 0 && !sift_cgord_group_bounds(buf->kmin, buf->key, buf->exact, m, d->first, raw + front, nan)))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	pfree(raw);
	buf->at = c * g->hdr.crows;
	buf->rows = buf->cm = m;
	buf->afrom = 0;
	buf->an = m;
	buf->compact = true;
	buf->kmin_ok = keys;
	buf->kfrom = buf->kn = 0;
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

	if (g == NULL || g->hdr.vtotal != 0 || !seg_gord_probe_ok(seg, g, col))
		return false;
	*entries = g->hdr.entries;
	*values = g->hdr.values;
	return true;
}

/* The block has compact chunks: the exact key of their entries is in the heap, a row apiece */
bool
sift_cseg_gorder_compact(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	return g != NULL && g->hdr.quant != 0;
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

#ifdef SIFT_TESTING
int64		sift_col_gorder_decoded;
#endif

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
	if (d.pad & SIFT_CGORD_POS)
		return seg_gord_pos_head(seg, g, col, c, &d, w, key, addr);
	buf = palloc(ALIGN8((Size) d.klen) + ALIGN8((Size) d.alen) + SIFT_COL_PAD);
	seg_read(seg, g->at + d.off, ALIGN8((Size) d.klen) + ALIGN8((Size) d.alen), buf);
	if (!sift_cgord_decode(seg->mcxt, buf, d.klen, buf + ALIGN8((Size) d.klen), d.alen, d.pad, m, d.first, w, key, addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	GORD_DECODED(Min(w, m));
	pfree(buf);
	return Min(w, m);
}

static void gord_window(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, uint32 p, uint32 w, SiftCGOrdBuf *buf);

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
	if (d.pad & (SIFT_CGORD_SEEK | SIFT_CGORD_POS))
	{
		seg_gord_seek_fill(seg, g, col, c, &d, p, w, buf);
		return;
	}
	if (w == UINT32_MAX)
	{
		buf->at = c * g->hdr.crows;
		buf->rows = sift_cgord_chunk_rows(&g->hdr, c);
		buf->cm = sift_cseg_gorder_head(seg, col, c, UINT32_MAX, buf->key, buf->addr);
		buf->afrom = 0;
		buf->an = buf->cm;
		buf->compact = false;
		buf->kfrom = 0;
		buf->kn = buf->cm;
		return;
	}
	gord_window(seg, g, col, c, &d, p, w, buf);
}

/*
 * The cursor's window of a chunk that is not compact: the w entries of the chunk the cursor reads next, from the entry p on (backwards: ending at it). The keys are a chain of deltas and come from the first
 * entry of the chunk to the end of the window, once; the addresses are only those of the window.
 */
static void
gord_window(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, uint32 p, uint32 w, SiftCGOrdBuf *buf)
{
	uint32		at = c * g->hdr.crows,
				m = sift_cgord_chunk_rows(&g->hdr, c),
				i0 = p - at,
				len = Max(w, 1),
				e = buf->desc ? i0 + 1 : Min(m, i0 + len),
				a0 = buf->desc ? (e > len ? e - len : 0) : i0;
	bool		same = buf->at == at && !buf->compact && buf->rows == m;
	Size		klen8 = ALIGN8((Size) d->klen);
	uint8	   *raw = palloc(klen8 + ALIGN8((Size) d->alen) + SIFT_COL_PAD);

	seg_read(seg, g->at + d->off, klen8 + ALIGN8((Size) d->alen), raw);
	if (!same || buf->cm < e)
	{
		sift_cgord_decode_keys(seg->mcxt, raw, d->klen, m, e, buf->key);
		GORD_DECODED(e);
		buf->cm = e;
	}
	if (!sift_cgord_decode_addrs(seg->mcxt, raw + klen8, d->alen, d->pad, m, buf->key, &a0, e, buf->addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	pfree(raw);
	buf->at = at;
	buf->rows = m;
	buf->afrom = a0;
	buf->an = e - a0;
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

static void gord_group_table(SiftColSegment *seg, int col);

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
	/*
	 * The groups are searched where they are for the first probe: a table of many groups is not read whole for one. A segment probed again (a scan that an enclosing query
	 * starts over for each of its rows) reads the table whole, once, and searches it in memory: the search was 12 pins of the buffers for each probe. The segment does not
	 * change while the source is open, so the copy cannot go stale.
	 */
	if (g->grps == NULL && ++g->gprobes > 1 && (uint64) g->hdr.ngroups * SIFT_CGORD_GRPENT <= SIFT_CGORD_GRP_EAGER)
		gord_group_table(seg, col);
	if (g->grps != NULL)
		for (int64 c = (int64) g->ghint + 1; c >= (int64) g->ghint - 1; c--)
			if (c >= 0 && c < hi && g->grps[c].group == group)
			{
				lo = (uint32) c;
				hi = lo;
				break;
			}
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		SiftCGOrdGrp e;

		if (g->grps != NULL)
			e = g->grps[mid];
		else
			seg_gord_grp_read(seg, g, col, mid, 1, &e);
		if (e.group < group)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < g->hdr.ngroups)
	{
		SiftCGOrdGrp e;

		if (g->grps != NULL)
			e = g->grps[lo];
		else
			seg_gord_grp_read(seg, g, col, lo, 1, &e);
		if (e.group == group)
		{
			if ((uint64) e.vstart + e.vcount > g->hdr.values || e.nstart < g->hdr.values || (uint64) e.nstart + e.ncount > g->hdr.entries)
				corrupt(seg, "The global order of leaf %d has a malformed group.", col);
			g->ghint = lo;
			*vs = e.vstart;
			*ve = e.vstart + e.vcount;
			*ns = e.nstart;
			*ne = e.nstart + e.ncount;
			return;
		}
	}
	*vs = *ve = *ns = *ne = 0;
}

/* The groups of a grouped block (an ungrouped one has one) */
uint32
sift_cseg_gorder_ngroups(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL)
		corrupt(seg, "The global order of leaf %d is not there.", col);
	return g->hdr.gcol < 0 ? 1 : g->hdr.ngroups;
}

/* The groups [i, i + n) of a block's table into out, packed or not */
void
seg_gord_grp_read(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 i, uint32 n, SiftCGOrdGrp *out)
{
	uint64		at = g->at + seg_gord_gat(seg, g, col),
				off;
	uint32		len;
	uint8		small[64],
			   *win;

	if (g->hdr.gbits == 0)
	{
		seg_read(seg, at + (uint64) i * SIFT_CGORD_GRPENT, (Size) n * SIFT_CGORD_GRPENT, out);
		return;
	}
	if (!g->gfirst_ok)
	{
		seg_read(seg, at, sizeof(int64), &g->gfirst);
		g->gfirst_ok = true;
	}
	sift_cgord_grp_span(&g->hdr, i, n, &off, &len);
	win = len <= sizeof(small) ? small : palloc(len);
	seg_read(seg, at + off, len, win);
	sift_cgord_grp_unpack(&g->hdr, g->gfirst, i, n, win, out);
	if (win != small)
		pfree(win);
}

/* The group table of a block in memory, for a table of at most SIFT_CGORD_GRP_EAGER bytes (the callers ask): a larger one is read entry by entry through the buffers, whatever its groups, and so is a packed one (decoding it whole costs more than the probes of a statement) */
static void
gord_group_table(SiftColSegment *seg, int col)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	if (g == NULL || g->hdr.gcol < 0 || g->grps != NULL || g->hdr.ngroups == 0 || g->hdr.gbits != 0)
		return;
	Assert((uint64) g->hdr.ngroups * SIFT_CGORD_GRPENT <= SIFT_CGORD_GRP_EAGER);
	g->grps = MemoryContextAlloc(seg->mcxt, (Size) g->hdr.ngroups * SIFT_CGORD_GRPENT);
	seg_gord_grp_read(seg, g, col, 0, g->hdr.ngroups, g->grps);
}

/* The group at place gi in the order of the groups of a grouped block, and its entries as sift_cseg_gorder_range gives them */
void
sift_cseg_gorder_group_at(SiftColSegment *seg, int col, uint32 gi, int64 *group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	SiftCGOrdGrp e;

	if (g == NULL || g->hdr.gcol < 0 || gi >= g->hdr.ngroups)
		corrupt(seg, "The global order of leaf %d has no group %u.", col, gi);
	if ((uint64) g->hdr.ngroups * SIFT_CGORD_GRPENT <= SIFT_CGORD_GRP_EAGER)
		gord_group_table(seg, col);
	if (g->grps != NULL)
		e = g->grps[gi];
	else
		seg_gord_grp_read(seg, g, col, gi, 1, &e);
	if ((uint64) e.vstart + e.vcount > g->hdr.values || e.nstart < g->hdr.values || (uint64) e.nstart + e.ncount > g->hdr.entries)
		corrupt(seg, "The global order of leaf %d has a malformed group.", col);
	*group = e.group;
	*vs = e.vstart;
	*ve = e.vstart + e.vcount;
	*ns = e.nstart;
	*ne = e.nstart + e.ncount;
}

uint32
seg_gord_chunk_memo(SiftColSegment *seg, int col, uint32 c, SiftCGOrdBuf *memo)
{
	uint32		at = c * seg_gord_leaf(seg, col)->hdr.crows;

	/* a chunk left with its keys only (a seek) gets its addresses */
	if (memo->at != at || (!memo->compact && (memo->afrom != 0 || memo->an < memo->rows)))
		gord_fill(seg, col, at, UINT32_MAX, false, memo);
	return memo->cm;
}

/* The keys of chunk c of leaf col (not compact) into memo, all of them and none of the addresses: a seek needs the keys, and a cursor that reads on from it asks for the addresses of its window */
static uint32
gord_keys(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, SiftCGOrdBuf *memo)
{
	uint32		at = c * g->hdr.crows,
				m = sift_cgord_chunk_rows(&g->hdr, c);

	if (memo->at != at || memo->compact || memo->kfrom != 0 || memo->kn < m)
	{
		SiftCGOrdDir d = seg_gord_dirent(seg, g, col, c);
		Size		klen8 = ALIGN8((Size) d.klen);
		uint8	   *raw = palloc(klen8 + SIFT_COL_PAD);

		seg_read(seg, g->at + d.off, klen8, raw);
		sift_cgord_decode_keys(seg->mcxt, raw, d.klen, m, m, memo->key);
		GORD_DECODED(m);
		pfree(raw);
		memo->at = at;
		memo->rows = memo->cm = m;
		memo->afrom = memo->an = 0;
		memo->compact = false;
		memo->kfrom = 0;
		memo->kn = m;
	}
	return memo->cm;
}

/*
 * The first entry of [from, to) in the compact chunk c whose key is not before key (after: is after it); to when there is none. The buckets say which entries are certainly before
 * key (their last key is) and which certainly are not (their first is): the entries between, in the bucket that holds key, are searched by the keys of their heap rows (0.7 of them a seek
 * for keys 4 buckets to a mean gap). The chunk is left in memo with its addresses.
 */
/*
 * The first entry i0 of [from, to) in the compact chunk in buf that is not certainly before key (its last key is not: before(k) is k < key, or k <= key for after) and the first i1 that
 * certainly is not (its first key is not before). The keys ascend in the range only: a chunk of a grouped block has the keys of each group from the start.
 */
static void
compact_bounds(const SiftCGOrdBuf *buf, uint32 from, uint32 to, int64 key, bool after, uint32 *i0, uint32 *i1)
{
	uint32		lo = from,
				hi = to;

	SIFT_BISECT(uint32, mid, lo, hi, after ? (mid == 0 ? buf->kmin[0] : sift_gq_kmax(buf->kmin[mid], buf->sh)) <= key : (mid == 0 ? buf->kmin[0] : sift_gq_kmax(buf->kmin[mid], buf->sh)) < key);
	*i0 = lo;
	hi = to;
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
	compact_bounds(memo, from - at, to - at, key, after, &i0, &i1);
	lo = i0;
	hi = i1;
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
uint32
seg_gord_seek(SiftColSegment *seg, int col, uint32 vs, uint32 ve, int64 key, bool after, SiftCGOrdBuf *memo)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		c0,
				lo,
				hi,
				m,
				from,
				to;
	uint32		crows;
	SiftCGOrdDir d;

	if (g == NULL)
		corrupt(seg, "The global order of leaf %d is not there.", col);
	crows = g->hdr.crows;
	if (vs >= ve)
		return ve;
	/* the chunks after the first one of the range start inside it: the last of them whose first key is before key (or equal to it, for an upper bound) holds the answer or ends before it */
	c0 = vs / crows;
	lo = c0 + 1;
	hi = (ve - 1) / crows + 1;
	seg_gord_dir_read(seg, g, col);
	SIFT_BISECT(uint32, mid, lo, hi, after ? seg_gord_dir_first(g, mid) <= key : seg_gord_dir_first(g, mid) < key);
	lo--;
	d = seg_gord_dirent(seg, g, col, lo);
	if (d.pad & SIFT_CGORD_COMPACT)
		return compact_seek(seg, g, col, lo, Max(vs, lo * crows), Min(ve, (lo + 1) * crows), key, after, memo);
	if (d.pad & (SIFT_CGORD_SEEK | SIFT_CGORD_POS))
		return seg_gord_seek_seek(seg, g, col, lo, &d, Max(vs, lo * crows), Min(ve, (lo + 1) * crows), key, after, memo);
	m = gord_keys(seg, g, col, lo, memo);
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
	return seg_gord_seek(seg, col, vs, ve, key, after, keep != NULL ? keep : &memo);
}

/*
 * The entries [*a, *b) of the global order of leaf col (ungrouped) whose key lies in [lo, hi]; false when the segment has no such block. The keys of a text block are
 * ranks of the segment's strings (str: those of sift_cseg_gorder_strkeys), not the codes of its dictionary that a range of the scan is in.
 */
static bool
gorder_span(SiftColSegment *seg, int col, bool str, bool group, int64 lo, int64 hi, uint32 *a, uint32 *b, SiftCGOrdBuf *memo)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);

	/* group: the block is grouped and lo is a group, the span the entries of its rows (a group with NULL rows is two spans, not counted) */
	if (group)
	{
		uint32		ns,
					ne;

		if (g == NULL || g->hdr.gcol < 0 || str || g->hdr.vtotal != 0)
			return false;
		sift_cseg_gorder_range(seg, col, lo, a, b, &ns, &ne);
		return ns == ne;
	}
	if (g == NULL || g->hdr.gcol >= 0 || (g->hdr.strbytes != 0) != str || g->hdr.vtotal != 0 || !seg_gord_probe_ok(seg, g, col))
		return false;
	*a = lo > hi ? 0 : seg_gord_seek(seg, col, 0, g->hdr.values, lo, false, memo);
	*b = lo > hi ? 0 : seg_gord_seek(seg, col, *a, g->hdr.values, hi, true, memo);
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

/* The slot of a grouped block whose group is the leaf col (for a count of its rows: sift_cseg_gorder_count with that slot and the group as lo and hi); -1 when the segment has none */
int
sift_cseg_gorder_group_slot(SiftColSegment *seg, int col)
{
	for (int s = 0; s < 2 * seg->schema->ncols; s++)
	{
		GOrdLeaf   *g = seg_gord_leaf(seg, s);

		if (g != NULL && g->hdr.gcol == col && g->hdr.vtotal == 0)
			return s;
	}
	return -1;
}

/* How many rows of the segment have a key in [lo, hi], by the global order of the leaf; UINT32_MAX when it has none */
uint32
sift_cseg_gorder_count(SiftColSegment *seg, int col, bool str, bool group, int64 lo, int64 hi)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b;

	memo.at = UINT32_MAX;
	return gorder_span(seg, col, str, group, lo, hi, &a, &b, &memo) ? b - a : UINT32_MAX;
}

/* The addresses of the entries [a, b) of leaf col, appended to t at *n */
void
seg_gord_addrs(SiftColSegment *seg, int col, uint32 a, uint32 b, SiftCGOrdBuf *memo, SiftKey *t, uint32 *n)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	uint32		crows = g->hdr.crows;

	for (uint32 c = a / crows; a < b && c <= (b - 1) / crows; c++)
	{
		SiftCGOrdDir d = seg_gord_dirent(seg, g, col, c);
		uint32		from = Max(a, c * crows),
					to = Min(b, (c + 1) * crows);

		if (!seg_gord_plain_addrs(seg, g, col, c, &d, memo, from - c * crows, to - c * crows))
			(void) seg_gord_chunk_memo(seg, col, c, memo);
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
	if (!gorder_span(seg, col, str, false, lo, hi, &a, &b, &memo) || b - a > limit)
		return false;
	t = palloc(Max(b - a, 1) * sizeof(SiftKey));
	seg_gord_addrs(seg, col, a, b, &memo, t, &n);
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

		if (!gorder_span(seg, col, false, false, keys[k], keys[k], &a, &b, &memo) || b - a > limit - n)
		{
			pfree(t);
			return false;
		}
		if (n + (b - a) > cap)
		{
			cap = Min(limit, Max(2 * cap, n + (b - a)));
			t = repalloc(t, cap * sizeof(SiftKey));
		}
		seg_gord_addrs(seg, col, a, b, &memo, t, &n);
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
sift_cseg_gorder_each(SiftColSegment *seg, int col, bool str, bool group, int64 lo, int64 hi, uint32 limit, void (*fn) (void *arg, SiftKey tid), void *arg)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b,
				crows;

	memo.at = UINT32_MAX;
	if (!gorder_span(seg, col, str, group, lo, hi, &a, &b, &memo) || b - a > limit)
		return false;
	crows = seg_gord_leaf(seg, col)->hdr.crows;
	for (uint32 c = a / crows; a < b && c <= (b - 1) / crows; c++)
	{
		uint32		from = Max(a, c * crows),
					to = Min(b, (c + 1) * crows);

		(void) seg_gord_chunk_memo(seg, col, c, &memo);
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
	else
		(void) gord_keys(seg, g, col, c, memo);
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

	if (g == NULL || g->hdr.gcol >= 0 || g->hdr.strbytes != 0 || g->hdr.vtotal != 0 || !seg_gord_probe_ok(seg, g, col) || ve > g->hdr.values)
		return false;
	*clo = *chi = 0;
	if (vs >= ve)
		return true;
	crows = g->hdr.crows;
	lo = vs / crows + 1;
	hi = (ve - 1) / crows + 1;
	seg_gord_dir_read(seg, g, col);
	if (!desc)
	{
		SIFT_BISECT(uint32, mid, lo, hi, seg_gord_dir_first(g, mid) < v);
		cs = lo - 1;
		if (seg_gord_dirent(seg, g, col, cs).first >= v)
			return true;
	}
	else
	{
		SIFT_BISECT(uint32, mid, lo, hi, seg_gord_dir_first(g, mid) <= v);
		cs = lo - 1;
		if (seg_gord_dirent(seg, g, col, cs).first > v)
		{
			*clo = *chi = ve - vs;
			return true;
		}
	}
	base = cs * crows;
	if (seg_gord_dirent(seg, g, col, cs).pad & SIFT_CGORD_SEEK)
	{
		pc = pp = base + seg_gord_seek_before(seg, g, col, cs, Min(sift_cgord_chunk_rows(&g->hdr, cs), g->hdr.values - base), v, desc, memo);
		goto counted;
	}
	gord_chunk_keys(seg, g, col, cs, memo);
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

		compact_bounds(memo, 0, m, v, false, &i0, &i1);
		pc = base + i0;
		pp = base + i1;
	}
	else
	{
		uint32		i0,
					i1;

		/* the entries certainly above v start at the first whose first key is, those that may be at the first whose last key is */
		compact_bounds(memo, 0, m, v, true, &i0, &i1);
		pc = base + i1;
		pp = base + i0;
	}
counted:
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
