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
cell_cmp(uint32 granule_a, const SiftColDeltaPatch *a, uint32 granule_b, const SiftColDeltaPatch *b)
{
	if (granule_a != granule_b)
		return granule_a < granule_b ? -1 : 1;
	return sift_col_delta_cell_cmp(a, b);
}

static int
collected_cmp(const void *a, const void *b)
{
	const Collected *x = a,
			   *y = b;
	int			c = cell_cmp(x->granule, &x->p, y->granule, &y->p);

	return c != 0 ? c : x->seq < y->seq ? -1 : x->seq > y->seq;
}

static bool
same_cell(const Collected *a, const Collected *b)
{
	return cell_cmp(a->granule, &a->p, b->granule, &b->p) == 0;
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
own_strings(SiftColDeltaPartMap *m)
{
	Size		total = 0,
				at = 0;

	for (uint32 i = 0; i < m->npatches; i++)
		total += is_string(&m->patch_pool[i]) ? m->patch_pool[i].vlen : 0;
	if (total == 0)
		return;
	m->strpool = palloc(total);
	for (uint32 i = 0; i < m->npatches; i++)
	{
		SiftColDeltaPatch *p = &m->patch_pool[i];

		if (!is_string(p))
			continue;
		memcpy(m->strpool + at, p->val.s, p->vlen);
		p->val.s = m->strpool + at;
		at += p->vlen;
	}
}

/* The pool of the map cut to its patches, the granules pointed at their place in it (they follow one another in it) */
static void
shrink_pool(SiftColDeltaPartMap *m)
{
	uint32		at = 0;

	m->patch_pool = repalloc(m->patch_pool, sizeof(SiftColDeltaPatch) * m->npatches);
	for (uint32 g = 0; g < m->ndirty_granules; at += m->grans[g++].npatches)
		m->grans[g].patches = m->patch_pool + at;
}

/* Where the patches of a map under construction go, in the order of (granule, attribute, row) */
static void
map_put(SiftColDeltaPartMap *m, uint32 granule, const SiftColDeltaPatch *p)
{
	SiftColDeltaGranMap *g;

	if (m->ndirty_granules == 0 || m->grans[m->ndirty_granules - 1].granule_id != granule)
	{
		g = &m->grans[m->ndirty_granules++];
		*g = (SiftColDeltaGranMap) {.granule_id = granule,.patches = &m->patch_pool[m->npatches]};
		m->granule_dirty_bits[granule / 64] |= UINT64CONST(1) << (granule % 64);
	}
	g = &m->grans[m->ndirty_granules - 1];
	m->patch_pool[m->npatches++] = *p;
	g->npatches++;
	g->attnum_bloom |= sift_cd_att_bit(p->attnum);
	m->dirty_attnum_bloom |= sift_cd_att_bit(p->attnum);
}

/* The patches of the newest layers of a map, granule by granule, that a merge takes in */
typedef struct OldCells
{
	const SiftColDeltaPartMap *m;
	int			nlayers;
	uint32		g;
	SiftColDeltaPatchIter it;
	const SiftColDeltaPatch *p;	/* the next, of granule m->grans[g]; NULL: none left */
} OldCells;

static void
old_next(OldCells *o)
{
	while ((o->p = sift_col_delta_patch_next(&o->it)) == NULL && ++o->g < o->m->ndirty_granules)
		sift_col_delta_patches_top(&o->it, &o->m->grans[o->g], o->nlayers);
}

static void
old_begin(OldCells *o, const SiftColDeltaPartMap *m, int nlayers)
{
	*o = (OldCells) {.m = m,.nlayers = nlayers};
	if (m == NULL || nlayers == 0 || m->ndirty_granules == 0)
		return;
	sift_col_delta_patches_top(&o->it, &m->grans[0], nlayers);
	old_next(o);
}

/* The own patches of the newest nlayers layers of the map */
static uint32
top_patches(const SiftColDeltaPartMap *m, int nlayers)
{
	uint32		n = 0;

	for (; m != NULL && nlayers-- > 0; m = m->under)
		n += m->npatches;
	return n;
}

/*
 * The map of the patches the newest nlayers layers of old show and of v (sorted, each cell once), the patches of v the newer; old may be NULL. in_place (no old): the patches are packed over v
 * itself, front to back (a patch is no longer than the Collected it is read from), and the map keeps that array, cut to them
 */
static SiftColDeltaPartMap *
map_of(const SiftColDeltaPartMap *old, int nlayers, Collected *v, int nv, uint64 part_gen, uint32 ngranules, uint64 tid_bloom, bool in_place)
{
	SiftColDeltaPartMap *m = palloc0(sizeof(SiftColDeltaPartMap));
	OldCells	o;
	uint32		nold = top_patches(old, nlayers);
	int			ni = 0,
				ngran_max = nv > 0;

	for (int i = 1; i < nv; i++)
		ngran_max += v[i].granule != v[i - 1].granule;
	if (nold > 0)
		ngran_max += old->ndirty_granules;
	m->nlayers = 1;
	m->part_gen = part_gen;
	m->ngranules = ngranules;
	m->dirty_tid_bloom = tid_bloom | (old != NULL ? old->dirty_tid_bloom : 0);
	m->granule_dirty_bits = palloc0(sizeof(uint64) * ((ngranules + 63) / 64));
	m->patch_pool = in_place ? (SiftColDeltaPatch *) v : palloc(sizeof(SiftColDeltaPatch) * Max(nold + nv, 1));
	m->grans = palloc(sizeof(SiftColDeltaGranMap) * Max(ngran_max, 1));
	for (old_begin(&o, old, nlayers); o.p != NULL || ni < nv;)
	{
		int			c = o.p == NULL ? 1 : ni >= nv ? -1 : cell_cmp(old->grans[o.g].granule_id, o.p, v[ni].granule, &v[ni].p);

		if (c < 0)
			map_put(m, old->grans[o.g].granule_id, o.p);
		else
		{
			SiftColDeltaPatch p = v[ni].p;

			map_put(m, v[ni].granule, &p);
			ni++;
		}
		if (c <= 0)
			old_next(&o);
	}
	if (in_place)
		shrink_pool(m);
	own_strings(m);
	return m;
}

/* The map laid over under, a shared map: its granules are those of both, each over the one of under it names, and under is held until the map goes */
static void
lay_over(SiftColDeltaPartMap *m, SiftColDeltaPartMap *under)
{
	SiftColDeltaGranMap *g = palloc(sizeof(SiftColDeltaGranMap) * Max(m->ndirty_granules + under->ndirty_granules, 1));
	uint32		i = 0,
				j = 0,
				n = 0;

	while (i < m->ndirty_granules || j < under->ndirty_granules)
	{
		const SiftColDeltaGranMap *u = j < under->ndirty_granules ? &under->grans[j] : NULL;
		bool		own = i < m->ndirty_granules && (u == NULL || m->grans[i].granule_id <= u->granule_id);

		g[n] = own ? m->grans[i++] : (SiftColDeltaGranMap) {.granule_id = u->granule_id};
		if (u != NULL && g[n].granule_id == u->granule_id)
		{
			g[n].under = u;
			g[n].attnum_bloom |= u->attnum_bloom;
			j++;
		}
		n++;
	}
	pfree(m->grans);
	m->grans = g;
	m->ndirty_granules = n;
	for (uint32 w = 0; w < (m->ngranules + 63) / 64; w++)
		m->granule_dirty_bits[w] |= under->granule_dirty_bits[w];
	m->dirty_attnum_bloom |= under->dirty_attnum_bloom;
	m->dirty_tid_bloom |= under->dirty_tid_bloom;
	m->under = sift_col_delta_part_map_ref(under);
	m->nlayers = under->nlayers + 1;
}

/*
 * The new layer takes in the newest layers of old while its patches are no fewer than those of the next one and the merge frees that one (nothing else holds it), or while the layers would pass the
 * bound: the layers grow from the newest to the oldest, so that a patch is copied into a new layer a number of times that grows as the log of the patches, and a layer another holder keeps is copied
 * only when the bound leaves no room
 */
SiftColDeltaPartMap *
sift_col_delta_part_map_merge(SiftColDeltaPartMap *old, bool old_goes, SiftColDeltaCollector *c, uint64 part_gen, uint32 ngranules)
{
	int			n = latest_only(c->v, c->n);
	SiftColDeltaPartMap *under = old;
	uint32		size = n;
	int			taken = 0;
	SiftColDeltaPartMap *m;

	for (bool frees = old_goes; under != NULL; under = under->under, taken++)
	{
		frees = frees && under->refcnt == 1;
		if (!(frees && size >= under->npatches) && under->nlayers < SIFT_COL_DELTA_MAP_LAYERS)
			break;
		size += under->npatches;
	}
	m = map_of(old, taken, c->v, n, part_gen, ngranules, c->tid_bloom, false);
	if (under != NULL)
		lay_over(m, under);
	return m;
}

SiftColDeltaPartMap *
sift_col_delta_part_map_take(SiftColDeltaCollector *c, uint64 part_gen, uint32 ngranules)
{
	SiftColDeltaPartMap *m;

	if (c->n == 0)
		return NULL;
	m = map_of(NULL, 0, c->v, latest_only(c->v, c->n), part_gen, ngranules, c->tid_bloom, true);
	c->v = NULL;
	c->n = c->cap = 0;
	return m;
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
		m = sift_col_delta_part_map_take(c, part_gen, ngranules);
	sift_col_delta_chain_free(chain);
	sift_col_delta_collector_free(c);
	return m;
}

void
sift_col_delta_part_map_free(SiftColDeltaPartMap *m)
{
	if (m == NULL)
		return;
	SIFT_INVARIANT(m->under == NULL, "a column delta part map that lies over a shared one was freed as one owner's");
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
	map->bytes = MemoryContextMemAllocated(map_cx, true) + (map->under != NULL ? map->under->bytes : 0);
	return map;
}

void
sift_col_delta_part_map_unref(SiftColDeltaPartMap *map)
{
	if (map == NULL)
		return;
	SIFT_INVARIANT(map->cx != NULL && map->refcnt > 0, "a column delta part map was given back more often than it was held");
	if (--map->refcnt == 0)
	{
		SiftColDeltaPartMap *under = map->under;

		MemoryContextDelete(map->cx);
		sift_col_delta_part_map_unref(under);
	}
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

/* The first of the n patches at or after (attnum, row) */
static const SiftColDeltaPatch *
first_not_before(const SiftColDeltaPatch *v, uint32 n, int16 attnum, int32 row)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		const SiftColDeltaPatch *p = &v[mid];

		if (p->attnum < attnum || (p->attnum == attnum && p->row_ordinal < row))
			lo = mid + 1;
		else
			hi = mid;
	}
	return v + lo;
}

/* The layers of the granule from the newest, at most nlayers, each the patches [lo(layer), hi(layer)) it has of the range */
static void
iter_init(SiftColDeltaPatchIter *it, const SiftColDeltaGranMap *gm, int nlayers, int16 attnum, bool all)
{
	it->n = 0;
	if (gm != NULL && !all && (gm->attnum_bloom & sift_cd_att_bit(attnum)) == 0)
		return;
	for (const SiftColDeltaGranMap *g = gm; g != NULL && nlayers-- > 0; g = g->under)
	{
		const SiftColDeltaPatch *lo = all ? g->patches : first_not_before(g->patches, g->npatches, attnum, 0);
		const SiftColDeltaPatch *hi = all ? g->patches + g->npatches : first_not_before(g->patches, g->npatches, attnum + 1, 0);

		if (lo == hi)
			continue;
		it->at[it->n] = lo;
		it->end[it->n++] = hi;
	}
}

void
sift_col_delta_patches_all(SiftColDeltaPatchIter *it, const SiftColDeltaGranMap *gm)
{
	iter_init(it, gm, SIFT_COL_DELTA_MAP_LAYERS, 0, true);
}

void
sift_col_delta_patches_top(SiftColDeltaPatchIter *it, const SiftColDeltaGranMap *gm, int nlayers)
{
	iter_init(it, gm, nlayers, 0, true);
}

void
sift_col_delta_patches_of(SiftColDeltaPatchIter *it, const SiftColDeltaGranMap *gm, int16 attnum)
{
	iter_init(it, gm, SIFT_COL_DELTA_MAP_LAYERS, attnum, false);
}

/* The patch of the cell among the n, NULL when none */
static const SiftColDeltaPatch *
patch_of_row(const SiftColDeltaPatch *v, uint32 n, int16 attnum, uint16 row)
{
	const SiftColDeltaPatch *at = first_not_before(v, n, attnum, row);

	return at < v + n && at->attnum == attnum && at->row_ordinal == row ? at : NULL;
}

const SiftColDeltaPatch *
sift_col_delta_find_row(const SiftColDeltaGranMap *gm, int16 attnum, uint16 row)
{
	const SiftColDeltaPatch *p;

	if (gm == NULL || (gm->attnum_bloom & sift_cd_att_bit(attnum)) == 0)
		return NULL;
	for (p = NULL; p == NULL && gm != NULL; gm = gm->under)
		p = patch_of_row(gm->patches, gm->npatches, attnum, row);
	return p;
}

static bool
patch_is_null(const SiftColDeltaPatch *p)
{
	return (p->flags & SIFT_CDCOL_IS_NULL) != 0;
}

int
sift_col_delta_apply_cells(SiftColDeltaCells *cells, const SiftColDeltaGranMap *gm, int16 attnum)
{
	SiftColDeltaPatchIter it;
	int			n = 0;

	sift_col_delta_patches_of(&it, gm, attnum);
	for (const SiftColDeltaPatch *p; (p = sift_col_delta_patch_next(&it)) != NULL; n++)
	{
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
	return n;
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
