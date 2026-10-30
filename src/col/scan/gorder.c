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

typedef struct ColGOrdMem
{
	int			leaf;
	int			gleaf;			/* the rows of the group gval of this leaf only (-1: all) */
	int64		gval;
	uint32		n;				/* entries: nvals with a value by (key, address), then those without by address */
	uint32		nvals;
	int64	   *key;
	uint64	   *tid;
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

typedef struct GOrdPair
{
	int64		key;
	uint64		tid;
} GOrdPair;

static int
gord_pair_cmp(const void *a, const void *b)
{
	const GOrdPair *x = a,
			   *y = b;

	return x->key != y->key ? SIFT_CMP3(x->key, y->key) : SIFT_CMP3(x->tid, y->tid);
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

/* The rows of the write buffers sorted like a segment's entries; false when they are more than maxrows (sorting them whole costs more than reading them as units), or one holds an overflow value of the leaf or an address a block of the global order cannot name */
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
	/* the rows of one group are the ones sorted: a buffer of many rows is looked through for them, up to 64 times what an ungrouped scan sorts */
	if (n > (gleaf < 0 ? maxrows : 64 * maxrows))
		return false;
	if (collid != 0)
		return gord_mem_text(src, leaf, maxrows, collid, v);
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
	if ((uint64) nv + nn > maxrows)
		return false;
	qsort(pv, nv, sizeof(GOrdPair), gord_pair_cmp);
	qsort(pn, nn, sizeof(GOrdPair), gord_pair_cmp);
	g = MemoryContextAllocZero(src->mcxt, sizeof(ColGOrdMem));
	g->leaf = leaf;
	g->gleaf = gleaf;
	g->gval = gval;
	g->n = nv + nn;
	g->nvals = nv;
	g->key = MemoryContextAlloc(src->mcxt, (Size) (g->n + 1) * sizeof(int64));
	g->tid = MemoryContextAlloc(src->mcxt, (Size) (g->n + 1) * sizeof(uint64));
	for (uint32 i = 0; i < nv; i++)
	{
		g->key[i] = pv[i].key;
		g->tid[i] = pv[i].tid;
	}
	for (uint32 i = 0; i < nn; i++)
	{
		g->key[nv + i] = 0;
		g->tid[nv + i] = pn[i].tid;
	}
	src->gmem[v] = g;
	pfree(pv);
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

		if (slot < 0 || !sift_cseg_gorder(src->seg[i].seg, slot, &entries, &values) || sift_cseg_gorder_collation(src->seg[i].seg, slot) != collid)
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
sift_col_gorder_open(SiftColSource *src, int col, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid)
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
		src->seg[i].gexact = NULL;
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
sift_col_gorder_info(SiftColSource *src, int i, int col, uint32 *entries, uint32 *values)
{
	int			v,
				j = gorder_src(src, i, &v);

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

	if (j < src->nseg)
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
	const ColGOrdMem *g = src->gmem[v];
	uint32		lo = vs,
				hi = ve;

	if (j < src->nseg)
		return sift_cseg_gorder_seek(src->seg[j].seg, gorder_slot(src, j, col), vs, ve, key, after, keep);
	SIFT_BISECT(uint32, mid, lo, hi, after ? g->key[mid] <= key : g->key[mid] < key);
	return lo;
}

void
sift_col_gorder_head(SiftColSource *src, int i, int col, uint32 p, uint32 w, struct SiftCGOrdBuf *buf)
{
	int			v,
				j = gorder_src(src, i, &v);
	const ColGOrdMem *g = src->gmem[v];

	if (j < src->nseg)
	{
		sift_cseg_gorder_fill(src->seg[j].seg, gorder_slot(src, j, col), p, w, buf);
		return;
	}
	buf->at = p / SIFT_CGORD_ROWS * SIFT_CGORD_ROWS;
	buf->rows = Min(g->n - buf->at, (uint32) SIFT_CGORD_ROWS);
	buf->cm = Min(buf->rows, Max(w, p - buf->at + 1));
	memcpy(buf->key, g->key + buf->at, buf->cm * sizeof(int64));
	for (uint32 k = 0; k < buf->cm; k++)
		buf->addr[k] = g->tid[buf->at + k];
}
