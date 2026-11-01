/*
 * col/scan/gorder.c - the global order of a scan: the array of a column in key order
 * built from the stored arrays and from the write buffer
 * the reads of its strings, ranges and heads
 */
#include "postgres.h"
#include "col/point.h"
#include "core/cmp.h"
#include "col/scan_int.h"

/* ---- the global order of a leaf as a source of sorted entries (col/gorder.h): the stored segments and the rows of the write buffers ---- */

typedef struct GOrdPair
{
	int64		key;
	uint64		tid;
} GOrdPair;

typedef struct ColGOrdMem
{
	int			leaf;
	int			gleaf;			/* the rows of the group gval of this leaf only (-1: all) */
	int64		gval;
	uint32		n;				/* entries: nvals with a value by (key, address), then those without by address */
	uint32		nvals;
	int64	   *key;
	uint64	   *tid;
	/* numeric key: key and tid of a range hold its entries once the range is sorted (gmem_sort); raw holds them by range, unsorted */
	GOrdPair   *raw;
	uint32	   *bstart;			/* the first entry of each of the nb ranges of the key (nb + 1 values) */
	bool	   *bdone;
	uint32		nb;
	int			shift;			/* the range of a key is (key - kmin) >> shift */
	int64		kmin;
	uint32		collid;
	uint32		nstr;			/* a text key: the key of an entry is the rank of its string among the nstr distinct ones, which are str[rank] (len[rank] bytes, in the batches of the buffers) */
	const char **str;
	uint32	   *len;
} ColGOrdMem;

typedef struct GOrdText
{
	const char *s;
	uint32		len;
	uint64		tid;
} GOrdText;

static uint32 gord_text_coll;

static int
gord_text_cmp(const void *a, const void *b)
{
	const GOrdText *x = a,
			   *y = b;
	int			c = sift_cgord_str_order(x->s, x->len, y->s, y->len, gord_text_coll);

	return c != 0 ? c : SIFT_CMP3(x->tid, y->tid);
}

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

/* The ranges of the key of the buffer sorted so far: what a scan reads is a few ranges of them, and a seek or a count by key sorts the one the key falls in */
static void
gmem_sort(ColGOrdMem *g, uint32 b)
{
	uint32		from = g->bstart[b],
				to = g->bstart[b + 1];

	if (g->bdone[b])
		return;
	gord_pair_sort(g->raw + from, to - from);
	for (uint32 i = from; i < to; i++)
	{
		g->key[i] = g->raw[i].key;
		g->tid[i] = g->raw[i].tid;
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

/* The rows of the write buffers of a text leaf sorted like the entries of a text block: by string (the collation, then the bytes) and address, the key the rank of the string; false as gord_mem, or a string longer than a dictionary entry */
static bool
gord_mem_text(SiftColSource *src, int leaf, uint64 maxrows, uint32 collid, int v)
{
	uint64		n = 0;
	GOrdText   *pv;
	uint64	   *pn;
	uint32		nv = 0,
				nn = 0,
				nstr = 0;
	ColGOrdMem *g;

	for (int i = 0; i < src->nmem; i++)
		n += src->mem[i].b->nrows;
	if (n > maxrows)
		return false;
	pv = palloc((n + 1) * sizeof(GOrdText));
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
				pv[nv++] = (GOrdText) {str, len, (uint64) b->keys[r]};
			}
		}
	}
	gord_text_coll = collid;
	qsort(pv, nv, sizeof(GOrdText), gord_text_cmp);
	sift_sort_u64(pn, nn);
	g = MemoryContextAllocZero(src->mcxt, sizeof(ColGOrdMem));
	g->leaf = leaf;
	g->gleaf = -1;
	g->n = nv + nn;
	g->nvals = nv;
	g->key = MemoryContextAlloc(src->mcxt, (Size) (g->n + 1) * sizeof(int64));
	g->tid = MemoryContextAlloc(src->mcxt, (Size) (g->n + 1) * sizeof(uint64));
	g->str = MemoryContextAlloc(src->mcxt, ((Size) nv + 1) * sizeof(char *));
	g->len = MemoryContextAlloc(src->mcxt, ((Size) nv + 1) * sizeof(uint32));
	for (uint32 i = 0; i < nv; i++)
	{
		if (i == 0 || pv[i].len != pv[i - 1].len || memcmp(pv[i].s, pv[i - 1].s, pv[i].len) != 0)
		{
			g->str[nstr] = pv[i].s;
			g->len[nstr++] = pv[i].len;
		}
		g->key[i] = nstr - 1;
		g->tid[i] = pv[i].tid;
	}
	g->nstr = nstr;
	g->collid = collid;
	for (uint32 i = 0; i < nn; i++)
	{
		g->key[nv + i] = 0;
		g->tid[nv + i] = pn[i];
	}
	src->gmem[v] = g;
	pfree(pv);
	pfree(pn);
	return true;
}

/* The entries with a value cut into ranges of the key of the same width, as many as 1 / 64 of them (4,096 at most): one pass counts them, one puts them in the order of the ranges. pv is freed */
static void
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

/* The rows of the write buffers sorted like a segment's entries; false when a text key has more than maxrows rows, a numeric one more than SIFT_CGORD_MEM_MAX, or one holds an overflow value of the leaf or an address a block of the global order cannot name */
static bool
gord_mem(SiftColSource *src, int leaf, uint64 maxrows, int gleaf, int64 gval, int v, uint32 collid)
{
	ColGOrdMem *g = src->gmem[v];
	uint64		n = 0;
	GOrdPair   *pv,
			   *pn;
	uint32		nv = 0,
				nn = 0;

	if (g != NULL && g->leaf == leaf && g->gleaf == gleaf && (gleaf < 0 || g->gval == gval) && (g->str != NULL) == (collid != 0))
		return true;
	for (int i = 0; i < src->nmem; i++)
		n += src->mem[i].b->nrows;
	if (collid != 0)
		return gord_mem_text(src, leaf, maxrows, collid, v);
	if (n > SIFT_CGORD_MEM_MAX)
		return false;
	pv = palloc((n + 1) * sizeof(GOrdPair));
	pn = palloc((n + 1) * sizeof(GOrdPair));
	for (int i = 0; i < src->nmem; i++)
	{
		const SiftColBatch *b = src->mem[i].b;
		const SiftCVals *v = &b->col[leaf];

		for (uint32 r = 0; r < b->nrows; r++)
		{
			if ((v->ovf && (v->ovf[r / 64] >> (r % 64) & 1)) || (b->keys[r] & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF)
				return false;
			if (gleaf >= 0)
			{
				const SiftCVals *gv = &b->col[gleaf];

				if ((gv->ovf && (gv->ovf[r / 64] >> (r % 64) & 1)) || (gv->nulls && (gv->nulls[r / 64] >> (r % 64) & 1)))
				{
					if (gv->ovf && (gv->ovf[r / 64] >> (r % 64) & 1))
						return false;
					continue;
				}
				if (gv->key[r] != gval)
					continue;
			}
			if (v->nulls && (v->nulls[r / 64] >> (r % 64) & 1))
				pn[nn++] = (GOrdPair) {0, (uint64) b->keys[r]};
			else
				pv[nv++] = (GOrdPair) {v->key[r], (uint64) b->keys[r]};
		}
	}
	gord_pair_sort(pn, nn);
	g = MemoryContextAllocZero(src->mcxt, sizeof(ColGOrdMem));
	g->leaf = leaf;
	g->gleaf = gleaf;
	g->gval = gval;
	g->n = nv + nn;
	g->nvals = nv;
	g->key = MemoryContextAlloc(src->mcxt, (Size) (g->n + 1) * sizeof(int64));
	g->tid = MemoryContextAlloc(src->mcxt, (Size) (g->n + 1) * sizeof(uint64));
	gmem_ranges(g, src->mcxt, pv, nv);
	for (uint32 i = 0; i < nn; i++)
	{
		g->key[nv + i] = 0;
		g->tid[nv + i] = pn[i].tid;
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

	if ((src->flags & (COLO_TABLE | SIFT_COLO_NOWBUF)) || src->schema->cols[col].kind != SIFT_COLK_SCALAR)
		return -1;
	if (!(npin > 0 && gorder_all(src, col, true, npin, pinleaf, pinval, &gleaf, vals, &nv, collid)) && !gorder_all(src, col, false, npin, pinleaf, pinval, &gleaf, vals, &nv, collid))
		return -1;
	src->gpin_leaf = gleaf;
	src->gpin_n = Max(nv, 1);
	memcpy(src->gpin_vals, vals, nv * sizeof(int64));
	*grouped = gleaf;
	for (int i = 0; i < src->nseg; i++)
	{
		src->seg[i].gexact = NULL;
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
	const ColGOrdMem *g = src->gmem[v];
	uint32		lo = 0,
				hi;

	if (j < src->nseg)
		return sift_cseg_gorder_strrank(src->seg[j].seg, gorder_slot(src, j, col), s, len, after);
	hi = g->nstr;
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
