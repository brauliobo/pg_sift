/*
 * col/scan/gorder.c - the global order of a scan: the array of a column in key order
 * built from the stored arrays and from the write buffer
 * the reads of its strings, ranges and heads
 */
#include "postgres.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "col/point.h"
#include "col/wbuf.h"
#include "core/cmp.h"
#include "col/scan_int.h"

/* ---- the global order of a leaf as a source of sorted entries (col/gorder.h): the stored segments and the rows of the write buffers ---- */

#include "col/scan/gmem.h"


#define GORD_PAIR_LESS(x, y) ((x)->key != (y)->key ? (x)->key < (y)->key : (x)->tid < (y)->tid)
SIFT_DEFINE_SORT(gord_pair_cmp_sort, GOrdPair, GORD_PAIR_LESS)

/* a copy of the entries for the radix sort: none when it would pass the largest allocation (the comparison sort then) */
static void *
gord_pair_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

#define GORD_PAIR_KEY(x) ((x)->key)
#define GORD_PAIR_NOKEY(x) ((int64) 0)
#define GORD_PAIR_TID(x) ((x)->tid)
/* the rows of the buffers come in the order of their addresses (the comparison sort takes any other order): by key, then address */
SIFT_DEFINE_RADIX_SORT(gord_pair_sort, GOrdPair, GORD_PAIR_KEY, GORD_PAIR_NOKEY, GORD_PAIR_TID, gord_pair_copy, pfree, gord_pair_cmp_sort)

void
gord_pairs_sort(GOrdPair *a, uint32 n)
{
	gord_pair_sort(a, n);
}

/* the first 8 bytes of a string, as the signed key that orders them like the bytes: strings with the same 8 bytes are ordered by the collation within a range */
static int64
gord_prefix(const char *s, uint32 len)
{
	uint64		p = 0;

	if (len >= 8)
	{
		memcpy(&p, s, 8);
		p = pg_bswap64(p);
	}
	else
		for (uint32 i = 0; i < len; i++)
			p |= (uint64) (uint8) s[i] << (56 - 8 * i);
	return (int64) (p ^ UINT64CONST(0x8000000000000000));
}

static const GOrdText *gord_text_ent;
static uint32 gord_text_coll;

/* a pair of a text range: the prefix, then the string of the row it names (tid is its place among the rows read), then its address */
static inline bool
gord_text_before(const GOrdPair *x, const GOrdPair *y)
{
	const GOrdText *a = &gord_text_ent[x->tid],
			   *b = &gord_text_ent[y->tid];
	int			c;

	if (x->key != y->key)
		return x->key < y->key;
	c = sift_cgord_str_order(a->s, a->len, b->s, b->len, gord_text_coll);
	return c != 0 ? c < 0 : a->tid < b->tid;
}

#define GORD_TEXT_LESS(x, y) gord_text_before(x, y)
SIFT_DEFINE_SORT(gord_text_sort, GOrdPair, GORD_TEXT_LESS)

/* The ranges of the key of the buffer sorted so far: what a scan reads is a few ranges of them, and a seek or a count by key sorts the one the key falls in */
static void
gmem_sort(ColGOrdMem *g, uint32 b)
{
	uint32		from = g->bstart[b],
				to = g->bstart[b + 1];

	if (g->bdone[b])
		return;
	if (g->ent == NULL)
	{
		gord_pair_sort(g->raw + from, to - from);
		for (uint32 i = from; i < to; i++)
		{
			g->key[i] = g->raw[i].key;
			g->tid[i] = g->raw[i].tid;
		}
	}
	else
	{
		uint32		run = from;

		gord_text_ent = g->ent;
		gord_text_coll = g->collid;
		gord_text_sort(g->raw + from, to - from);
		for (uint32 i = from; i < to; i++)
		{
			const GOrdText *e = &g->ent[g->raw[i].tid];

			/* the order of a deterministic collation ends a tie by the bytes: equal strings are the same bytes */
			if (i > from && (e->len != g->len[i - 1] || memcmp(e->s, g->str[i - 1], e->len) != 0))
				run = i;
			g->key[i] = run;
			g->tid[i] = e->tid;
			g->str[i] = e->s;
			g->len[i] = e->len;
		}
	}
	g->bdone[b] = true;
}

/* The entries [from, to) with a value are in order */
static void
gmem_sort_range(ColGOrdMem *g, uint32 from, uint32 to)
{
	uint32		lo = 0,
				hi = g->nb;

	if (from >= to || g->nb == 0)
		return;
	SIFT_BISECT(uint32, mid, lo, hi, g->bstart[mid + 1] <= from);
	for (uint32 b = lo; b < g->nb && g->bstart[b] < to; b++)
		gmem_sort(g, b);
}

/* The entries with a value whose key is before key (after: up to it): the position a bisection of the sorted array gives */
static uint32
gmem_rank(ColGOrdMem *g, int64 key, bool after)
{
	uint64		d;
	uint32		b,
				lo,
				hi;

	if (key < g->kmin)
		return 0;
	d = ((uint64) key - (uint64) g->kmin) >> g->shift;
	if (g->nb == 0 || d >= g->nb)
		return g->nvals;
	b = (uint32) d;
	gmem_sort(g, b);
	lo = g->bstart[b];
	hi = g->bstart[b + 1];
	SIFT_BISECT(uint32, mid, lo, hi, after ? g->key[mid] <= key : g->key[mid] < key);
	return lo;
}

/* The entries with a value cut into ranges of the key of the same width, as many as 1 / 64 of them (4,096 at most): one pass counts them, one puts them in the order of the ranges. pv is freed */
void
gmem_ranges(ColGOrdMem *g, MemoryContext mcxt, GOrdPair *pv, uint32 nv)
{
	int64		kmax;
	uint64		range;
	int			bits = 0,
				lg = 0;
	uint32	   *cnt;

	g->kmin = kmax = nv > 0 ? pv[0].key : 0;
	for (uint32 i = 1; i < nv; i++)
	{
		g->kmin = Min(g->kmin, pv[i].key);
		kmax = Max(kmax, pv[i].key);
	}
	range = (uint64) kmax - (uint64) g->kmin;
	while (bits < 64 && (range >> bits) != 0)
		bits++;
	while (lg < 12 && (nv >> (lg + 6)) != 0)
		lg++;
	g->shift = Min(bits > lg ? bits - lg : 0, 63);
	g->nb = nv == 0 ? 0 : (uint32) (range >> g->shift) + 1;
	g->bstart = MemoryContextAllocZero(mcxt, ((Size) g->nb + 2) * sizeof(uint32));
	g->bdone = MemoryContextAllocZero(mcxt, ((Size) g->nb + 1) * sizeof(bool));
	g->raw = MemoryContextAlloc(mcxt, ((Size) nv + 1) * sizeof(GOrdPair));
	cnt = g->bstart + 1;
	for (uint32 i = 0; i < nv; i++)
		cnt[((uint64) pv[i].key - (uint64) g->kmin) >> g->shift]++;
	for (uint32 b = 0; b < g->nb; b++)
		cnt[b] += g->bstart[b];
	/* cnt[b] is now the end of range b, which is bstart[b + 1]; the entries go in at the start of theirs, which moves to the end */
	for (uint32 i = 0; i < nv; i++)
		g->raw[g->bstart[((uint64) pv[i].key - (uint64) g->kmin) >> g->shift]++] = pv[i];
	for (uint32 b = g->nb; b > 0; b--)
		g->bstart[b] = g->bstart[b - 1];
	g->bstart[0] = 0;
	for (uint32 b = 0; b < g->nb; b++)
		if (g->bstart[b + 1] - g->bstart[b] <= 1)
			gmem_sort(g, b);
	pfree(pv);
}

/*
 * The rows of the write buffers of a text leaf sorted like the entries of a text block: by string (the collation, then the bytes) and address; false as gord_mem, or a string longer than
 * a dictionary entry. Under a collation that orders by bytes the rows are cut into ranges by their first 8 bytes and a range is sorted when it is read, as for a number; under another
 * the whole is one range, sorted at the first read.
 */
bool
gord_mem_text(SiftColSource *src, int leaf, uint32 collid, int v, MemoryContext mc)
{
	bool		bytes = sift_collate_is_c(collid);
	uint64		n = gord_mem_rows(src);
	GOrdText   *ent;
	GOrdPair   *pv;
	uint64	   *pn;
	uint32		nv = 0,
				nn = 0,
				plen = 0;
	ColGOrdMem *g;

	ent = MemoryContextAlloc(mc, (n + 1) * sizeof(GOrdText));
	pv = palloc((n + 1) * sizeof(GOrdPair));
	pn = palloc((n + 1) * sizeof(uint64));
	for (int i = 0; i < src->nmem; i++)
	{
		const SiftColBatch *b = src->mem[i].b;
		const SiftCVals *v = &b->col[leaf];

		for (uint32 r = 0; r < b->nrows; r++)
		{
			if ((v->ovf && (v->ovf[r / 64] >> (r % 64) & 1)) || (b->keys[r] & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF)
				return false;
			if (v->nulls && (v->nulls[r / 64] >> (r % 64) & 1))
				pn[nn++] = (uint64) b->keys[r];
			else
			{
				uint32		len;
				const char *str = sift_cdict_str(b->dict[leaf], (uint32) v->key[r], &len);

				if (len > SIFT_COL_DICT_MAX_ENTRY)
					return false;
				ent[nv++] = (GOrdText) {str, len, (uint64) b->keys[r]};
			}
		}
	}
	/* strings that begin alike ('customer-...') are told apart by the 8 bytes after what they share */
	if (bytes && nv > 0)
	{
		plen = ent[0].len;
		for (uint32 i = 1, k; i < nv && plen > 0; i++)
		{
			for (k = 0; k < plen && k < ent[i].len && ent[i].s[k] == ent[0].s[k]; k++)
				;
			plen = k;
		}
	}
	for (uint32 i = 0; i < nv; i++)
		pv[i] = (GOrdPair) {bytes ? gord_prefix(ent[i].s + plen, ent[i].len - plen) : 0, i};
	sift_sort_u64(pn, nn);
	g = MemoryContextAllocZero(mc, sizeof(ColGOrdMem));
	g->leaf = leaf;
	g->gleaf = -1;
	g->plen = plen;
	g->pre = nv > 0 ? ent[0].s : NULL;
	g->n = nv + nn;
	g->nvals = nv;
	g->key = MemoryContextAlloc(mc, (Size) (g->n + 1) * sizeof(int64));
	g->tid = MemoryContextAlloc(mc, (Size) (g->n + 1) * sizeof(uint64));
	g->str = MemoryContextAlloc(mc, ((Size) nv + 1) * sizeof(char *));
	g->len = MemoryContextAlloc(mc, ((Size) nv + 1) * sizeof(uint32));
	g->ent = ent;
	g->bytes = bytes;
	g->collid = collid;
	gmem_ranges(g, mc, pv, nv);
	for (uint32 i = 0; i < nn; i++)
	{
		g->key[nv + i] = 0;
		g->tid[nv + i] = pn[i];
	}
	src->gmem[v] = g;
	pfree(pn);
	return true;
}

/* The source i of a scan that reads the global order: the value of the group it reads (v) and its place among the sources of that value (j: a segment, or the write buffers after them) */
static inline int
gorder_src(const SiftColSource *src, int i, int *v)
{
	*v = i / src->gper;
	return i % src->gper;
}

/* The slot of the block of leaf col that segment j is read through: the one sorted by group when the scan pinned a group, else the plain one */
static int
gorder_slot(SiftColSource *src, int j, int col)
{
	if (src->galeaf1 > 0)
		return sift_cseg_gorder_slot(src->seg[j].seg, src->galeaf1 - 1, true);
	return src->gpin_leaf >= 0 ? sift_cseg_gorder_slot(src->seg[j].seg, col, true) : col;
}

/*
 * A segment that keeps the ends of the order only (column format minor 40): the entries [0, head) are its first values and [head, values) its last, with nothing
 * between, whose keys are after the first end's and before the last one's. A scan reads the end it starts at and stops at its edge; this finds the end, which is
 * the start of the scan's range in the order it reads: false when that falls in what the block left out, or the scan wants more rows than an end holds.
 */
static bool
gorder_view(ColCursor *cur, int col, const SiftCGOrdWant *want)
{
	uint32		entries,
				values,
				head,
				vtotal,
				nulls,
				at;
	SiftColSegment *seg = cur->seg;

	cur->gends = sift_cseg_gorder_ends(seg, col, &entries, &values, &head, &vtotal);
	if (!cur->gends)
		return true;
	if (want == NULL || want->rows > Max(head, values - head))
		return false;
	cur->gcuts = (SiftCGOrdCuts) {0};
	if (!want->desc)
	{
		at = want->has_lo ? sift_cseg_gorder_seek(seg, col, 0, values, want->lo, false, NULL) : 0;
		if (at == head)
			return false;
		cur->gwin[0] = at < head ? 0 : head;
		cur->gwin[1] = at < head ? head : values;
		cur->gcuts.hi_cut = at < head;
	}
	else
	{
		at = want->has_hi ? sift_cseg_gorder_seek(seg, col, 0, values, want->hi, true, NULL) : values;
		if (at == head)
			return false;
		cur->gwin[0] = at < head ? 0 : head;
		cur->gwin[1] = at < head ? head : values;
		cur->gcuts.lo_cut = at > head;
	}
	cur->gcuts.lo = cur->gwin[0];
	cur->gcuts.hi = cur->gwin[1];
	nulls = (uint32) seg->entry.ndocs - vtotal;
	cur->gwin[2] = values;
	cur->gwin[3] = want->has_lo || want->has_hi ? values : entries;
	cur->gcuts.null_cut = !(want->has_lo || want->has_hi) && entries - values < nulls;
	return true;
}

/*
 * Every stored segment keeps the block of leaf col sorted by group (or plain), and for the first the one group leaf that a pin names; *gleaf is that leaf and vals its pinned
 * values (the pins on it, once each: an IN list pins a leaf more than once), *nvals their number
 */
static bool
gorder_all(SiftColSource *src, int col, bool grouped, int npin, const int *pinleaf, const int64 *pinval, int *gleaf, int64 *vals, int *nvals, uint32 collid)
{
	*gleaf = -1;
	*nvals = 0;
	for (int i = 0; i < src->nseg; i++)
	{
		uint32		entries,
					values;
		int			slot = sift_cseg_gorder_slot(src->seg[i].seg, col, grouped),
					gc;

		uint32		head,
					vtotal;

		if (slot < 0 || !(sift_cseg_gorder(src->seg[i].seg, slot, &entries, &values) || (!grouped && sift_cseg_gorder_ends(src->seg[i].seg, slot, &entries, &values, &head, &vtotal))) ||
			sift_cseg_gorder_collation(src->seg[i].seg, slot) != collid)
			return false;
		gc = sift_cseg_gorder_group(src->seg[i].seg, slot);
		if (gc >= 0)
		{
			if (*gleaf >= 0 && *gleaf != gc)
				return false;
			if (*gleaf < 0)
			{
				*gleaf = gc;
				for (int k = 0; k < npin; k++)
				{
					int			m = 0;

					if (pinleaf[k] != gc)
						continue;
					while (m < *nvals && vals[m] != pinval[k])
						m++;
					if (m == *nvals)
						vals[(*nvals)++] = pinval[k];
				}
			}
			if (*nvals == 0)
				return false;
		}
	}
	return !grouped || (src->nseg > 0 && *gleaf >= 0);
}

/*
 * A leaf with no order of its own, that every stored segment has as the group of the second order of one leaf (the same in all): that second order lists the rows group after
 * group, in the order of the groups, and the rows of every group (a row with no group is not in the block, so the block must hold every row). -1 when it is not so.
 */
static int
gorder_by_group(SiftColSource *src, int col)
{
	int			found = -1;

	if (src->nseg == 0)
		return -1;
	for (int a = 0; a < src->schema->ncols && found < 0; a++)
		if (a != col && sift_cseg_gorder_grouped_by(src->seg[0].seg, a, col))
			found = a;
	for (int i = 0; i < src->nseg && found >= 0; i++)
	{
		int			slot = sift_cseg_gorder_slot(src->seg[i].seg, found, true);
		uint32		entries,
					values;

		if (slot < 0 || !sift_cseg_gorder_grouped_by(src->seg[i].seg, found, col) || !sift_cseg_gorder(src->seg[i].seg, slot, &entries, &values) ||
			entries != (uint32) src->seg[i].seg->entry.ndocs)
			found = -1;
	}
	return found;
}

/*
 * The sources of the global order of leaf col: one for each stored segment, then one for the write buffers when they hold rows. -1 when the order cannot
 * answer (a segment lacks it, the source reads a sift_table, a buffer holds an overflow value, a block grouped by a leaf that none of the npin pins (leaf pinleaf[i]
 * = pinval[i], conditions of the query) names). A scan that pins the group of the blocks sorted by group reads the entries of that group only: the sources are those of that
 * group (those of each value of an IN list, one value after the other); any other reads the plain blocks.
 */
int
sift_col_gorder_open(SiftColSource *src, int col, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid, const SiftCGOrdWant *want)
{
	int			gleaf = -1,
				nv = 0;
	int64		vals[SIFT_GORD_PIN_MAX];

	src->galeaf1 = 0;
	if ((src->flags & (COLO_TABLE | SIFT_COLO_NOWBUF)) || src->schema->cols[col].kind != SIFT_COLK_SCALAR)
		return -1;
	if (!(npin > 0 && gorder_all(src, col, true, npin, pinleaf, pinval, &gleaf, vals, &nv, collid)) && !gorder_all(src, col, false, npin, pinleaf, pinval, &gleaf, vals, &nv, collid))
	{
		int			a = npin == 0 && collid == 0 && want != NULL && want->bygroup && !(want->has_lo || want->has_hi) ? gorder_by_group(src, col) : -1;

		if (a < 0)
			return -1;
		src->galeaf1 = a + 1;
		nv = 0;
		gleaf = -1;
	}
	src->gpin_leaf = gleaf;
	src->gpin_n = Max(nv, 1);
	memcpy(src->gpin_vals, vals, nv * sizeof(int64));
	*grouped = gleaf;
	for (int i = 0; i < src->nseg; i++)
	{
		if (!gorder_view(&src->seg[i], gorder_slot(src, i, col), want))
			return -1;
	}
	if (!src->mem_loaded)
		scan_load_wbuf(src);
	src->gper = src->nseg + (src->nmem > 0);
	if (src->nmem > 0)
		for (int v = 0; v < src->gpin_n; v++)
			if (!gord_mem(src, col, Max(SIFT_CGORD_MEM_ROWS, 8 * (uint64) rows), gleaf, src->gpin_vals[v], v, collid))
				return -1;
	return src->gper * src->gpin_n;
}

const char *
sift_col_gorder_str(SiftColSource *src, int i, int col, int64 key, uint32 *len)
{
	int			v,
				j = gorder_src(src, i, &v);

	if (j < src->nseg)
		return sift_cseg_gorder_str(src->seg[j].seg, gorder_slot(src, j, col), (uint32) key, len);
	*len = src->gmem[v]->len[key];
	return src->gmem[v]->str[key];
}

uint32
sift_col_gorder_strrank(SiftColSource *src, int i, int col, const char *s, uint32 len, bool after)
{
	int			v,
				j = gorder_src(src, i, &v);
	ColGOrdMem *g = src->gmem[v];
	int64		pk;
	uint64		d;
	uint32		lo,
				hi;
	int			c;

	if (j < src->nseg)
		return sift_cseg_gorder_strrank(src->seg[j].seg, gorder_slot(src, j, col), s, len, after);
	/* a string that does not begin as they all do is before them all or after them all */
	c = g->plen > 0 ? memcmp(s, g->pre, Min(len, g->plen)) : 0;
	if (c != 0)
		return c < 0 ? 0 : g->nvals;
	if (len < g->plen)
		return 0;
	pk = g->bytes ? gord_prefix(s + g->plen, len - g->plen) : 0;
	/* the entries of the ranges before the one of the string come first, those of the ranges after it last */
	if (pk < g->kmin)
		return 0;
	d = ((uint64) pk - (uint64) g->kmin) >> g->shift;
	if (g->nb == 0 || d >= g->nb)
		return g->nvals;
	gmem_sort(g, (uint32) d);
	lo = g->bstart[d];
	hi = g->bstart[d + 1];
	SIFT_BISECT(uint32, mid, lo, hi, after ? sift_cgord_str_order(g->str[mid], g->len[mid], s, len, g->collid) <= 0 : sift_cgord_str_order(g->str[mid], g->len[mid], s, len, g->collid) < 0);
	return lo;
}

bool
sift_col_gorder_cuts(SiftColSource *src, int i, int col, SiftCGOrdCuts *cuts)
{
	int			v,
				j = gorder_src(src, i, &v);

	if (j >= src->nseg || !src->seg[j].gends)
		return false;
	*cuts = src->seg[j].gcuts;
	return true;
}

bool
sift_col_gorder_compact(SiftColSource *src, int i, int col)
{
	int			v,
				j = gorder_src(src, i, &v);

	return j < src->nseg && sift_cseg_gorder_compact(src->seg[j].seg, gorder_slot(src, j, col));
}

bool
sift_col_gorder_info(SiftColSource *src, int i, int col, uint32 *entries, uint32 *values)
{
	int			v,
				j = gorder_src(src, i, &v);

	if (j < src->nseg && src->seg[j].gends)
	{
		uint32		head,
					vtotal;

		return sift_cseg_gorder_ends(src->seg[j].seg, gorder_slot(src, j, col), entries, values, &head, &vtotal);
	}
	if (j < src->nseg)
		return sift_cseg_gorder(src->seg[j].seg, gorder_slot(src, j, col), entries, values);
	*entries = src->gmem[v]->n;
	*values = src->gmem[v]->nvals;
	return true;
}

void
sift_col_gorder_range(SiftColSource *src, int i, int col, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	int			v,
				j = gorder_src(src, i, &v);

	if (j < src->nseg && src->seg[j].gends)
	{
		*vs = src->seg[j].gwin[0];
		*ve = src->seg[j].gwin[1];
		*ns = src->seg[j].gwin[2];
		*ne = src->seg[j].gwin[3];
	}
	else if (j < src->nseg)
		sift_cseg_gorder_range(src->seg[j].seg, gorder_slot(src, j, col), src->gpin_vals[v], vs, ve, ns, ne);
	else
	{
		*vs = 0;
		*ve = *ns = src->gmem[v]->nvals;
		*ne = src->gmem[v]->n;
	}
}

/* Is source i read group after group (its entries are those of the second order of another leaf, whose key here is the group), and how many groups it has */
bool
sift_col_gorder_groups(SiftColSource *src, int i, int col, uint32 *ngroups)
{
	int			v,
				j = gorder_src(src, i, &v);

	if (src->galeaf1 == 0 || j >= src->nseg)
		return false;
	*ngroups = sift_cseg_gorder_ngroups(src->seg[j].seg, gorder_slot(src, j, col));
	return true;
}

void
sift_col_gorder_group_at(SiftColSource *src, int i, int col, uint32 gi, int64 *group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	int			v,
				j = gorder_src(src, i, &v);

	sift_cseg_gorder_group_at(src->seg[j].seg, gorder_slot(src, j, col), gi, group, vs, ve, ns, ne);
}

uint32
sift_col_gorder_seek(SiftColSource *src, int i, int col, uint32 vs, uint32 ve, int64 key, bool after, SiftCGOrdBuf *keep)
{
	int			v,
				j = gorder_src(src, i, &v);
	ColGOrdMem *g = src->gmem[v];
	uint32		lo = vs,
				hi = ve;

	if (j < src->nseg)
		return sift_cseg_gorder_seek(src->seg[j].seg, gorder_slot(src, j, col), vs, ve, key, after, keep);
	if (g->ent != NULL)
	{
		/* the rank of a string (sift_col_gorder_strrank) is the position of its first entry */
		Assert(!after);
		return Max(vs, Min(ve, (uint32) Min((uint64) key, (uint64) g->nvals)));
	}
	if (g->raw != NULL)
		return Max(vs, Min(ve, gmem_rank(g, key, after)));
	SIFT_BISECT(uint32, mid, lo, hi, after ? g->key[mid] <= key : g->key[mid] < key);
	return lo;
}

bool
sift_col_gorder_before(SiftColSource *src, int i, int col, uint32 vs, uint32 ve, int64 v, bool desc, uint32 *clo, uint32 *chi, SiftCGOrdBuf *memo)
{
	int			vv,
				j = gorder_src(src, i, &vv);
	ColGOrdMem *g = src->gmem[vv];
	uint32		lo = vs,
				hi = ve;

	if (j < src->nseg)
		return sift_cseg_gorder_before(src->seg[j].seg, gorder_slot(src, j, col), vs, ve, v, desc, clo, chi, memo);
	/* the write buffer's entries are sorted in memory: the count of those that may come first is exact, and none of them is certainly a row (it is not in the heap's all-visible pages yet), so none is given up */
	if (g->raw != NULL)
		lo = Max(vs, Min(ve, gmem_rank(g, v, desc)));
	else if (!desc)
		SIFT_BISECT(uint32, mid, lo, hi, g->key[mid] < v);
	else
		SIFT_BISECT(uint32, mid, lo, hi, g->key[mid] <= v);
	*chi = desc ? ve - lo : lo - vs;
	*clo = 0;
	return true;
}

void
sift_col_gorder_head(SiftColSource *src, int i, int col, uint32 p, uint32 w, struct SiftCGOrdBuf *buf)
{
	int			v,
				j = gorder_src(src, i, &v);
	ColGOrdMem *g = src->gmem[v];

	if (j < src->nseg)
	{
		sift_cseg_gorder_fill(src->seg[j].seg, gorder_slot(src, j, col), p, w, buf);
		return;
	}
	buf->at = p / SIFT_CGORD_ROWS * SIFT_CGORD_ROWS;
	buf->rows = Min(g->n - buf->at, (uint32) SIFT_CGORD_ROWS);
	buf->cm = Min(buf->rows, Max(w, p - buf->at + 1));
	gmem_sort_range(g, buf->at, Min(buf->at + buf->cm, g->nvals));
	buf->compact = false;
	buf->kfrom = 0;
	buf->kn = buf->cm;
	memcpy(buf->key, g->key + buf->at, buf->cm * sizeof(int64));
	for (uint32 k = 0; k < buf->cm; k++)
		buf->addr[k] = g->tid[buf->at + k];
}

/* The memory of what the reads of the global order make, for sift_col_rescan to drop */
MemoryContext
gorder_cx(SiftColSource *src)
{
	if (src->gcx == NULL)
		src->gcx = AllocSetContextCreate(src->mcxt, "sift col global order", ALLOCSET_DEFAULT_SIZES);
	return src->gcx;
}

static MemoryContext
gexact_cx(SiftColSource *src)
{
	if (src->gxcx == NULL)
		src->gxcx = AllocSetContextCreate(src->mcxt, "sift col unit exactness", ALLOCSET_DEFAULT_SIZES);
	return src->gxcx;
}

/* ---- what is known of an entry of the order: exact, dead, to be confirmed by the heap ---- */

/*
 * Is every entry of the segment exact: no unit has a dead row and every page of the heap that its superblocks cover is all-visible? A table that was vacuumed after its last change is
 * all of this kind, and then the lookup of each entry in its unit (below) is most of what a scan of the whole order costs besides the merge. Asked once for a scan (the units' answers
 * below are kept the same way); a page that is not all-visible, a dead row or a directory that moved leaves the lookups to the units.
 */
static bool
gorder_clean(SiftColSource *src, ColCursor *c)
{
	SiftColSegment *seg = c->seg;
	uint64	   *dirty;
	uint64		av[SIFT_PAGEMASK_WORDS];
	BlockNumber nblocks;
	bool		clean = true;

	if (sift_cseg_keyed(&seg->entry) || seg->nsb == 0 || !segments_current(src))
		return false;
	SIFT_COL_JITTER();
	dirty = palloc(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	sift_cseg_dirty_units(seg, dirty);
	for (uint32 w = 0; w < SIFT_COL_NWORDS(seg->nsb); w++)
		clean &= dirty[w] == 0;
	pfree(dirty);
	nblocks = RelationGetNumberOfBlocks(src->heap);
	for (uint32 u = 0; clean && u < seg->nsb; u++)
	{
		uint32		sb = sift_cseg_unit_sb(seg, u);
		uint64		first = (uint64) sb * SIFT_SB_PAGES,
					pages;

		if (first >= nblocks)
			return false;
		pages = Min((uint64) SIFT_SB_PAGES, nblocks - first);
		src->visibility.sb_mask(src->visibility.arg, sb, av);
		for (uint64 p = 0; p < pages; p += 64)
		{
			uint64		need = pages - p >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (pages - p)) - 1;

			clean &= (av[p / 64] & need) == need;
		}
	}
	return clean;
}

bool
sift_col_gorder_clean(SiftColSource *src, int i)
{
	ColCursor  *c;

	if (i >= src->nseg || (src->flags & COLO_TABLE))
		return false;
	c = &src->seg[i];
	if (c->gclean == 0)
		c->gclean = gorder_clean(src, c) ? 1 : 2;
	return c->gclean == 1;
}

/*
 * What is known of the entry of the global order of stored segment i at heap address tid: 1 if it is of a row that is live and visible to every snapshot, so that nothing needs
 * the heap to confirm it (an OFFSET throws it away unread): its page is all-visible and its row not among the dead rows of its unit; -1 if the row is among them; 0 if the unit holds it
 * live on a page that is not all-visible, which the heap confirms for the snapshot; -2 if the unit cannot say. The dead rows of a unit are listed once for the unit, in the
 * order of col_super_take: the map, the directory, the liveness. A segment cut by key has no unit of the superblock of the page: the page is looked up in the map by block
 * and the dead rows of the segment (those of its units that have any) are one list. The scan asks the heap about a dead row: a vacuumed address may hold a row of another
 * source with the same key, which the merge of the sources has already dropped as the repeat of this entry; the skip of an OFFSET counts it for none, as that row is another source's.
 */
int
sift_col_gorder_exact(SiftColSource *src, int i, SiftKey tid)
{
	ColCursor  *c;
	SiftColSegment *seg;
	GExact	   *x;
	bool		keyed;
	uint32		sb = sift_block_sb(sift_key_block(tid)),
				u = 0,
				lo = 0,
				hi,
				pg;
	uint64		av[SIFT_PAGEMASK_WORDS];

	if (i >= src->nseg || (src->flags & COLO_TABLE))
		return -2;
	if (sift_col_gorder_clean(src, i))
		return 1;
	c = &src->seg[i];
	seg = c->seg;
	keyed = sift_cseg_keyed(&seg->entry);
	if (!keyed)
	{
		/* the superblocks of a segment are mostly all of them in a row: the unit at the offset from the first is the one, else the gallop finds it */
		u = seg->nsb > 0 ? sb - sift_cseg_unit_sb(seg, 0) : 0;
		if (u >= seg->nsb || sift_cseg_unit_sb(seg, u) != sb)
			u = sift_cseg_unit_of_sb(seg, sb);
		if (u >= seg->nsb)
			return -2;
	}
	if (c->gexact == NULL)
		c->gexact = MemoryContextAllocZero(gexact_cx(src), (keyed ? 1 : seg->nsb) * sizeof(GExact));
	x = &c->gexact[u];
	if (x->state == 0)
	{
		uint32		room = 0;

		bool		ok = true;

		x->state = 2;
		if (!keyed)
			col_sb_mask(src, seg, sb, x->av);
		if (segments_current(src))
		{
			SIFT_COL_JITTER();
			if (!keyed)
				ok = sift_cseg_unit_dead(seg, u, gexact_cx(src), &x->dead, &x->ndead, &room);
			else
			{
				uint64	   *dirty = palloc(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));

				sift_cseg_dirty_units(seg, dirty);
				for (uint32 k = 0; ok && k < seg->nsb; k++)
					if (sift_col_bit_test(dirty, k))
						ok = sift_cseg_unit_dead(seg, k, gexact_cx(src), &x->dead, &x->ndead, &room);
				pfree(dirty);
				if (x->ndead > 1)
					qsort(x->dead, x->ndead, sizeof(SiftKey), sift_cmp_u64);
			}
			if (ok)
				x->state = 1;
		}
	}
	if (x->state != 1)
		return -2;
	hi = x->ndead;
	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		if (x->dead[mid] < tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < x->ndead && x->dead[lo] == tid)
		return -1;
	if (keyed)
	{
		BlockNumber blk = sift_key_block(tid);

		if (src->vis.heap == NULL)
			return -2;
		col_blocks_allvisible(src, seg, &blk, 1, av);
		pg = 0;
	}
	else
	{
		memcpy(av, x->av, sizeof(av));
		pg = sift_key_block(tid) - sb * SIFT_SB_PAGES;
	}
	return av[pg / 64] >> (pg % 64) & 1 ? 1 : 0;
}

/* The values of leaf col at n heap addresses, sorted by (source, address) in ord for one pass over the units */
typedef struct ValPair
{
	int			seg;
	SiftKey		tid;
	uint32		at;
} ValPair;

#define VAL_PAIR_LESS(x, y) ((x)->seg != (y)->seg ? (x)->seg < (y)->seg : (x)->tid < (y)->tid)
SIFT_DEFINE_SORT(val_pairs, ValPair, VAL_PAIR_LESS)

/* The m rows of unit u at keys (ascending): false when the unit cannot say (a key-cut unit, a row not found, a chunk that is not plain integers) */
static bool
unit_values(SiftColSegment *seg, uint32 u, int col, const SiftKey *keys, uint32 m, int64 *val, uint8 *st)
{
	SiftCUnitView v;
	SiftCChunk	c;
	uint32	   *rows = palloc(m * sizeof(uint32));

	if (seg->schema->cols[col].store != SIFT_COLS_INT || !sift_cseg_unit_view(seg, u, &v) || v.keyed ||
		sift_cunit_find_rows(&v, sift_block_sb(sift_key_block(keys[0])), keys, m, rows) != m)
		return false;
	if (v.chunk_len[col] > 0)
		sift_cseg_unit_fill(seg, u, v.chunk_off[col], v.chunk_len[col]);
	if (!sift_cunit_chunk(&v, col, &c))
		memset(st, 2, m);		/* no chunk: every value of the unit is NULL */
	else
	{
		if (c.n != v.nrows)
			return false;
		sift_cchunk_decode_sel(&c, rows, m, val);
		for (uint32 k = 0; k < m; k++)
			st[k] = c.ovf != NULL && sift_col_bit_test(c.ovf, rows[k]) ? 0 : c.nulls != NULL && sift_col_bit_test(c.nulls, rows[k]) ? 2 : 1;
	}
	pfree(rows);
	return true;
}

/*
 * The value of integer leaf col of the rows at the heap addresses tid[k] of the segment seg[k], read from the units (the caller has them from sift_col_gorder_exact: live, on all-visible
 * pages, so the unit's value is the row's): st[k] 1 with val[k], 2 a NULL, 0 not read (a source that is not a segment, a key-cut unit, a value the leaf cannot hold): the heap has it.
 */
void
sift_col_gorder_values(SiftColSource *src, int col, const int *seg, const SiftKey *tid, uint32 n, int64 *val, uint8 *st)
{
	ValPair    *pr = palloc(n * sizeof(ValPair));
	int64	   *uv = palloc((n + SIFT_COL_PAD / 8) * sizeof(int64));
	uint8	   *us = palloc(n);
	SiftKey	   *keys = palloc(n * sizeof(SiftKey));

	memset(st, 0, n);
	for (uint32 k = 0; k < n; k++)
		pr[k] = (ValPair) {.seg = seg[k], .tid = tid[k], .at = k};
	val_pairs(pr, n);
	for (uint32 a = 0, b; a < n; a = b)
	{
		uint32		sb = sift_block_sb(sift_key_block(pr[a].tid)),
					u;

		for (b = a; b < n && pr[b].seg == pr[a].seg && sift_block_sb(sift_key_block(pr[b].tid)) == sb; b++)
			keys[b - a] = pr[b].tid;
		if (pr[a].seg >= src->nseg || (src->flags & COLO_TABLE) || sift_cseg_keyed(&src->seg[pr[a].seg].seg->entry) ||
			(u = sift_cseg_unit_of_sb(src->seg[pr[a].seg].seg, sb)) >= src->seg[pr[a].seg].seg->nsb ||
			!unit_values(src->seg[pr[a].seg].seg, u, col, keys, b - a, uv, us))
			continue;
		for (uint32 k = a; k < b; k++)
		{
			val[pr[k].at] = uv[k - a];
			st[pr[k].at] = us[k - a];
		}
	}
	pfree(pr);
	pfree(uv);
	pfree(us);
	pfree(keys);
}
