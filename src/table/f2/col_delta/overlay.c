/*
 * table/f2/col_delta/overlay.c - what one snapshot sees of a chain of column delta pages, laid over the columns a scan decodes (table/f2/col_delta/col_delta.h).
 *
 * A row's patches are read oldest to newest and the newest the snapshot sees of a column wins. A patch never decides whether a row exists: the deletion vector and the pending
 * deletes do, and a reader that does not see the row never asks for its patches. No server call: the snapshot is SiftColDeltaOps.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "core/cachebudget.h"
#include "core/host.h"
#include "core/memtrack.h"
#include "table/f2/col_delta/batch_enc.h"
#include "table/f2/col_delta/col_delta.h"

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

#define COLLECT_STR_CHUNK	65536

typedef struct StrChunk
{
	struct StrChunk *next;
	Size		left;
	char	   *at;
} StrChunk;

struct SiftColDeltaCollector
{
	Collected  *v;
	int			n;
	int			cap;
	uint32		seq;
	uint64		tid_bloom;
	StrChunk   *strs;			/* the strings of the patches, copied off the page they were read from */
};

SiftColDeltaCollector *
sift_col_delta_collector_new(void)
{
	return palloc0(sizeof(SiftColDeltaCollector));
}

void
sift_col_delta_collector_free(SiftColDeltaCollector *c)
{
	while (c->strs != NULL)
	{
		StrChunk   *next = c->strs->next;

		pfree(c->strs);
		c->strs = next;
	}
	if (c->v != NULL)
		pfree(c->v);
	pfree(c);
}

int
sift_col_delta_collected(const SiftColDeltaCollector *c)
{
	return c->n;
}

static bool
is_string(const SiftColDeltaPatch *p)
{
	return SIFT_CDCOL_KIND(p->flags) == SIFT_CELL_STR && (p->flags & SIFT_CDCOL_IS_NULL) == 0;
}

static const char *
collector_keep(SiftColDeltaCollector *c, const char *s, Size len)
{
	char	   *dst;

	if (c->strs == NULL || c->strs->left < len)
	{
		Size		cap = Max((Size) COLLECT_STR_CHUNK, len);
		StrChunk   *k = palloc(sizeof(StrChunk) + cap);

		*k = (StrChunk) {.next = c->strs,.left = cap,.at = (char *) (k + 1)};
		c->strs = k;
	}
	dst = c->strs->at;
	memcpy(dst, s, len);
	c->strs->at += len;
	c->strs->left -= len;
	return dst;
}

static Collected *
collect_slot(SiftColDeltaCollector *c, const SiftColDeltaEntryHdr *h)
{
	Collected  *e;

	if (c->n == c->cap)
	{
		c->cap = Max(64, c->cap * 2);
		c->v = c->v != NULL ? repalloc(c->v, sizeof(Collected) * c->cap) : palloc(sizeof(Collected) * c->cap);
	}
	e = &c->v[c->n++];
	e->granule = h->granule_id;
	e->seq = c->seq;
	return e;
}

static void
collect_done(SiftColDeltaCollector *c, const SiftColDeltaEntryHdr *h)
{
	c->seq++;
	c->tid_bloom |= sift_cd_tid_bit(sift_col_delta_hdr_tid(h));
}

void
sift_col_delta_collect_record(SiftColDeltaCollector *c, const uint8 *rec, const SiftColDeltaEntryHdr *h)
{
	const uint8 *p = rec + sizeof(*h),
			   *end = rec + h->rec_bytes;

	for (int k = 0; k < h->ncols; k++)
	{
		Collected  *e = collect_slot(c, h);

		p = sift_col_delta_decode_col(p, end, &e->p);
		SIFT_INVARIANT(p != NULL, "a column delta page was read before it was checked");
		if (is_string(&e->p))
			e->p.val.s = collector_keep(c, e->p.val.s, e->p.vlen);
		e->p.row_ordinal = h->row_ordinal;
	}
	collect_done(c, h);
}

static void
collect_batch_row(SiftColDeltaCollector *c, const SiftColDeltaRecs *recs, const SiftColDeltaEntryHdr *h)
{
	for (int k = 0; k < h->ncols; k++)
		sift_col_delta_recs_patch(recs, k, &collect_slot(c, h)->p);
	collect_done(c, h);
}

/* The records of the page from byte from_used of its data (0: the first) to byte to_used */
static void
recs_of_range(SiftColDeltaRecs *recs, const char *page, uint32 from_used, uint32 to_used)
{
	sift_col_delta_recs_init_hdrs(recs, from_used > 0 ? (const uint8 *) SiftPageGetData(page) + from_used : sift_col_delta_page_first(page), (const uint8 *) SiftPageGetData(page) + to_used);
}

uint32
sift_col_delta_page_cut(const char *page, uint32 from_used, uint64 part_gen, bool (*keep) (void *ctx, TransactionId xmin), void *ctx)
{
	SiftColDeltaRecs recs;
	SiftColDeltaEntryHdr h;
	const uint8 *rec,
			   *start = NULL;

	if ((sift_col_delta_page_hdr(page)->part_gen_bloom & sift_cd_part_bit(part_gen)) == 0)
		return SiftPageGetUsed(page);
	recs_of_range(&recs, page, from_used, SiftPageGetUsed(page));
	for (const uint8 *at = recs.p; sift_col_delta_recs_next(&recs, &rec, &h); at = recs.left == 0 ? recs.p : start)
	{
		start = at;
		if (h.part_gen == part_gen && !keep(ctx, h.xmin))
			return (uint32) (start - (const uint8 *) SiftPageGetData(page));
	}
	return SiftPageGetUsed(page);
}

bool
sift_col_delta_collect_page(SiftColDeltaCollector *c, const char *page, uint32 from_used, uint32 to_used, uint64 part_gen, uint32 ngranules, const SiftColDeltaOps *ops, const char **why)
{
	SiftColDeltaRecs recs;
	SiftColDeltaEntryHdr h;
	const uint8 *rec;

	if ((sift_col_delta_page_hdr(page)->part_gen_bloom & sift_cd_part_bit(part_gen)) == 0)
		return true;
	recs_of_range(&recs, page, from_used, to_used);
	while (sift_col_delta_recs_next(&recs, &rec, &h))
	{
		if (h.part_gen == part_gen && ops->visible(ops->ctx, h.xmin, h.cid))
		{
			if (h.granule_id >= ngranules)
			{
				*why = "a column delta record names a granule the part does not have";
				return false;
			}
			if (rec != NULL)
				sift_col_delta_collect_record(c, rec, &h);
			else
				collect_batch_row(c, &recs, &h);
		}
	}
	return true;
}

static bool
collected_sorted(const Collected *v, int n)
{
	for (int i = 1; i < n; i++)
		if (collected_cmp(&v[i - 1], &v[i]) > 0)
			return false;
	return true;
}

/* The patches sorted and each (granule, attribute, row) once, the newest: the records of one statement come in the order of the rows it read, which is the order of the map */
static int
latest_only(Collected *v, int n)
{
	int			kept = 0;

	if (!collected_sorted(v, n))
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

/* The strings of the patches copied where the map holds them: what they pointed into (the pages of a chain, the pool of another map) can go */
static void
own_strings(SiftColDeltaPartMap *m, int n)
{
	Size		total = 0,
				at = 0;

	for (int i = 0; i < n; i++)
		total += is_string(&m->patch_pool[i]) ? m->patch_pool[i].vlen : 0;
	if (total == 0)
		return;
	m->strpool = palloc(total);
	for (int i = 0; i < n; i++)
	{
		SiftColDeltaPatch *p = &m->patch_pool[i];

		if (!is_string(p))
			continue;
		memcpy(m->strpool + at, p->val.s, p->vlen);
		p->val.s = m->strpool + at;
		at += p->vlen;
	}
}

/* Where the patches of a map under construction go, in the order of (granule, attribute, row) */
static void
map_put(SiftColDeltaPartMap *m, int *n, uint32 granule, const SiftColDeltaPatch *p)
{
	SiftColDeltaGranMap *g;

	if (m->ndirty_granules == 0 || m->grans[m->ndirty_granules - 1].granule_id != granule)
	{
		g = &m->grans[m->ndirty_granules++];
		*g = (SiftColDeltaGranMap) {.granule_id = granule,.patches = &m->patch_pool[*n]};
		m->granule_dirty_bits[granule / 64] |= UINT64CONST(1) << (granule % 64);
	}
	g = &m->grans[m->ndirty_granules - 1];
	m->patch_pool[(*n)++] = *p;
	g->npatches++;
	g->attnum_bloom |= sift_cd_att_bit(p->attnum);
	m->dirty_attnum_bloom |= sift_cd_att_bit(p->attnum);
}

static int
cell_cmp(uint32 granule_a, const SiftColDeltaPatch *a, uint32 granule_b, const SiftColDeltaPatch *b)
{
	if (granule_a != granule_b)
		return granule_a < granule_b ? -1 : 1;
	if (a->attnum != b->attnum)
		return a->attnum < b->attnum ? -1 : 1;
	return a->row_ordinal < b->row_ordinal ? -1 : a->row_ordinal > b->row_ordinal;
}

/* The map of the patches of old and of v (sorted, each cell once), the patches of v the newer; old may be NULL */
static SiftColDeltaPartMap *
map_of(const SiftColDeltaPartMap *old, const Collected *v, int nv, uint64 part_gen, uint32 ngranules, uint64 tid_bloom)
{
	SiftColDeltaPartMap *m = palloc0(sizeof(SiftColDeltaPartMap));
	uint32		og = 0,
				ok = 0;
	int			ni = 0,
				n = 0,
				nold = 0,
				ngran_max = nv > 0;

	for (int i = 1; i < nv; i++)
		ngran_max += v[i].granule != v[i - 1].granule;
	if (old != NULL)
	{
		ngran_max += old->ndirty_granules;
		for (uint32 g = 0; g < old->ndirty_granules; g++)
			nold += old->grans[g].npatches;
	}
	m->part_gen = part_gen;
	m->ngranules = ngranules;
	m->dirty_tid_bloom = tid_bloom | (old != NULL ? old->dirty_tid_bloom : 0);
	m->granule_dirty_bits = palloc0(sizeof(uint64) * ((ngranules + 63) / 64));
	m->patch_pool = palloc(sizeof(SiftColDeltaPatch) * Max(nold + nv, 1));
	m->grans = palloc(sizeof(SiftColDeltaGranMap) * Max(ngran_max, 1));
	for (;;)
	{
		const SiftColDeltaGranMap *g = old != NULL && og < old->ndirty_granules ? &old->grans[og] : NULL;
		int			c;

		if (g == NULL && ni >= nv)
			break;
		c = ni >= nv ? -1 : g == NULL ? 1 : cell_cmp(g->granule_id, &g->patches[ok], v[ni].granule, &v[ni].p);
		if (c < 0)
			map_put(m, &n, g->granule_id, &g->patches[ok]);
		else
		{
			map_put(m, &n, v[ni].granule, &v[ni].p);
			ni++;
		}
		if (g != NULL && c <= 0 && ++ok == g->npatches)
		{
			og++;
			ok = 0;
		}
	}
	own_strings(m, n);
	return m;
}

SiftColDeltaPartMap *
sift_col_delta_part_map_merge(const SiftColDeltaPartMap *old, SiftColDeltaCollector *c, uint64 part_gen, uint32 ngranules)
{
	int			n = latest_only(c->v, c->n);

	return map_of(old, c->v, n, part_gen, ngranules, c->tid_bloom);
}

SiftColDeltaPartMap *
sift_col_delta_build_part_map(SiftColDeltaChain *chain, uint64 part_gen, uint32 ngranules, const SiftColDeltaOps *ops, const char **why)
{
	SiftColDeltaCollector *c = sift_col_delta_collector_new();
	SiftColDeltaPartMap *m = NULL;
	bool		ok = true;

	*why = NULL;
	for (int i = 0; i < chain->npages && ok; i++)
		ok = sift_col_delta_collect_page(c, sift_col_delta_chain_page(chain, i), 0, SiftPageGetUsed(sift_col_delta_chain_page(chain, i)), part_gen, ngranules, ops, why);
	if (ok && c->n > 0)
		m = sift_col_delta_part_map_merge(NULL, c, part_gen, ngranules);
	sift_col_delta_chain_free(chain);
	sift_col_delta_collector_free(c);
	return m;
}

void
sift_col_delta_part_map_free(SiftColDeltaPartMap *m)
{
	if (m == NULL)
		return;
	if (m->strpool != NULL)
		pfree(m->strpool);
	pfree(m->granule_dirty_bits);
	pfree(m->patch_pool);
	pfree(m->grans);
	pfree(m);
}

static MemoryContext pm_root = NULL;

MemoryContext
sift_col_delta_part_map_context(MemoryContext cx)
{
	return sift_mem_allocset_create(cx, "sift cache f2 column delta part map");
}

/* The map is built in its context under cx, so that an error frees it; it moves under the cache when it is complete and takes its first reference */
SiftColDeltaPartMap *
sift_col_delta_part_map_share(MemoryContext map_cx, SiftColDeltaPartMap *map)
{
	if (map == NULL)
	{
		MemoryContextDelete(map_cx);
		return NULL;
	}
	if (pm_root == NULL)
		pm_root = sift_cache_create_context("sift cache f2 column delta part maps");
	MemoryContextSetParent(map_cx, pm_root);
	map->cx = map_cx;
	map->refcnt = 1;
	return map;
}

void
sift_col_delta_part_map_unref(SiftColDeltaPartMap *map)
{
	if (map == NULL)
		return;
	SIFT_INVARIANT(map->cx != NULL && map->refcnt > 0, "a column delta part map was given back more often than it was held");
	if (--map->refcnt == 0)
		MemoryContextDelete(map->cx);
}

static void
unref_callback(void *arg)
{
	sift_col_delta_part_map_unref(arg);
}

void
sift_col_delta_part_map_hold_in(MemoryContext cx, SiftColDeltaPartMap *map)
{
	MemoryContextCallback *cb = MemoryContextAlloc(cx, sizeof(*cb));

	cb->func = unref_callback;
	cb->arg = sift_col_delta_part_map_ref(map);
	MemoryContextRegisterResetCallback(cx, cb);
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

/* ---- one row ---- */

static bool
page_may_hold_row(const char *page, const SiftColDeltaTarget *t)
{
	const SiftColDeltaPageData *h = sift_col_delta_page_hdr(page);

	return (h->part_gen_bloom & sift_cd_part_bit(t->part_gen)) != 0 && (h->tid_bloom & sift_cd_tid_bit(t->tid)) != 0;
}

/*
 * The place in out of each patched column, by its number: a table past the last call's. The slots a call sets are put back at the start of the next, so that the cost of a call follows the
 * columns it meets, and a call that raised leaves nothing wrong behind.
 */
static int16 patch_slot[MaxTupleAttributeNumber + 1];
static int16 patch_set[MaxTupleAttributeNumber + 1];
static int	npatch_set;

int
sift_col_delta_fetch_row(const SiftColDeltaChain *chain, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops, SiftColDeltaPatch *out, int cap)
{
	int			n = 0;

	for (int i = 0; i < npatch_set; i++)
		patch_slot[patch_set[i]] = 0;
	npatch_set = 0;

	for (int i = 0; i < chain->npages; i++)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		SiftColDeltaRecs recs;
		SiftColDeltaEntryHdr h;
		const uint8 *rec;

		if (!page_may_hold_row(page, t))
			continue;
		sift_col_delta_recs_init(&recs, sift_col_delta_page_first(page), sift_col_delta_page_end(page));
		while (sift_col_delta_recs_next(&recs, &rec, &h))
		{
			const uint8 *c = rec + sizeof(h),
					   *rend = rec + h.rec_bytes;

			if (!sift_col_delta_rec_is_of_row(&h, t) || !ops->visible(ops->ctx, h.xmin, h.cid))
				continue;
			for (int k = 0; k < h.ncols; k++)
			{
				SiftColDeltaPatch col;
				int			at;

				c = sift_col_delta_decode_col(c, rend, &col);
				SIFT_INVARIANT(c != NULL, "a column delta page was read before it was checked");
				SIFT_INVARIANT(col.attnum >= 0 && col.attnum <= MaxTupleAttributeNumber, "a column delta patch names column %d", col.attnum);
				col.row_ordinal = t->row_ordinal;
				if (patch_slot[col.attnum] == 0)
				{
					SIFT_INVARIANT(n < cap, "a row has more patched columns than its table has");
					patch_slot[col.attnum] = (int16) ++n;
					patch_set[npatch_set++] = col.attnum;
				}
				at = patch_slot[col.attnum] - 1;
				out[at] = col;
			}
		}
	}
	return n;
}

SiftColDeltaLast
sift_col_delta_latest_for_row(const SiftColDeltaChain *chain, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops)
{
	SiftColDeltaLast last = {0};

	for (int i = chain->npages - 1; i >= 0 && !last.found; i--)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		SiftColDeltaRecs recs;
		SiftColDeltaEntryHdr h;
		const uint8 *rec;

		if (!page_may_hold_row(page, t))
			continue;
		sift_col_delta_recs_init_hdrs(&recs, sift_col_delta_page_first(page), sift_col_delta_page_end(page));
		while (sift_col_delta_recs_next(&recs, &rec, &h))
		{
			if (sift_col_delta_rec_is_of_row(&h, t) && !ops->is_aborted(ops->ctx, h.xmin))
				last = (SiftColDeltaLast) {.found = true,.xid = h.xmin,.cid = h.cid};
		}
	}
	return last;
}
