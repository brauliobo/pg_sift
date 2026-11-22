/*
 * col/f2/f2_scope_over.c - what the summary of a scope lacks, or counts too many, for a snapshot. The rows of the parts it does not name and of the slabs are added to the cube of each key their
 * member belongs to; the rows of the parts it names that the snapshot does not see (a deletion vector, a pending delete, an inserter that is not settled) are taken out of it. Both are rows read
 * once, each granule that holds some decoded once for every key, and kept with the keys their members belong to, so that a key's cube of them is a read of its own rows.
 * A row is seen by the rules of sift_part_count_visible: its inserter (the version vector, the slab batch), the deletion vector of its part and the pending deletes of its partition.
 */
/* check-memory: allow-allocs the rows kept are the overlay of one snapshot, which sift_f2s_overlay_open and gone_open bound */
#include "postgres.h"

#include "col/f2/f2_scope.h"
#include "col/f2/l0_slab.h"
#include "col/f2/part_vis.h"
#include "core/cmp.h"
#include "core/memtrack.h"

#include "col/f2/f2_scope_int.h"

typedef struct AddedLink
{
	int64		key;
	uint32		row;
} AddedLink;

struct SiftF2ScopeRowSet
{
	uint32		n;
	uint32		cap;
	int64	   *member;
	uint8	   *member_null;
	int64	   *time;
	uint8	   *time_null;
	int64	   *dim[SIFT_SCOPE_DIMS_CAP];
	int64	   *meas[SIFT_SCOPE_MEAS_CAP];
	uint8	   *meas_null[SIFT_SCOPE_MEAS_CAP];
	SiftScopeRows rows;
	uint32		nlinks;
	AddedLink  *link;
	const SiftF2ScopeCfg *sc;
	SiftF2ScopeParts *owner;
	struct GoneGranule *gran;	/* the granules the rows were read from, ascending by first row; their cells but the member are read when a key asks for one of their rows (gone_fill) */
	uint32		ngran;
};

typedef struct GoneGranule
{
	SiftF2ScopeSlot *s;
	uint32		g;
	uint32		first;
	uint32		n;
	uint16	   *rows;
	bool		filled;
} GoneGranule;

static SiftTidIndexEntry *
slot_tix(SiftF2ScopeParts *p, SiftF2ScopeSlot *s)
{
	if (s->tix == NULL)
	{
		SiftPart   *part = sift_f2s_slot_part(p, s);
		uint64		len;
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->tix = sift_part_read_section(p->rel, part, SEC_TID_INDEX, &len);
		MemoryContextSwitchTo(old);
		if (s->tix == NULL || len != (uint64) part->hdr.ngranules * sizeof(SiftTidIndexEntry))
			SIFT_SEG_CORRUPT(p->rel, part->hdr.part_gen, "The part has no usable TID index.");
	}
	return s->tix;
}

uint32
sift_f2s_unseen_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, uint16 *rows, uint32 n)
{
	const SiftGranuleDesc *dir;
	SiftPart   *part;
	const FullTransactionId xmin = sift_vis_snap_xmin(p->snap);
	uint64	   *tids = NULL;
	SiftRowVer *vers = NULL;
	uint16	   *own,
			   *merged;
	uint32		nown = 0;

	if (!s->unseen)
		return n;
	dir = sift_f2s_slot_gdir(p, s);
	part = sift_f2s_slot_part(p, s);
	if (s->px != NULL && sift_pdel_idx_may_hit_granule(s->px, &slot_tix(p, s)[g]))
		tids = sift_part_read_tids(p->rel, part, dir, g);
	if (!sift_vis_granule_inserts_settled(&s->d, &dir[g], xmin))
		vers = sift_part_read_vers(p->rel, part, dir, g);
	if (tids == NULL && vers == NULL)
		return n;
	own = palloc(Max(dir[g].nrows, 1) * sizeof(uint16));
	for (uint32 r = 0; r < dir[g].nrows; r++)
		if ((tids != NULL && sift_pdel_idx_has(s->px, tids[r])) || (vers != NULL && !sift_vis_inserted_cached(&p->xc, vers[r].xmin, vers[r].cmin)))
			own[nown++] = (uint16) r;
	merged = palloc(SIFT_GRANULE_ROWS * sizeof(uint16));
	n = sift_scope_excl_union(rows, n, own, nown, merged, SIFT_GRANULE_ROWS);
	memcpy(rows, merged, n * sizeof(uint16));
	pfree(own);
	pfree(merged);
	return n;
}

static struct SiftF2ScopeRowSet *
rowset_new(const SiftF2ScopeCfg *sc, uint32 cap)
{
	struct SiftF2ScopeRowSet *a = palloc0(sizeof(*a));

	a->sc = sc;
	a->cap = cap;
	a->member = palloc(cap * sizeof(int64));
	a->member_null = palloc(cap);
	if (sc->time_leaf >= 0)
	{
		a->time = palloc(cap * sizeof(int64));
		a->time_null = palloc(cap);
	}
	for (int d = 0; d < sc->shape.ndims; d++)
		a->dim[d] = palloc(cap * sizeof(int64));
	for (int m = 0; m < sc->shape.nmeas; m++)
	{
		a->meas[m] = palloc(cap * sizeof(int64));
		a->meas_null[m] = palloc(cap);
	}
	return a;
}

static void
rowset_set_member(struct SiftF2ScopeRowSet *a, uint32 k, const SiftScopeRows *r, uint32 i)
{
	a->member[k] = r->member[i];
	a->member_null[k] = r->member_null[i];
}

static void
rowset_set_cells(struct SiftF2ScopeRowSet *a, uint32 k, const SiftScopeRows *r, uint32 i)
{
	if (a->time != NULL)
	{
		a->time[k] = r->time[i];
		a->time_null[k] = r->time_null[i];
	}
	for (int d = 0; d < a->sc->shape.ndims; d++)
		a->dim[d][k] = r->dim[d][i];
	for (int m = 0; m < a->sc->shape.nmeas; m++)
	{
		a->meas[m][k] = r->meas[m][i];
		a->meas_null[m][k] = r->meas_null[m][i];
	}
}

/* Row i of a decoded granule, after the rows of a (room for it made) */
static void
rowset_append(struct SiftF2ScopeRowSet *a, const SiftScopeRows *r, uint32 i)
{
	rowset_set_member(a, a->n, r, i);
	rowset_set_cells(a, a->n++, r, i);
}

/* The rows of a decoded granule but those in gone (ascending) */
static void
added_granule(struct SiftF2ScopeRowSet *a, const SiftScopeRows *r, const uint16 *gone, uint32 ngone)
{
	uint32		at = 0;

	for (uint32 i = 0; i < r->n; i++)
	{
		if (at < ngone && gone[at] == i)
			at++;
		else
			rowset_append(a, r, i);
	}
}

static void
added_slab(struct SiftF2ScopeRowSet *a, SiftF2ScopeParts *p, const SiftRowBuf *rb, const SiftPendDelIndex *px)
{
	const SiftF2ScopeCfg *sc = a->sc;

	for (int r = 0; r < rb->n; r++)
	{
		const uint32 k = a->n;

		if (!sift_vis_inserted_cached(&p->xc, rb->xmin[r], rb->cmin[r]) || (px != NULL && sift_pdel_idx_has(px, rb->tid[r])) ||
			(p->excl_leaf >= 0 && !rb->isnull[p->excl_leaf][r]))
			continue;
		a->member[k] = rb->i[sc->member_leaf][r];
		a->member_null[k] = rb->isnull[sc->member_leaf][r];
		if (a->time != NULL)
		{
			a->time[k] = rb->i[sc->time_leaf][r] * sc->time_unit;
			a->time_null[k] = rb->isnull[sc->time_leaf][r];
		}
		for (int d = 0; d < sc->shape.ndims; d++)
			a->dim[d][k] = rb->isnull[sc->dim_leaf[d]][r] ? -1 : rb->i[sc->dim_leaf[d]][r];
		for (int m = 0; m < sc->shape.nmeas; m++)
		{
			const int	leaf = sc->meas_leaf[m];

			if (sc->shape.float_meas >> m & 1)
				memcpy(&a->meas[m][k], &rb->f[leaf][r], sizeof(double));
			else
				a->meas[m][k] = rb->i[leaf][r];
			a->meas_null[m][k] = rb->isnull[leaf][r];
		}
		a->n++;
	}
}

static int
link_order(const void *a, const void *b)
{
	const AddedLink *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->row < y->row ? -1 : x->row > y->row;
}

/* Each row to the keys its member belongs to, by key */
static void
rowset_link(struct SiftF2ScopeRowSet *a, SiftScopePairMap *pm)
{
	uint32		total = 0;

	for (uint32 i = 0; i < a->n; i++)
	{
		uint32		lo,
					hi;

		if (!a->member_null[i] && sift_spm_find(pm, a->member[i], &lo, &hi))
			total += hi - lo;
	}
	a->link = palloc(Max(total, 1) * sizeof(AddedLink));
	for (uint32 i = 0; i < a->n; i++)
	{
		uint32		lo,
					hi;

		if (!a->member_null[i] && sift_spm_find(pm, a->member[i], &lo, &hi))
			for (uint32 q = lo; q < hi; q++)
				a->link[a->nlinks++] = (AddedLink) {pm->scope_keys[pm->key_ordinals[q]], i};
	}
	qsort(a->link, a->nlinks, sizeof(AddedLink), link_order);
	a->rows = (SiftScopeRows) {.n = a->n,.member = a->member,.member_null = a->member_null,.time = a->time,.time_null = a->time_null};
	for (int d = 0; d < a->sc->shape.ndims; d++)
		a->rows.dim[d] = a->dim[d];
	for (int m = 0; m < a->sc->shape.nmeas; m++)
	{
		a->rows.meas[m] = a->meas[m];
		a->rows.meas_null[m] = a->meas_null[m];
	}
}

/* The first link of the key */
static uint32
link_first(const struct SiftF2ScopeRowSet *a, int64 key)
{
	uint32		lo = 0,
				hi = a->nlinks;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (a->link[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void gone_fill(struct SiftF2ScopeRowSet *a, uint32 row);

static bool
rowset_cube(struct SiftF2ScopeRowSet *a, int64 scope_key, int32 month, SiftScopeCubeAcc *out)
{
	sift_scube_init(out, a->sc->shape);
	for (uint32 i = link_first(a, scope_key); i < a->nlinks && a->link[i].key == scope_key; i++)
	{
		const uint32 row = a->link[i].row;

		if (a->gran != NULL)
			gone_fill(a, row);

		if (month == SIFT_SCOPE_NO_MONTH || (a->time != NULL && !a->time_null[row] && sift_scope_month_of(a->time[row]) == month))
			sift_scube_add_row(out, &a->rows, row, NULL);
	}
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

bool
sift_f2s_added_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out)
{
	const SiftF2ScopeParts *p = arg;

	return rowset_cube(p->added, scope_key, month, out);
}

uint32
sift_f2s_added_months(void *arg, int64 scope_key, const int32 **months)
{
	const SiftF2ScopeParts *p = arg;
	const struct SiftF2ScopeRowSet *a = p->added;
	const uint32 first = link_first(a, scope_key);
	int32	   *all = palloc(Max(a->nlinks - first, 1) * sizeof(int32));
	uint32		n = 0;

	for (uint32 i = first; a->time != NULL && i < a->nlinks && a->link[i].key == scope_key; i++)
		if (!a->time_null[a->link[i].row])
			all[n++] = sift_scope_month_of(a->time[a->link[i].row]);
	qsort(all, n, sizeof(int32), sift_cmp_i32);
	*months = all;
	return n;
}

/* The rows of the overlay a read is worth: a tenth of those of the parts the summary names, within bounds */
static uint64
overlay_cap(const SiftF2ScopeParts *p)
{
	uint64		object_rows = 0;

	for (int i = 0; i < p->n; i++)
		object_rows += p->s[i].d.nrows;
	return Min(Max(object_rows / 100 * SIFT_SCOPE_OVERLAY_PCT, SIFT_SCOPE_OVERLAY_MIN_ROWS), SIFT_SCOPE_OVERLAY_MAX_ROWS);
}

/* The partitions of the manifest the snapshot sees: the pending deletes of each (set on its parts) and its slab, read; false when they are more than a read is worth */
static bool
load_partitions(SiftF2ScopeParts *p, const SiftManifestRead *mr, SiftRowBuf **slab, const SiftPendDelIndex **px, uint64 max_rows)
{
	const FullTransactionId xmin = sift_vis_snap_xmin(p->snap);
	const SiftF2Rel *f = sift_f2_rel(p->rel);
	uint64		npend = 0,
				rows = 0;

	for (int k = 0; k < mr->n; k++)
	{
		const SiftPartitionEntry *e = &mr->entries[k];
		int			ndead = 0;
		uint64	   *dead = sift_f2_pending_dead(p->rel, e, p->snap, &ndead);

		npend += ndead;
		px[k] = ndead > 0 ? sift_pdel_idx_build(dead, ndead) : NULL;
		for (int i = 0; i < p->n + p->nadd; i++)
		{
			SiftF2ScopeSlot *s = &p->s[i];
			const bool	settled = sift_vis_part_inserts_settled(&s->d, xmin);

			if (s->part_id != e->part_id)
				continue;
			s->px = px[k];
			s->unseen = px[k] != NULL || !settled;
			rows += i >= p->n || !settled ? s->d.nrows : 0;
		}
		if (e->l0_slab_blk != InvalidBlockNumber)
		{
			BlockNumber *pages;
			int			npages;

			slab[k] = sift_rowbuf_new(&f->spec);
			sift_l0_slab_read(p->rel, e->part_id, e->l0_slab_blk, slab[k], &f->spec, p->snap, &pages, &npages);
			rows += slab[k]->n;
		}
		if (rows > max_rows || npend > SIFT_SCOPE_OVERLAY_PEND_MAX)
			return false;
	}
	return true;
}

/* The parts of the manifest the summary does not name, after the ones it does */
static void
add_unnamed_parts(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftScopeMeta *m)
{
	for (int k = 0; k < mr->n; k++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(p->rel, &mr->entries[k], &np);

		for (int j = 0; j < np; j++)
		{
			SiftPart   *part;

			if (sift_f2s_meta_names(m, parts[j].part_gen))
				continue;
			part = sift_part_open(p->rel, parts[j].header_blk, parts[j].part_gen, (uint32) mr->entries[k].part_id);
			if (part->hdr.ngranules == 0)
				continue;
			p->s = repalloc(p->s, (p->n + p->nadd + 1) * sizeof(SiftF2ScopeSlot));
			p->s[p->n + p->nadd++] = (SiftF2ScopeSlot) {.part_gen = parts[j].part_gen,.part_id = mr->entries[k].part_id,.d = parts[j],.ng = part->hdr.ngranules,.part = part};
		}
	}
}

static void
added_parts_rows(struct SiftF2ScopeRowSet *a, SiftF2ScopeParts *p)
{
	for (int i = p->n; i < p->n + p->nadd; i++)
		for (uint32 g = 0; g < p->s[i].ng; g++)
		{
			SiftScopeRows r = {0};
			const uint16 *gone;
			const uint32 ngone = sift_f2s_granule_excl(p, &p->s[i], g, &gone);
			MemoryContext old;

			MemoryContextReset(p->gcx);
			old = MemoryContextSwitchTo(p->gcx);
			sift_f2s_decode_rows(p, &p->s[i], g, NULL, 0, false, &r);
			MemoryContextSwitchTo(old);
			added_granule(a, &r, gone, ngone);
		}
}

bool
sift_f2s_overlay_open(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftScopeMeta *m)
{
	SiftRowBuf **slab = palloc0(Max(mr->n, 1) * sizeof(SiftRowBuf *));
	const SiftPendDelIndex **px = palloc0(Max(mr->n, 1) * sizeof(SiftPendDelIndex *));
	uint64		cap = 0;
	struct SiftF2ScopeRowSet *a;

	sift_xid_cache_init(&p->xc, p->snap);
	add_unnamed_parts(p, mr, m);
	if (!load_partitions(p, mr, slab, px, overlay_cap(p)))
		return false;
	for (int i = p->n; i < p->n + p->nadd; i++)
		cap += p->s[i].d.nrows;
	for (int k = 0; k < mr->n; k++)
		cap += slab[k] != NULL ? slab[k]->n : 0;
	if (cap == 0)
		return true;
	a = rowset_new(p->sc, (uint32) cap);
	added_parts_rows(a, p);
	for (int k = 0; k < mr->n; k++)
		if (slab[k] != NULL)
		{
			added_slab(a, p, slab[k], px[k]);
			sift_rowbuf_free(slab[k]);
		}
	rowset_link(a, sift_f2s_pairs(p));
	p->added = a;
	return true;
}

/* The granules of the parts the summary names that hold rows the snapshot does not see, each with those rows; false when they are more than max */
static bool
gone_granules(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a, uint64 max, uint32 *total)
{
	uint32		cap = 0;

	*total = 0;
	for (int i = 0; i < p->n; i++)
	{
		SiftF2ScopeSlot *s = &p->s[i];

		for (uint32 g = 0; s->part_gen != 0 && sift_f2s_slot_dirty(p, s) && g < s->ng; g++)
		{
			const uint16 *rows;
			const uint32 n = sift_f2s_granule_excl(p, s, g, &rows);

			if (n == 0)
				continue;
			if (*total + n > max)
				return false;
			if (a->ngran == cap)
			{
				cap = Max(cap * 2, 16);
				a->gran = a->gran == NULL ? palloc(cap * sizeof(GoneGranule)) : repalloc(a->gran, cap * sizeof(GoneGranule));
			}
			a->gran[a->ngran++] = (GoneGranule) {s, g, *total, n, memcpy(palloc(n * sizeof(uint16)), rows, n * sizeof(uint16)), false};
			*total += n;
		}
	}
	return true;
}

/* The members of the rows of every granule, which tell the keys the rows count for */
static void
gone_members(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a)
{
	for (uint32 k = 0; k < a->ngran; k++)
	{
		const GoneGranule *gr = &a->gran[k];
		SiftScopeRows r = {0};
		MemoryContext old;

		MemoryContextReset(p->gcx);
		old = MemoryContextSwitchTo(p->gcx);
		sift_f2s_decode_rows(p, gr->s, gr->g, gr->rows, gr->n, true, &r);
		MemoryContextSwitchTo(old);
		for (uint32 i = 0; i < gr->n; i++)
			rowset_set_member(a, gr->first + i, &r, i);
	}
}

/* The cells of the granule that holds row, read the first time a key asks for one of its rows: the granules whose rows belong to no key asked for are never read */
static void
gone_fill(struct SiftF2ScopeRowSet *a, uint32 row)
{
	uint32		lo = 0,
				hi = a->ngran;
	GoneGranule *gr;
	SiftScopeRows r = {0};
	MemoryContext old;

	while (hi - lo > 1)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (a->gran[mid].first <= row)
			lo = mid;
		else
			hi = mid;
	}
	gr = &a->gran[lo];
	if (gr->filled)
		return;
	MemoryContextReset(a->owner->gcx);
	old = MemoryContextSwitchTo(a->owner->gcx);
	sift_f2s_decode_rows(a->owner, gr->s, gr->g, gr->rows, gr->n, false, &r);
	MemoryContextSwitchTo(old);
	for (uint32 i = 0; i < gr->n; i++)
		rowset_set_cells(a, gr->first + i, &r, i);
	gr->filled = true;
}

static void
gone_open(SiftF2ScopeParts *p)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));
	struct SiftF2ScopeRowSet *a = palloc0(sizeof(*a));
	uint32		total;

	a->owner = p;
	if (gone_granules(p, a, overlay_cap(p), &total))
	{
		struct SiftF2ScopeRowSet *g = rowset_new(p->sc, Max(total, 1));

		g->owner = p;
		g->gran = a->gran;
		g->ngran = a->ngran;
		g->n = total;
		gone_members(p, g);
		rowset_link(g, sift_f2s_pairs(p));
		p->gone = g;
	}
	else
		p->gone_many = true;
	MemoryContextSwitchTo(old);
}

SiftScopeGone
sift_f2s_gone_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out)
{
	SiftF2ScopeParts *p = arg;

	if (p->gone == NULL && !p->gone_many)
		gone_open(p);
	if (p->gone_many)
		return SIFT_GONE_MANY;
	return rowset_cube(p->gone, scope_key, month, out) ? SIFT_GONE_ROWS : SIFT_GONE_NONE;
}
