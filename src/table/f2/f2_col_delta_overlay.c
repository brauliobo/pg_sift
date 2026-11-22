/*
 * table/f2/f2_col_delta_overlay.c - what one snapshot sees of a chain of column delta pages, laid over the columns a scan decodes (table/f2/f2_col_delta.h).
 *
 * A row's patches are read oldest to newest and the newest the snapshot sees of a column wins. A patch never decides whether a row exists: the deletion vector and the pending
 * deletes do, and a reader that does not see the row never asks for its patches. No server call: the snapshot is SiftColDeltaOps.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "table/f2/f2_col_delta.h"

/* ---- the map of a part ---- */

typedef struct Collected
{
	uint32		granule;
	uint32		seq;
	SiftColDeltaPatch p;
} Collected;

static int
collected_cmp(const void *a, const void *b)
{
	const Collected *x = a,
			   *y = b;

	if (x->granule != y->granule)
		return x->granule < y->granule ? -1 : 1;
	if (x->p.attnum != y->p.attnum)
		return x->p.attnum < y->p.attnum ? -1 : 1;
	if (x->p.row_ordinal != y->p.row_ordinal)
		return x->p.row_ordinal < y->p.row_ordinal ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static bool
same_cell(const Collected *a, const Collected *b)
{
	return a->granule == b->granule && a->p.attnum == b->p.attnum && a->p.row_ordinal == b->p.row_ordinal;
}

typedef struct Collector
{
	Collected  *v;
	int			n;
	int			cap;
	uint32		seq;
	uint64		tid_bloom;
} Collector;

static void
collect_record(Collector *c, const uint8 *rec, const SiftColDeltaEntryHdr *h)
{
	const uint8 *p = rec + sizeof(*h),
			   *end = rec + h->rec_bytes;

	for (int k = 0; k < h->ncols; k++)
	{
		Collected  *e;

		if (c->n == c->cap)
		{
			c->cap = Max(64, c->cap * 2);
			c->v = c->v != NULL ? repalloc(c->v, sizeof(Collected) * c->cap) : palloc(sizeof(Collected) * c->cap);
		}
		e = &c->v[c->n++];
		p = sift_col_delta_decode_col(p, end, &e->p);
		SIFT_INVARIANT(p != NULL, "a column delta page was read before it was checked");
		e->p.row_ordinal = h->row_ordinal;
		e->granule = h->granule_id;
		e->seq = c->seq;
	}
	c->seq++;
	c->tid_bloom |= sift_cd_tid_bit(sift_col_delta_hdr_tid(h));
}

/* Every record of the part the snapshot sees, oldest first; false (why set) when one names a granule the part does not have */
static bool
collect_part(Collector *c, const SiftColDeltaChain *chain, uint64 part_gen, uint32 ngranules, const SiftColDeltaOps *ops, const char **why)
{
	for (int i = 0; i < chain->npages; i++)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		const uint8 *p = sift_col_delta_page_first(page),
				   *end = sift_col_delta_page_end(page);

		if ((sift_col_delta_page_hdr(page)->part_gen_bloom & sift_cd_part_bit(part_gen)) == 0)
			continue;
		while (p < end)
		{
			SiftColDeltaEntryHdr h;

			memcpy(&h, p, sizeof(h));
			if (h.part_gen == part_gen && ops->visible(ops->ctx, h.xmin, h.cid))
			{
				if (h.granule_id >= ngranules)
				{
					*why = "a column delta record names a granule the part does not have";
					return false;
				}
				collect_record(c, p, &h);
			}
			p += h.rec_bytes;
		}
	}
	return true;
}

/* The patches sorted and each (granule, attribute, row) once, the newest */
static int
latest_only(Collected *v, int n)
{
	int			kept = 0;

	qsort(v, n, sizeof(Collected), collected_cmp);
	for (int i = 0; i < n; i++)
	{
		if (kept > 0 && same_cell(&v[kept - 1], &v[i]))
			v[kept - 1] = v[i];
		else
			v[kept++] = v[i];
	}
	return kept;
}

static SiftColDeltaPartMap *
make_map(const Collected *v, int n, uint64 part_gen, uint32 ngranules, uint64 tid_bloom, char *arena)
{
	SiftColDeltaPartMap *m = palloc0(sizeof(SiftColDeltaPartMap));
	int			ndirty = 0;

	m->part_gen = part_gen;
	m->ngranules = ngranules;
	m->dirty_tid_bloom = tid_bloom;
	m->page_arena = arena;
	m->granule_dirty_bits = palloc0(sizeof(uint64) * ((ngranules + 63) / 64));
	m->patch_pool = palloc(sizeof(SiftColDeltaPatch) * n);
	for (int i = 0; i < n; i++)
		ndirty += i == 0 || v[i].granule != v[i - 1].granule;
	m->grans = palloc(sizeof(SiftColDeltaGranMap) * ndirty);
	for (int i = 0; i < n; i++)
	{
		SiftColDeltaGranMap *g;

		if (i == 0 || v[i].granule != v[i - 1].granule)
		{
			g = &m->grans[m->ndirty_granules++];
			*g = (SiftColDeltaGranMap) {.granule_id = v[i].granule,.patches = &m->patch_pool[i]};
			m->granule_dirty_bits[v[i].granule / 64] |= UINT64CONST(1) << (v[i].granule % 64);
		}
		g = &m->grans[m->ndirty_granules - 1];
		m->patch_pool[i] = v[i].p;
		g->npatches++;
		g->attnum_bloom |= sift_cd_att_bit(v[i].p.attnum);
		m->dirty_attnum_bloom |= sift_cd_att_bit(v[i].p.attnum);
	}
	return m;
}

SiftColDeltaPartMap *
sift_col_delta_build_part_map(SiftColDeltaChain *chain, uint64 part_gen, uint32 ngranules, const SiftColDeltaOps *ops, const char **why)
{
	Collector	c = {0};
	SiftColDeltaPartMap *m = NULL;
	char	   *arena = chain->arena;
	int			n = 0;

	*why = NULL;
	if (collect_part(&c, chain, part_gen, ngranules, ops, why))
		n = latest_only(c.v, c.n);
	chain->arena = NULL;
	sift_col_delta_chain_free(chain);
	if (n > 0)
		m = make_map(c.v, n, part_gen, ngranules, c.tid_bloom, arena);
	else
		pfree(arena);
	if (c.v != NULL)
		pfree(c.v);
	return m;
}

void
sift_col_delta_part_map_free(SiftColDeltaPartMap *m)
{
	if (m == NULL)
		return;
	pfree(m->page_arena);
	pfree(m->granule_dirty_bits);
	pfree(m->patch_pool);
	pfree(m->grans);
	pfree(m);
}

const SiftColDeltaGranMap *
sift_col_delta_gran_map(const SiftColDeltaPartMap *m, uint32 granule_id)
{
	uint32		lo = 0,
				hi = m->ndirty_granules;

	if (granule_id >= m->ngranules || (m->granule_dirty_bits[granule_id / 64] & (UINT64CONST(1) << (granule_id % 64))) == 0)
		return NULL;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (m->grans[mid].granule_id < granule_id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return &m->grans[lo];
}

/* ---- the patches of a column ---- */

static uint32
first_not_before(const SiftColDeltaGranMap *gm, int16 attnum, int32 row)
{
	uint32		lo = 0,
				hi = gm->npatches;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		const SiftColDeltaPatch *p = &gm->patches[mid];

		if (p->attnum < attnum || (p->attnum == attnum && p->row_ordinal < row))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

void
sift_col_delta_patch_slice(const SiftColDeltaGranMap *gm, int16 attnum, uint32 *lo, uint32 *hi)
{
	*lo = *hi = 0;
	if (gm == NULL || (gm->attnum_bloom & sift_cd_att_bit(attnum)) == 0)
		return;
	*lo = first_not_before(gm, attnum, 0);
	*hi = first_not_before(gm, attnum + 1, 0);
}

const SiftColDeltaPatch *
sift_col_delta_find_row(const SiftColDeltaGranMap *gm, int16 attnum, uint16 row)
{
	uint32		at;

	if (gm == NULL || (gm->attnum_bloom & sift_cd_att_bit(attnum)) == 0)
		return NULL;
	at = first_not_before(gm, attnum, row);
	return at < gm->npatches && gm->patches[at].attnum == attnum && gm->patches[at].row_ordinal == row ? &gm->patches[at] : NULL;
}

static bool
patch_is_null(const SiftColDeltaPatch *p)
{
	return (p->flags & SIFT_CDCOL_IS_NULL) != 0;
}

int
sift_col_delta_apply_cells(SiftColDeltaCells *cells, const SiftColDeltaGranMap *gm, int16 attnum)
{
	uint32		lo,
				hi;

	sift_col_delta_patch_slice(gm, attnum, &lo, &hi);
	for (uint32 k = lo; k < hi; k++)
	{
		const SiftColDeltaPatch *p = &gm->patches[k];
		uint16		r = p->row_ordinal;

		SIFT_INVARIANT(SIFT_CDCOL_KIND(p->flags) == cells->kind, "a column delta of attribute %d is of another kind than its column", attnum);
		cells->isnull[r] = patch_is_null(p);
		if (cells->tag != NULL)
			cells->tag[r] = 0;
		if (cells->kind == SIFT_CELL_INT)
			cells->i[r] = patch_is_null(p) ? 0 : p->val.i;
		else if (cells->kind == SIFT_CELL_F8)
			cells->f[r] = patch_is_null(p) ? 0 : p->val.f;
		else
		{
			char	   *copy = NULL;

			if (!patch_is_null(p))
			{
				copy = MemoryContextAlloc(cells->cx, (Size) p->vlen + 1);
				memcpy(copy, p->val.s, p->vlen);
				copy[p->vlen] = '\0';
			}
			cells->s[r] = (SiftStrCell) {.p = copy,.len = patch_is_null(p) ? 0 : p->vlen,.isnull = patch_is_null(p)};
		}
	}
	return (int) (hi - lo);
}

static void *
copy_of(MemoryContext cx, const void *src, Size n)
{
	void	   *dst = MemoryContextAllocZero(cx, Max(n, 1));

	if (src != NULL)
		memcpy(dst, src, n);
	return dst;
}

void
sift_col_delta_patch_colview(ColView *view, uint16 nrows, const SiftColDeltaGranMap *gm, int16 attnum, MemoryContext cx)
{
	uint32		lo,
				hi;
	bool	   *isnull;
	uint8	   *tag = NULL;
	int64	   *i = NULL;
	double	   *f = NULL;
	const char **s = NULL;
	uint32	   *slen = NULL;

	sift_col_delta_patch_slice(gm, attnum, &lo, &hi);
	if (lo == hi)
		return;
	isnull = copy_of(cx, view->isnull, sizeof(bool) * nrows);
	if (view->tag != NULL)
		tag = copy_of(cx, view->tag, nrows);
	if (view->kind == SIFT_CELL_INT)
		i = copy_of(cx, view->i, sizeof(int64) * nrows);
	else if (view->kind == SIFT_CELL_F8)
		f = copy_of(cx, view->f, sizeof(double) * nrows);
	else
	{
		s = copy_of(cx, view->s, sizeof(char *) * nrows);
		slen = copy_of(cx, view->slen, sizeof(uint32) * nrows);
	}
	for (uint32 k = lo; k < hi; k++)
	{
		const SiftColDeltaPatch *p = &gm->patches[k];
		uint16		r = p->row_ordinal;

		SIFT_INVARIANT(SIFT_CDCOL_KIND(p->flags) == view->kind, "a column delta of attribute %d is of another kind than its column", attnum);
		isnull[r] = patch_is_null(p);
		if (tag != NULL)
			tag[r] = 0;
		if (i != NULL)
			i[r] = patch_is_null(p) ? 0 : p->val.i;
		else if (f != NULL)
			f[r] = patch_is_null(p) ? 0 : p->val.f;
		else
		{
			s[r] = patch_is_null(p) ? NULL : p->val.s;
			slen[r] = patch_is_null(p) ? 0 : p->vlen;
		}
	}
	view->isnull = isnull;
	view->tag = tag;
	view->i = i;
	view->f = f;
	view->s = s;
	view->slen = slen;
}

SiftColDeltaGranMap *
sift_col_delta_gran_copy(const SiftColDeltaGranMap *gm, MemoryContext cx)
{
	Size		strings = 0;
	SiftColDeltaGranMap *c;
	char	   *tail;

	for (uint32 k = 0; k < gm->npatches; k++)
		if (SIFT_CDCOL_KIND(gm->patches[k].flags) == SIFT_CELL_STR && !patch_is_null(&gm->patches[k]))
			strings += gm->patches[k].vlen;
	c = MemoryContextAlloc(cx, sizeof(*c) + sizeof(SiftColDeltaPatch) * gm->npatches + strings);
	*c = *gm;
	c->patches = (SiftColDeltaPatch *) (c + 1);
	memcpy(c->patches, gm->patches, sizeof(SiftColDeltaPatch) * gm->npatches);
	tail = (char *) (c->patches + gm->npatches);
	for (uint32 k = 0; k < gm->npatches; k++)
		if (SIFT_CDCOL_KIND(c->patches[k].flags) == SIFT_CELL_STR && !patch_is_null(&c->patches[k]))
		{
			memcpy(tail, gm->patches[k].val.s, c->patches[k].vlen);
			c->patches[k].val.s = tail;
			tail += c->patches[k].vlen;
		}
	return c;
}

/* ---- one row ---- */

static bool
record_is_of_row(const SiftColDeltaEntryHdr *h, const SiftColDeltaTarget *t)
{
	return h->part_gen == t->part_gen && h->granule_id == t->granule_id && h->row_ordinal == t->row_ordinal && sift_col_delta_hdr_tid(h) == t->tid;
}

static bool
page_may_hold_row(const char *page, const SiftColDeltaTarget *t)
{
	const SiftColDeltaPageData *h = sift_col_delta_page_hdr(page);

	return (h->part_gen_bloom & sift_cd_part_bit(t->part_gen)) != 0 && (h->tid_bloom & sift_cd_tid_bit(t->tid)) != 0;
}

int
sift_col_delta_fetch_row(const SiftColDeltaChain *chain, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops, SiftColDeltaPatch *out, int cap)
{
	int			n = 0;

	for (int i = 0; i < chain->npages; i++)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		const uint8 *p = sift_col_delta_page_first(page),
				   *end = sift_col_delta_page_end(page);

		if (!page_may_hold_row(page, t))
			continue;
		while (p < end)
		{
			SiftColDeltaEntryHdr h;
			const uint8 *c = p + sizeof(h),
					   *rend;

			memcpy(&h, p, sizeof(h));
			rend = p + h.rec_bytes;
			p = rend;
			if (!record_is_of_row(&h, t) || !ops->visible(ops->ctx, h.xmin, h.cid))
				continue;
			for (int k = 0; k < h.ncols; k++)
			{
				SiftColDeltaPatch col;
				int			at = 0;

				c = sift_col_delta_decode_col(c, rend, &col);
				SIFT_INVARIANT(c != NULL, "a column delta page was read before it was checked");
				col.row_ordinal = t->row_ordinal;
				while (at < n && out[at].attnum != col.attnum)
					at++;
				SIFT_INVARIANT(at < cap, "a row has more patched columns than its table has");
				out[at] = col;
				n = Max(n, at + 1);
			}
		}
	}
	return n;
}

SiftColDeltaLast
sift_col_delta_latest_for_row(const SiftColDeltaChain *chain, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops)
{
	SiftColDeltaLast last = {0};

	for (int i = 0; i < chain->npages; i++)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		const uint8 *p = sift_col_delta_page_first(page),
				   *end = sift_col_delta_page_end(page);

		if (!page_may_hold_row(page, t))
			continue;
		while (p < end)
		{
			SiftColDeltaEntryHdr h;

			memcpy(&h, p, sizeof(h));
			p += h.rec_bytes;
			if (record_is_of_row(&h, t) && !ops->is_aborted(ops->ctx, h.xmin))
				last = (SiftColDeltaLast) {.found = true,.xid = h.xmin,.cid = h.cid};
		}
	}
	return last;
}

