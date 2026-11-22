/*
 * col/f2/f2_col_delta_idx.c - the column delta chain of a sift_col index: records, address changes, the patch of a granule (col/f2/f2_col_delta_idx.h). No server call.
 */
#include "postgres.h"

#include "col/f2/f2_col_delta_idx.h"
#include "core/hash.h"

/* ---- the records ---- */

uint32
sift_cdi_encode(uint8 *dst, uint32 cap, const SiftColDeltaTarget *target, TransactionId xmin, CommandId cid, bool subxid, bool dirty_scope, uint64 new_tid, const SiftCellKind *kinds,
				const int16 *leaf, const SiftCell *cell, const bool *key_absent, int nleaf)
{
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR];
	uint8		flags = SIFT_CDENT_FLAG_LEAF_ADDR | (subxid ? SIFT_CDENT_FLAG_SUBXID : 0) | (dirty_scope ? SIFT_CDENT_FLAG_DIRTY_SCOPE : 0);
	int			n = 0;

	if (nleaf + 1 > SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR)
		return 0;
	cols[n++] = (SiftColDeltaCol) {.attnum = SIFT_CD_ATT_TID_REMAP,.kind = SIFT_CELL_INT,.cell = {.i = (int64) new_tid}};
	for (int i = 0; i < nleaf; i++)
	{
		cols[n++] = (SiftColDeltaCol) {.attnum = (int16) (leaf[i] + 1),.kind = kinds[leaf[i]],.cell = cell[i],.key_absent = key_absent != NULL && key_absent[i]};
		if (cell[i].tag != 0)
			flags |= SIFT_CDENT_FLAG_TAG_BYTE;
	}
	return sift_col_delta_encode(dst, cap, target, xmin, cid, flags, cols, n);
}

/* ---- the address changes of a chain ---- */

static int
slot_cmp(const void *a, const void *b)
{
	const SiftCdRemap *x = a,
			   *y = b;

	if (x->part_gen != y->part_gen)
		return x->part_gen < y->part_gen ? -1 : 1;
	if (x->granule_id != y->granule_id)
		return x->granule_id < y->granule_id ? -1 : 1;
	return (x->row > y->row) - (x->row < y->row);
}

static int
new_cmp(const void *a, const void *b)
{
	uint64		x = ((const SiftCdRemap *) a)->new_tid,
				y = ((const SiftCdRemap *) b)->new_tid;

	return x < y ? -1 : x > y;
}

typedef struct Seen
{
	SiftCdRemap r;
	uint32		seq;
} Seen;

static int
seen_cmp(const void *a, const void *b)
{
	const Seen *x = a,
			   *y = b;
	int			c = slot_cmp(&x->r, &y->r);

	return c != 0 ? c : (x->seq > y->seq) - (x->seq < y->seq);
}

static uint32
slot_home(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, uint16 row)
{
	return (uint32) (sift_fmix64(part_gen * SIFT_FIB64 + ((uint64) granule_id << 16 | row)) & m->ht_mask);
}

static uint32
new_home(const SiftCdRemapMap *m, uint64 tid)
{
	return (uint32) (sift_fmix64(tid) & m->ht_mask);
}

/* The summary of the changes [first, first + n) of slot: added to sums */
static void
summary_add(SiftCdSummary *s, const SiftCdRemap *r, int at)
{
	BlockNumber blk = (BlockNumber) (r->new_tid >> 16);

	if (s->n == 0)
		*s = (SiftCdSummary) {.part_gen = r->part_gen,.granule_id = s->granule_id,.blk_min = blk,.blk_max = blk,.first = at};
	s->blk_min = Min(s->blk_min, blk);
	s->blk_max = Max(s->blk_max, blk);
	s->blk_bloom |= sift_tid_block_bit(r->new_tid);
	s->n++;
}

/* The hashes of the two orders and the sums of each part and of each granule, in the one pass that slot allows */
static void
remap_index(SiftCdRemapMap *m)
{
	uint32		cap = 64;

	m->parts = palloc(m->n * sizeof(SiftCdSummary));
	m->grans = palloc(m->n * sizeof(SiftCdSummary));
	for (int i = 0; i < m->n; i++)
	{
		const SiftCdRemap *r = &m->slot[i];

		if (m->nparts == 0 || m->parts[m->nparts - 1].part_gen != r->part_gen)
			m->parts[m->nparts++] = (SiftCdSummary) {0};
		if (m->ngrans == 0 || m->grans[m->ngrans - 1].part_gen != r->part_gen || m->grans[m->ngrans - 1].granule_id != r->granule_id)
			m->grans[m->ngrans++] = (SiftCdSummary) {.granule_id = r->granule_id};
		summary_add(&m->parts[m->nparts - 1], r, i);
		summary_add(&m->grans[m->ngrans - 1], r, i);
	}
	if (m->n <= SIFT_CD_REMAP_HASH_MIN)
		return;
	while (cap < 2 * (uint32) m->n)
		cap *= 2;
	m->ht_mask = cap - 1;
	m->slot_ht = palloc0(cap * sizeof(uint32));
	m->new_ht = palloc0(cap * sizeof(uint32));
	for (int i = 0; i < m->n; i++)
	{
		uint32		h = slot_home(m, m->slot[i].part_gen, m->slot[i].granule_id, m->slot[i].row);

		while (m->slot_ht[h] != 0)
			h = (h + 1) & m->ht_mask;
		m->slot_ht[h] = i + 1;
		h = new_home(m, m->by_new[i].new_tid);
		while (m->new_ht[h] != 0)
			h = (h + 1) & m->ht_mask;
		m->new_ht[h] = i + 1;
	}
}

SiftCdRemapMap *
sift_cd_remap_build(const SiftColDeltaChain *chain, const SiftColDeltaOps *ops)
{
	Seen	   *v = NULL;
	int			n = 0,
				cap = 0,
				m = 0;
	uint32		seq = 0;
	SiftCdRemapMap *map;

	for (int i = 0; i < chain->npages; i++)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		const uint8 *p = sift_col_delta_page_first(page),
				   *end = sift_col_delta_page_end(page);

		while (p < end)
		{
			SiftColDeltaEntryHdr h;
			SiftColDeltaPatch col;

			memcpy(&h, p, sizeof(h));
			if ((h.flags & SIFT_CDENT_FLAG_LEAF_ADDR) != 0 && h.ncols > 0 && ops->visible(ops->ctx, h.xmin, h.cid) &&
				sift_col_delta_decode_col(p + sizeof(h), p + h.rec_bytes, &col) != NULL && col.attnum == SIFT_CD_ATT_TID_REMAP)
			{
				if (n == cap)
				{
					cap = Max(16, cap * 2);
					v = v != NULL ? repalloc(v, cap * sizeof(Seen)) : palloc(cap * sizeof(Seen));
				}
				v[n++] = (Seen) {{h.part_gen, h.granule_id, h.row_ordinal, (uint64) col.val.i}, seq};
			}
			seq++;
			p += h.rec_bytes;
		}
	}
	if (n == 0)
		return NULL;
	qsort(v, n, sizeof(Seen), seen_cmp);
	map = palloc0(sizeof(SiftCdRemapMap));
	map->slot = palloc(n * sizeof(SiftCdRemap));
	for (int i = 0; i < n; i++)
		if (i + 1 == n || slot_cmp(&v[i].r, &v[i + 1].r) != 0)
			map->slot[m++] = v[i].r;
	map->n = m;
	map->by_new = memcpy(palloc(m * sizeof(SiftCdRemap)), map->slot, m * sizeof(SiftCdRemap));
	qsort(map->by_new, m, sizeof(SiftCdRemap), new_cmp);
	pfree(v);
	remap_index(map);
	return map;
}

void
sift_cd_remap_free(SiftCdRemapMap *m)
{
	if (m == NULL)
		return;
	pfree(m->slot);
	pfree(m->by_new);
	pfree(m->parts);
	pfree(m->grans);
	if (m->slot_ht != NULL)
	{
		pfree(m->slot_ht);
		pfree(m->new_ht);
	}
	pfree(m);
}

/* The first entry of the slot-ordered array that is not before the place */
static int
slot_lower(const SiftCdRemapMap *m, const SiftCdRemap *key)
{
	int			a = 0,
				b = m->n;

	while (a < b)
	{
		int			mid = a + (b - a) / 2;

		if (slot_cmp(&m->slot[mid], key) < 0)
			a = mid + 1;
		else
			b = mid;
	}
	return a;
}

const SiftCdRemap *
sift_cd_remap_by_new(const SiftCdRemapMap *m, uint64 tid)
{
	SiftCdRemap key = {.new_tid = tid};

	if (m == NULL)
		return NULL;
	if (m->new_ht == NULL)
		return bsearch(&key, m->by_new, m->n, sizeof(SiftCdRemap), new_cmp);
	for (uint32 h = new_home(m, tid); m->new_ht[h] != 0; h = (h + 1) & m->ht_mask)
		if (m->by_new[m->new_ht[h] - 1].new_tid == tid)
			return &m->by_new[m->new_ht[h] - 1];
	return NULL;
}

uint64
sift_cd_remap_new_tid(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, uint16 row)
{
	SiftCdRemap key = {part_gen, granule_id, row, 0};
	int			i;

	if (m == NULL)
		return 0;
	if (m->slot_ht != NULL)
	{
		for (uint32 h = slot_home(m, part_gen, granule_id, row); m->slot_ht[h] != 0; h = (h + 1) & m->ht_mask)
			if (slot_cmp(&m->slot[m->slot_ht[h] - 1], &key) == 0)
				return m->slot[m->slot_ht[h] - 1].new_tid;
		return 0;
	}
	i = slot_lower(m, &key);
	return i < m->n && slot_cmp(&m->slot[i], &key) == 0 ? m->slot[i].new_tid : 0;
}

bool
sift_cd_remap_supersedes(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, uint16 row, uint64 raw_tid)
{
	uint64		now = sift_cd_remap_new_tid(m, part_gen, granule_id, row);

	return now != 0 && now != raw_tid;
}

/* The summary of the part or of the granule that the key is the place of, NULL when no change is there */
static const SiftCdSummary *
summary_find(const SiftCdSummary *v, int n, uint64 part_gen, uint32 granule_id)
{
	int			a = 0,
				b = n;

	while (a < b)
	{
		int			mid = a + (b - a) / 2;

		if (v[mid].part_gen < part_gen || (v[mid].part_gen == part_gen && v[mid].granule_id < granule_id))
			a = mid + 1;
		else
			b = mid;
	}
	return a < n && v[a].part_gen == part_gen && v[a].granule_id == granule_id ? &v[a] : NULL;
}

/* Whether a change of the summary lies in a block the map says is not all-visible: the summary says no for most, and the changes are walked for the rest */
static bool
summed_in_dirty(const SiftCdRemapMap *m, const SiftCdSummary *s, const SiftVmDirtySet *ds)
{
	if (s == NULL || !sift_vm_dirty_overlaps_bloom(ds, s->blk_min, s->blk_max, s->blk_bloom))
		return false;
	for (int i = s->first; i < s->first + s->n; i++)
		if (sift_vm_dirty_overlaps(ds, (BlockNumber) (m->slot[i].new_tid >> 16), (BlockNumber) (m->slot[i].new_tid >> 16)))
			return true;
	return false;
}

bool
sift_cd_remap_part_in_dirty(const SiftCdRemapMap *m, uint64 part_gen, const SiftVmDirtySet *ds)
{
	return m != NULL && summed_in_dirty(m, summary_find(m->parts, m->nparts, part_gen, 0), ds);
}

bool
sift_cd_remap_granule_in_dirty(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, const SiftVmDirtySet *ds)
{
	return m != NULL && summed_in_dirty(m, summary_find(m->grans, m->ngrans, part_gen, granule_id), ds);
}

void
sift_cd_remap_cursor_init(SiftCdRemapCursor *cur, const SiftCdRemapMap *rm, uint64 part_gen, uint32 granule_id)
{
	const SiftCdSummary *s = rm != NULL ? summary_find(rm->grans, rm->ngrans, part_gen, granule_id) : NULL;

	*cur = (SiftCdRemapCursor) {.rm = rm,.pos = s != NULL ? s->first : 0,.end = s != NULL ? s->first + s->n : 0};
}

uint64
sift_cd_remap_cursor_new_tid(SiftCdRemapCursor *cur, uint16 row)
{
	while (cur->pos < cur->end && cur->rm->slot[cur->pos].row < row)
		cur->pos++;
	return cur->pos < cur->end && cur->rm->slot[cur->pos].row == row ? cur->rm->slot[cur->pos].new_tid : 0;
}

/* ---- what a scan lays over a part ---- */

/* The arrays of a leaf's view copied, so that a patch is written over a copy: the tag array is made when the leaf had none. A leaf of a key-exists predicate has its key bits alone */
static void
copy_view(ColView *v, int n, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);

	if (v->keyp != NULL)
		v->keyp = memcpy(palloc(Max(n, 1) * sizeof(bool)), v->keyp, n * sizeof(bool));
	if (v->isnull != NULL)
	{
		v->isnull = memcpy(palloc(Max(n, 1) * sizeof(bool)), v->isnull, n * sizeof(bool));
		v->tag = v->tag != NULL ? memcpy(palloc(Max(n, 1)), v->tag, n) : palloc0(Max(n, 1));
		if (v->kind == SIFT_CELL_INT)
			v->i = memcpy(palloc(Max(n, 1) * sizeof(int64)), v->i, n * sizeof(int64));
		else if (v->kind == SIFT_CELL_F8)
			v->f = memcpy(palloc(Max(n, 1) * sizeof(double)), v->f, n * sizeof(double));
		else if (v->dcode != NULL)
		{
			const char **s = palloc0(Max(n, 1) * sizeof(char *));
			uint32	   *slen = palloc0(Max(n, 1) * sizeof(uint32));

			for (int r = 0; r < n; r++)
				if (!v->isnull[r])
				{
					s[r] = v->dict->bytes + v->dict->off[v->dcode[r]];
					slen[r] = v->dict->off[v->dcode[r] + 1] - v->dict->off[v->dcode[r]];
				}
			v->s = s;
			v->slen = slen;
			v->dcode = NULL;
			v->dict = NULL;
		}
		else
		{
			v->s = memcpy(palloc(Max(n, 1) * sizeof(char *)), v->s, n * sizeof(char *));
			v->slen = memcpy(palloc(Max(n, 1) * sizeof(uint32)), v->slen, n * sizeof(uint32));
		}
	}
	MemoryContextSwitchTo(old);
}

static void
put_cell(ColView *v, const SiftColDeltaPatch *p)
{
	int			r = p->row_ordinal;
	bool		isnull = (p->flags & SIFT_CDCOL_IS_NULL) != 0;

	if (v->keyp != NULL)
		((bool *) v->keyp)[r] = (p->flags & SIFT_CDCOL_KEY_ABSENT) == 0;
	if (v->isnull == NULL)
		return;
	((bool *) v->isnull)[r] = isnull;
	((uint8 *) v->tag)[r] = (p->flags & SIFT_CDCOL_HAS_TAG) != 0 ? p->tag : 0;
	Assert(SIFT_CDCOL_KIND(p->flags) == v->kind);
	if (v->kind == SIFT_CELL_INT)
		((int64 *) v->i)[r] = isnull ? 0 : p->val.i;
	else if (v->kind == SIFT_CELL_F8)
		((double *) v->f)[r] = isnull ? 0 : p->val.f;
	else
	{
		((const char **) v->s)[r] = isnull ? NULL : p->val.s;
		((uint32 *) v->slen)[r] = isnull ? 0 : p->vlen;
	}
}

void
sift_cdi_remap_tids(const SiftColDeltaGranMap *gm, uint64 *tids, int n)
{
	for (uint32 k = 0; k < gm->npatches; k++)
		if (gm->patches[k].attnum == SIFT_CD_ATT_TID_REMAP)
		{
			Assert(gm->patches[k].row_ordinal < n);
			tids[gm->patches[k].row_ordinal] = (uint64) gm->patches[k].val.i;
		}
}

void
sift_cdi_patch_granule(const SiftColDeltaGranMap *gm, int ncols, ColView *views, uint64 *tids, int n, MemoryContext cx)
{
	bool	   *copied = MemoryContextAllocZero(cx, Max(ncols, 1));

	sift_cdi_remap_tids(gm, tids, n);
	for (uint32 k = 0; k < gm->npatches; k++)
	{
		const SiftColDeltaPatch *p = &gm->patches[k];
		int			leaf = p->attnum - 1;

		Assert(p->row_ordinal < n);
		if (p->attnum != SIFT_CD_ATT_TID_REMAP && leaf < ncols && (views[leaf].isnull != NULL || views[leaf].keyp != NULL))
		{
			if (!copied[leaf])
				copy_view(&views[leaf], n, cx);
			copied[leaf] = true;
			put_cell(&views[leaf], p);
		}
	}
}

/* The leaf of a patch of a granule that a predicate reads and no patch of the granule before it names: the first of each, as the patches are ascending by leaf */
static bool
first_of_pred_leaf(const SiftColDeltaGranMap *gm, uint32 k, const bool *pred_leaf, int ncols)
{
	int			leaf = gm->patches[k].attnum - 1;

	return leaf >= 0 && leaf < ncols && pred_leaf[leaf] && (k == 0 || gm->patches[k - 1].attnum != gm->patches[k].attnum);
}

/* The exception of the k-th patch of a granule, whose leaf a predicate reads */
static SiftVariantExcSummary
patch_exc(const SiftColDeltaGranMap *gm, uint32 k)
{
	return (SiftVariantExcSummary) {.granule = gm->granule_id,.col = (uint16) (gm->patches[k].attnum - 1)};
}

static int
exc_cmp(const SiftVariantExcSummary *a, const SiftVariantExcSummary *b)
{
	return a->granule != b->granule ? (a->granule < b->granule ? -1 : 1) : (a->col < b->col ? -1 : a->col > b->col);
}

/* exc is ascending by (granule, col), and so are the granules of the map and the patches of a granule: one merge keeps the result ascending, an exception both name once */
const SiftVariantExcSummary *
sift_cdi_exceptions(const SiftColDeltaPartMap *map, const SiftVariantExcSummary *exc, int nexc, const bool *pred_leaf, int ncols, int *nout)
{
	SiftVariantExcSummary *out;
	int			extra = 0,
				n = 0,
				i = 0;

	*nout = nexc;
	if (map == NULL)
		return exc;
	for (uint32 gi = 0; gi < map->ndirty_granules; gi++)
		for (uint32 k = 0; k < map->grans[gi].npatches; k++)
			extra += first_of_pred_leaf(&map->grans[gi], k, pred_leaf, ncols);
	if (extra == 0)
		return exc;
	out = palloc((nexc + extra) * sizeof(SiftVariantExcSummary));
	for (uint32 gi = 0; gi < map->ndirty_granules; gi++)
		for (uint32 k = 0; k < map->grans[gi].npatches; k++)
		{
			SiftVariantExcSummary cd;
			int			c = 1;

			if (!first_of_pred_leaf(&map->grans[gi], k, pred_leaf, ncols))
				continue;
			cd = patch_exc(&map->grans[gi], k);
			while (i < nexc && (c = exc_cmp(&exc[i], &cd)) < 0)
				out[n++] = exc[i++];
			if (i < nexc && c == 0)
				out[n++] = exc[i++];
			else
				out[n++] = cd;
		}
	while (i < nexc)
		out[n++] = exc[i++];
	*nout = n;
	return out;
}
