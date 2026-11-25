/*
 * col/f2/col_delta/host.c - sift_col_host_ops: a sift_col index of format major 2 as the host of the shared columnar engine (col/f2/store/store.h).
 *
 * An index keeps its rows in the heap: the heap decides visibility (filter_granule_vis asks its map), a wide value is a leaf of the index (no read_wide_blob, no spill), and nothing
 * is mapped through attribute numbers (no build_col_map). The leaves, the key and the time columns are those of the index (sift_f2_rel); the store keeps copies of them, so that it
 * outlives the entry of the relcache they were made with.
 */
#include "core/host.h"

#include "access/htup_details.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/col_delta/host.h"
#include "col/f2/store/store.h"

static uint16
filter_granule_vis(void *vis_ctx, const SiftTidIndexEntry *tidx, const uint64 *tids, uint16 nrows, uint64 *sel_mask)
{
	const SiftColHostVis *c = vis_ctx;

	return sift_vis_f2_granule_filter(c->vis, c->guard, tidx->tid_min, tidx->tid_max, tids, nrows, sel_mask);
}

static bool
needs_heap_vet(const SiftF2Store *store pg_attribute_unused(), const SiftColMetaPageData *meta)
{
	return (meta->common.compat_flags & SIFT_COMPAT_NATIVE_MVCC) == 0;
}

static void *
copy_of(MemoryContext cx, const void *src, Size len)
{
	return src != NULL ? memcpy(MemoryContextAlloc(cx, Max(len, 1)), src, len) : NULL;
}

static void
define_attributes(const SiftColRel *r, SiftF2Store *s, MemoryContext cx)
{
	s->att_first = MemoryContextAlloc(cx, sizeof(int) * (s->natts + 1));
	s->leaf_attno = MemoryContextAlloc(cx, sizeof(int16) * Max(s->ncols, 1));
	for (int a = 0; a <= s->natts; a++)
		s->att_first[a] = -1;
	for (int i = s->ncols - 1; i >= 0; i--)
	{
		int			a = r->schema->cols[i].attno;

		s->leaf_attno[i] = a;
		s->att_first[a] = i;
		s->has_jsonb = s->has_jsonb || r->json[a];
	}
}

static void
define_keys(const SiftF2Rel *f, SiftF2Store *s, MemoryContext cx)
{
	s->key_leaf = copy_of(cx, f->key_leaf, f->ncols * sizeof(bool));
	s->key_len = copy_of(cx, f->key_len, f->ncols * sizeof(uint16));
	s->key_attno = copy_of(cx, f->key_attno, f->ncols * sizeof(int16));
	s->key_name = MemoryContextAllocZero(cx, sizeof(char *) * Max(f->ncols, 1));
	for (int i = 0; i < f->ncols; i++)
		if (f->key[i] != NULL)
			s->key_name[i] = memcpy(MemoryContextAllocZero(cx, f->key_len[i] + 1), f->key[i], f->key_len[i]);
	s->has_keys = f->has_keys;
	s->nkey = f->nkey;
	memcpy(s->key_cols, f->key_cols, sizeof(s->key_cols));
}

static void
load_options(Relation rel, SiftF2Store *s, MemoryContext cx)
{
	const SiftColRel *r = sift_col_rel_private(rel, cx);
	const SiftF2Rel *f = sift_f2_rel(rel);
	SiftF2StoreOpts *o = &s->opts;

	o->key_order = o->paths = o->compression = o->scope = o->connect = o->preagg = o->sketches = MemoryContextStrdup(cx, "");
	o->skip_index = MemoryContextStrdup(cx, sift_col_options_skip_index(rel));
	o->auto_maint = true;
	o->vacuum_truncate = true;
	o->track_changes = sift_col_track_on(rel);
	s->host_data = (void *) r;
	s->schema = r->schema;
	s->ncols = f->ncols;
	s->kinds = copy_of(cx, f->kinds, f->ncols * sizeof(SiftCellKind));
	s->hints = copy_of(cx, f->hints, f->ncols);
	s->scales = copy_of(cx, f->scales, f->ncols);
	s->parent = copy_of(cx, f->parent, f->ncols * sizeof(int16));
	s->has_groups = f->has_groups;
	memcpy(s->time_unit, f->time_unit, sizeof(s->time_unit));
	define_attributes(r, s, cx);
	define_keys(f, s, cx);
	s->spec = f->spec;
	s->spec.kinds = s->kinds;
	s->spec.hints = s->hints;
	s->spec.scales = s->scales;
	s->spec.sort_cols = s->key_cols;
	s->spec.key_leaf = s->has_keys ? s->key_leaf : NULL;
	s->spec.parent = s->has_groups ? s->parent : NULL;
}

const SiftF2HostOps sift_col_host_ops = {
	.host_kind = SIFT_F2_HOST_INDEX,
	.am_kind = SIFT_AM_COL,
	.base_compat_flags = 0,
	.filter_granule_vis = filter_granule_vis,
	.needs_heap_vet = needs_heap_vet,
	.load_options = load_options,
};
