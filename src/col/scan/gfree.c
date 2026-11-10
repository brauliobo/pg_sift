/*
 * col/scan/gfree.c - the rows of the segments written without the order array of a leaf, made into a source of the global order and kept by the backend (col/scan/gmem.c)
 */
/* check-memory: allow-allocs the rows are at most SIFT_CGORD_FREE_MAX, checked before the allocation */
#include "postgres.h"
#include "col/point.h"
#include "col/wbuf.h"
#include "core/cmp.h"
#include "core/hash.h"
#include "col/scan/gmem.h"

/* The chunk of leaf col of the unit of view v, its slots decoded (the NULL ones 0); NULL when the unit has none (every row is NULL) */
static int64 *
unit_leaf(const SiftCUnitView *v, int col, SiftCChunk *c)
{
	int64	   *vals;

	if (v->chunk_len[col] == 0 || !sift_cunit_chunk(v, col, c))
		return NULL;
	vals = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
	sift_cchunk_decode(c, vals);
	return vals;
}

/* The rows of the segments without an order block of an integer leaf, made once for the scan from the chunks of their units and split by the value of the group leaf the scan pins */
#define GFREE_HASH	64			/* places for the 16 values a scan pins at most: a quarter full */

typedef struct GOrdSlot
{
	GOrdPair   *pv;				/* the entries with a value, nv of capv */
	uint32		nv,
				capv;
	GOrdPair   *pn;				/* those without one */
	uint32		nn,
				capn;
} GOrdSlot;

typedef struct GOrdFree
{
	int			leaf,
				gleaf,
				n;				/* slots: one for each pinned value (one for a scan that pins none) */
	int64		vals[SIFT_GORD_PIN_MAX];
	uint8		hash[GFREE_HASH];	/* open addressing of the pinned values: the slot + 1 of a value, 0 for a free place */
	GOrdSlot	slot[SIFT_GORD_PIN_MAX];
	MemoryContext cx;
} GOrdFree;

static inline uint32
gfree_home(int64 g)
{
	return (uint32) (((uint64) g * UINT64CONST(0x9E3779B97F4A7C15)) >> 58);
}

static void
gfree_pins(GOrdFree *f, const int64 *pins, int n)
{
	f->n = n;
	memcpy(f->vals, pins, n * sizeof(int64));
	for (int i = 0; i < n; i++)
	{
		uint32		h = gfree_home(pins[i]);

		while (f->hash[h] != 0)
			h = (h + 1) % GFREE_HASH;
		f->hash[h] = i + 1;
		/* never NULL: the slot is copied out whole */
		f->slot[i].pv = MemoryContextAlloc(f->cx, sizeof(GOrdPair));
		f->slot[i].pn = MemoryContextAlloc(f->cx, sizeof(GOrdPair));
		f->slot[i].capv = f->slot[i].capn = 1;
	}
}

/* The slot of group value g, -1 when the scan pins none such */
static inline int
gfree_slot(const GOrdFree *f, int64 g)
{
	for (uint32 h = gfree_home(g); f->hash[h] != 0; h = (h + 1) % GFREE_HASH)
		if (f->vals[f->hash[h] - 1] == g)
			return f->hash[h] - 1;
	return -1;
}

static void
slot_grow(GOrdPair **a, uint32 *cap)
{
	*cap = Max(*cap * 2, 256);
	*a = repalloc(*a, *cap * sizeof(GOrdPair));
}

/*
 * The live rows of unit u of a segment without an order block, for an integer leaf (and the group leaf gleaf, when >= 0), put in the slot of their group value as gord_collect puts those
 * of a write buffer: the chunks of the two leaves decode in bulk and no batch is made (65 instructions a row, where a batch costs 360). False when a row holds an overflow value of
 * a leaf or an address a block of the global order cannot name.
 */
static bool
gord_load_unit(SiftColSegment *seg, uint32 u, GOrdFree *f)
{
	const SiftColSBRef *ref = &seg->sbdir[u];
	SiftCUnitView view;
	SiftCChunk	c = {0},
				gc = {0};
	int64	   *val,
			   *gvals = NULL;
	SiftKey    *keys = palloc(Max(ref->nrows, 1) * sizeof(SiftKey));
	uint64	   *live = palloc(SIFT_COL_NWORDS(ref->nrows) * sizeof(uint64));
	bool		ok = true;

	if (!sift_cseg_unit_view(seg, u, &view))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column unit: unit %u of a segment does not parse", u)));
	for (int col = 0; col < seg->schema->ncols; col++)
		if ((col == f->leaf || col == f->gleaf) && view.chunk_len[col] > 0)
			sift_cseg_unit_fill(seg, u, view.chunk_off[col], view.chunk_len[col]);
	val = unit_leaf(&view, f->leaf, &c);
	if (f->gleaf >= 0)
		gvals = unit_leaf(&view, f->gleaf, &gc);
	sift_cunit_keys(&view, ref->id, keys);
	sift_cseg_live(seg, u, live);
	for (uint32 r = 0; r < ref->nrows && ok; r++)
	{
		GOrdSlot   *sl = f->slot;
		GOrdPair	p;

		if (!sift_col_bit_test(live, r))
			continue;
		if (sift_col_bit_test(c.ovf, r) || (keys[r] & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF || sift_col_bit_test(gc.ovf, r))
		{
			ok = false;
			continue;
		}
		if (f->gleaf >= 0)
		{
			int			at;

			if (gvals == NULL || sift_col_bit_test(gc.nulls, r) || (at = gfree_slot(f, gvals[r])) < 0)
				continue;
			sl += at;
		}
		p = (GOrdPair) {val ? val[r] : 0, (uint64) keys[r]};
		if (val == NULL || sift_col_bit_test(c.nulls, r))
		{
			if (sl->nn == sl->capn)
				slot_grow(&sl->pn, &sl->capn);
			sl->pn[sl->nn++] = p;
		}
		else
		{
			if (sl->nv == sl->capv)
				slot_grow(&sl->pv, &sl->capv);
			sl->pv[sl->nv++] = p;
		}
	}
	pfree(keys);
	pfree(live);
	if (val)
		pfree(val);
	if (gvals)
		pfree(gvals);
	return ok;
}

/* The values of group leaf gcol of the live rows of a segment without an order block, appended to vals (room for its rows); false when a live row has no value or an overflow one */
bool
gord_free_group_values(SiftColSegment *seg, int gcol, int64 *vals, uint64 *n)
{
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const SiftColSBRef *ref = &seg->sbdir[u];
		SiftCUnitView view;
		SiftCChunk	c = {0};
		int64	   *val;
		uint64	   *live = palloc(SIFT_COL_NWORDS(ref->nrows) * sizeof(uint64));
		bool		ok = true;

		if (!sift_cseg_unit_view(seg, u, &view))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column unit: unit %u of a segment does not parse", u)));
		if (view.chunk_len[gcol] > 0)
			sift_cseg_unit_fill(seg, u, view.chunk_off[gcol], view.chunk_len[gcol]);
		val = unit_leaf(&view, gcol, &c);
		sift_cseg_live(seg, u, live);
		for (uint32 r = 0; r < ref->nrows && ok; r++)
			if (sift_col_bit_test(live, r))
			{
				ok = val != NULL && !sift_col_bit_test(c.nulls, r) && !sift_col_bit_test(c.ovf, r);
				if (ok)
					vals[(*n)++] = val[r];
			}
		pfree(live);
		if (val)
			pfree(val);
		if (!ok)
			return false;
	}
	return true;
}

/* The source of the segments without a block is the one of leaf, gleaf and the values the scan pins now */
static bool
gfree_loaded(const SiftColSource *src, int leaf, int gleaf)
{
	return src->gfsrc && src->gf_leaf == leaf && src->gf_gleaf == gleaf && (gleaf < 0 || (src->gf_n == src->gpin_n && memcmp(src->gf_vals, src->gpin_vals, src->gf_n * sizeof(int64)) == 0));
}

static GFreeTok *
gfree_tokens(const SiftColSource *src, int *n)
{
	GFreeTok   *t = palloc(Max(src->nseg, 1) * sizeof(GFreeTok));

	*n = 0;
	for (int i = 0; i < src->nseg; i++)
		if (src->seg[i].gfree)
			t[(*n)++] = (GFreeTok) {src->seg[i].seg->entry.gen, sift_cseg_live_rows(src->seg[i].seg)};
	return t;
}

/*
 * The live rows of the segments without an order block of an integer leaf as one source of the global order for each value of the group the scan pins: made from the chunks of their units
 * (gord_load_unit) and kept by the backend under the generation and the live rows of the segments, so that a scan after the first reads the order that is there (the units are read once for all
 * the scans of the backend, not for each)
 */
static bool
gfree_open(SiftColSource *src, int leaf, int gleaf)
{
	int			n;
	GFreeTok   *tok = gfree_tokens(src, &n);
	bool		hit[SIFT_GORD_PIN_MAX];
	bool		all = true;
	MemoryContext tmp = NULL;
	GOrdFree   *f = NULL;

	/* an order found is held at once: the orders stored for the other values below may push it out of the backend's cache */
	for (int v = 0; v < src->gpin_n; v++)
	{
		struct GOrdKept *k = gkept_find_free(src, leaf, gleaf, gleaf >= 0 ? src->gpin_vals[v] : 0, tok, n);

		if ((hit[v] = k != NULL))
		{
			GKEPT_STAT(0);
			src->gfmem[v] = gkept_use(src, k);
		}
		all &= hit[v];
	}
	if (!all)
	{
		tmp = AllocSetContextCreate(CurrentMemoryContext, "sift cache unordered segment rows", ALLOCSET_DEFAULT_SIZES);
		f = MemoryContextAllocZero(tmp, sizeof(GOrdFree));
		f->leaf = leaf;
		f->gleaf = gleaf;
		f->cx = tmp;
		gfree_pins(f, src->gpin_vals, src->gpin_n);
		for (int i = 0; i < src->nseg; i++)
			for (uint32 u = 0; src->seg[i].gfree && u < src->seg[i].seg->nsb; u++)
				if (!gord_load_unit(src->seg[i].seg, u, f))
				{
					MemoryContextDelete(tmp);
					return false;
				}
	}
	for (int v = 0; v < src->gpin_n; v++)
	{
		if (!hit[v])
		{
			MemoryContext cx = AllocSetContextCreate(gorder_cx(src), "sift cache unordered segment order", ALLOCSET_DEFAULT_SIZES);
			GOrdSlot   *sl = &f->slot[v];
			int64		gval = gleaf >= 0 ? src->gpin_vals[v] : 0;
			ColGOrdMem *g = gord_mem_make(leaf, gleaf, gval, cx, sl->pv, sl->nv, sl->pn, sl->nn);

			GKEPT_STAT(2);
			gkept_store_free(src, cx, g, leaf, gleaf, gval, tok, n, v);
		}
	}
	if (tmp != NULL)
		MemoryContextDelete(tmp);
	src->gfsrc = true;
	src->gf_leaf = leaf;
	src->gf_gleaf = gleaf;
	src->gf_n = src->gpin_n;
	memcpy(src->gf_vals, src->gpin_vals, src->gpin_n * sizeof(int64));
	return true;
}

bool
gord_mem_load(SiftColSource *src, int leaf, int gleaf, uint64 rows, uint32 collid)
{
	MemoryContext cx = gorder_cx(src);
	bool		want[SIFT_COL_MAX_COLS] = {false};
	uint64		free_rows = 0;
	uint32		units = 0;
	bool		direct = src->schema->cols[leaf].store == SIFT_COLS_INT && (gleaf < 0 || src->schema->cols[gleaf].store == SIFT_COLS_INT);

	if (direct ? gfree_loaded(src, leaf, gleaf) : (src->ngb > 0 && src->gb_leaf == leaf && src->gb_gleaf == gleaf))
		return true;
	for (int i = 0; i < src->nseg; i++)
		if (src->seg[i].gfree)
		{
			free_rows += src->seg[i].seg->entry.ndocs;
			units += src->seg[i].seg->nsb;
		}
	if (free_rows > SIFT_CGORD_FREE_CAP(src->schema->cols[leaf].store != SIFT_COLS_INT) || free_rows + gord_mem_rows(src) > SIFT_CGORD_MEM_CAP(collid != 0 && !sift_collate_is_c(collid), rows))
		return false;
	/* the rows of an integer leaf are made straight from the chunks of the units, once for all the groups of the scan (gord_load_unit); those of a text leaf come with their strings in the batches of a decode */
	if (direct)
		return gfree_open(src, leaf, gleaf);
	src->gb_leaf = leaf;
	src->gb_gleaf = gleaf;
	src->ngb = 0;
	src->gfsrc = false;
	want[leaf] = true;
	if (gleaf >= 0)
		want[gleaf] = true;
	src->gb = MemoryContextAlloc(cx, (units + 1) * sizeof(SiftColBatch *));
	for (int i = 0; i < src->nseg; i++)
		for (uint32 u = 0; src->seg[i].gfree && u < src->seg[i].seg->nsb; u++)
		{
			src->gb[src->ngb] = sift_cbatch_create(cx, src->seg[i].seg->schema);
			sift_cseg_decode_unit_leaves(src->seg[i].seg, u, want, src->gb[src->ngb++]);
		}
	return true;
}

