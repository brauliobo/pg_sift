/*
 * col/f2/f2_scope.c - the scope summaries of a relation of format major 2, a sift_col index or a sift_table (f2_scope.h): the shape of the cubes the leaves of the relation make, the pairs of a
 * scope read from the btree of its relation, and the summary opened for a query. The granules of the entity parts are in f2_scope_ent.c, the build and the refresh in f2_scope_build.c.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_scope_cache.h"
#include "col/f2/f2_store_schema.h"
#include "col/f2/part_vis.h"
#include "col/scope.h"
#include "core/memtrack.h"
#include "core/page_io.h"

#include "col/f2/f2_scope_int.h"

bool
sift_f2s_plain_leaf(const SiftColDef *d)
{
	return d->parent < 0 && d->lane == 0 && d->lanes < 2 && d->kind == SIFT_COLK_SCALAR && d->path[0] == '\0' && !(d->flags & (SIFT_COLD_DERIVED | SIFT_COLD_SCOPE));
}

/* The leaves that can be the order of a ring: the time leaves (SIFT_RING_KIND_*) and the integer columns */
int
sift_f2s_ring_kind(const SiftColDef *d)
{
	if (!sift_f2s_plain_leaf(d) || d->store != SIFT_COLS_INT)
		return 0;
	switch (d->type)
	{
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return SIFT_RING_KIND_TIMESTAMP;
		case SIFT_COLT_DATE:
			return SIFT_RING_KIND_DATE;
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
			return SIFT_RING_KIND_ORDINAL;
		default:
			return 0;
	}
}

static bool
measure_leaf(const SiftColDef *d)
{
	return sift_f2s_plain_leaf(d) && (d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_INT8 || d->type == SIFT_COLT_FLOAT4 || d->type == SIFT_COLT_FLOAT8 ||
							 (d->type == SIFT_COLT_NUMERIC && d->store == SIFT_COLS_INT));
}

static bool
is_float(const SiftColDef *d)
{
	return d->type == SIFT_COLT_FLOAT4 || d->type == SIFT_COLT_FLOAT8;
}

static bool
key_order_leaf(const SiftF2Store *store, int leaf)
{
	for (int k = 0; k < store->nkey; k++)
		if (store->key_cols[k] == leaf)
			return true;
	return false;
}

int
sift_f2s_nscopes(const SiftF2Store *store)
{
	return sift_f2s_is_table(store) ? store->nscopes : ((const SiftColRel *) store->host_data)->nscope;
}

/* The leaf of the member key: that of the column the declaration names, in a table; the leaf of its attribute the derived leaf of the scope stands for, in an index */
static int
member_leaf_of(const SiftF2Store *store, int ord)
{
	const SiftColRel *r = store->host_data;

	return sift_f2s_is_table(store) ? store->scopes[ord].member_leaf : store->att_first[store->schema->cols[r->scope_leaf[ord]].attno];
}

static const SiftColDef *
member_def(const SiftF2Store *store, int ord)
{
	return &store->schema->cols[member_leaf_of(store, ord)];
}

static int
time_leaf_of(const SiftF2Store *store, int ord)
{
	return sift_f2s_is_table(store) ? store->scopes[ord].time_leaf : sift_scope_time_leaf(store->schema->cols, store->ncols);
}

static bool
member_ok(const SiftColDef *member)
{
	return member->store == SIFT_COLS_INT && !(member->flags & SIFT_COLD_CODED) && member->lanes <= 1 &&
		(member->type == SIFT_COLT_INT2 || member->type == SIFT_COLT_INT4 || member->type == SIFT_COLT_INT8);
}

bool
sift_f2s_supported(Relation rel, const SiftF2Store *store, int ord)
{
	return member_ok(member_def(store, ord));
}

bool
sift_f2s_kept(const SiftF2Store *store, const SiftScopeSpec *sp, int leaf)
{
	if (!sift_f2s_is_table(store) || sp->cols == NIL)
		return true;
	foreach_ptr(char, name, sp->cols)
		if (strcmp(store->schema->cols[leaf].name, name) == 0)
			return true;
	return false;
}

void
sift_f2s_cfg(Relation rel, const SiftF2Store *store, int ord, SiftF2ScopeCfg *sc)
{
	const SiftScopeSpec *sp = sift_scope_specs(rel)->s[ord];
	const SiftColDef *member = member_def(store, ord);
	const char *kind = sift_f2s_is_table(store) ? "table" : "index";

	if (!member_ok(member))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\" of %s \"%s\" has a member key that is not an integer column", sp->key_name, kind, RelationGetRelationName(rel)),
						errhint("Declare the scope over an integer member column.")));
	*sc = (SiftF2ScopeCfg) {.ord = ord,.spec = sp,.member_leaf = member->id,.time_leaf = time_leaf_of(store, ord),.time_unit = 1};
	if (sc->time_leaf >= 0 && store->schema->cols[sc->time_leaf].type == SIFT_COLT_DATE)
		sc->time_unit = USECS_PER_DAY;
	for (int i = 0; i < store->ncols; i++)
	{
		const SiftColDef *d = &store->schema->cols[i];

		if (i == sc->member_leaf || i == sc->time_leaf || key_order_leaf(store, i) || !sift_f2s_kept(store, sp, i))
			continue;
		if (measure_leaf(d) && sc->shape.nmeas < SIFT_SCOPE_MEAS_CAP)
		{
			sc->shape.float_meas |= is_float(d) ? (uint32) 1 << sc->shape.nmeas : 0;
			sc->meas_leaf[sc->shape.nmeas++] = i;
		}
		else if (sift_f2s_plain_leaf(d) && d->type == SIFT_COLT_BOOL && sc->shape.ndims < SIFT_SCOPE_DIMS_CAP)
			sc->dim_leaf[sc->shape.ndims++] = i;
	}
	sc->shape.minmax = true;
}

/* The leaves a summary reads or may be asked to read: the member key, the time and the key order leaves, the leaves of the cubes (measures and flags) and those that can order a ring */
bool
sift_f2s_reads_leaf(const SiftF2Store *store, const SiftF2ScopeCfg *sc, int leaf)
{
	const SiftColDef *d = &store->schema->cols[leaf];

	if (leaf == sc->member_leaf || leaf == sc->time_leaf || key_order_leaf(store, leaf) || (d->flags & (SIFT_COLD_DERIVED | SIFT_COLD_SCOPE)))
		return true;
	return sift_f2s_kept(store, sc->spec, leaf) && (measure_leaf(d) || sift_f2s_ring_kind(d) != 0 || (sift_f2s_plain_leaf(d) && d->type == SIFT_COLT_BOOL));
}

bool
sift_f2_scope_reads_any(Relation rel, const int16 *leaves, int n)
{
	SiftF2Store *store = sift_f2_store_open(rel);

	for (int ord = 0; ord < sift_f2s_nscopes(store); ord++)
	{
		SiftF2ScopeCfg sc;

		if (!sift_f2s_supported(rel, store, ord))
			return true;
		sift_f2s_cfg(rel, store, ord, &sc);
		for (int i = 0; i < n; i++)
			if (sift_f2s_reads_leaf(store, &sc, leaves[i]))
				return true;
	}
	return false;
}

/* Format 2 summarizes the scopes whose pairs are integers; the other indexes of a scope stay in format major 1 */
bool
sift_f2s_index_summarizable(Relation heap, Relation index)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		Relation	pairs;
		AttrNumber	member,
					key;
		bool		ok;

		sift_scope_relation(heap, index, specs->s[i], &pairs, &member, &key);
		ok = sift_f2s_int_type(get_atttype(RelationGetRelid(pairs), member)) && sift_f2s_int_type(get_atttype(RelationGetRelid(pairs), key));
		table_close(pairs, NoLock);
		if (!ok)
			return false;
	}
	return true;
}

bool
sift_f2s_ready(Relation rel, const SiftF2Store *store, int ord)
{
	Relation	pairs;
	AttrNumber	a,
				b;
	bool		ok = sift_f2s_supported(rel, store, ord);

	if (!ok || !sift_f2s_is_table(store))
		return ok;
	ok = OidIsValid(sift_scope_relation_index(rel, rel, sift_scope_specs(rel)->s[ord], &pairs, &a, &b));
	if (ok)
	{
		ok = sift_f2s_int_type(TupleDescAttr(pairs->rd_att, a - 1)->atttypid) && sift_f2s_int_type(TupleDescAttr(pairs->rd_att, b - 1)->atttypid);
		table_close(pairs, NoLock);
	}
	return ok && sift_scope_has_journal(rel, ord);
}

/* The rings of a summary are those its header names: the time leaf and up to three order leaves */
void
sift_f2s_cfg_rings_of(SiftF2ScopeCfg *cfg, const SiftF2Store *store, const SiftScopeMeta *m)
{
	SiftScopeTopKDesc desc = {0};

	cfg->nrings = cfg->time_leaf >= 0 ? Max((int) m->ntopk_rings, 1) : 0;
	if (m->hdr.flags & SIFT_SMETA_MULTI_TOPK)
		memcpy(&desc, &m->hdr.reserved[0], sizeof(desc));
	for (int q = 1; q < cfg->nrings; q++)
	{
		cfg->ring_leaf[q - 1] = desc.extra_leaf[q - 1];
		cfg->ring_unit[q - 1] = store->schema->cols[cfg->ring_leaf[q - 1]].type == SIFT_COLT_DATE ? USECS_PER_DAY : 1;
	}
}

int
sift_f2_nscopes(Relation rel)
{
	return sift_f2s_nscopes(sift_f2_store_open(rel));
}

bool
sift_f2_scoped(Relation rel)
{
	return sift_f2_nscopes(rel) > 0;
}

static SiftF2Scope *
scope_abandon(MemoryContext cx, MemoryContext old, Relation rel, Relation heap)
{
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	if (heap != rel)
		table_close(heap, NoLock);
	return NULL;
}

/* The summary of scope ord as the directory names it: the entry of the backend's cache when the statement reads the cubes of a snapshot (live), else the object read from its part, rings unpacked */
static const SiftScopeMeta *
scope_object(Relation rel, const SiftPlanSnapshot *plan, int ord, MemoryContext cx, bool live, SiftScopeMeta *own)
{
	const SiftManifestScopeRef *ref = sift_f2_scope_cache_ref(plan, ord);
	uint64		len;
	char	   *bytes;

	if (ref == NULL)
		return NULL;
	if (live)
		return sift_f2_scope_cache_get(rel, plan, ord, cx);
	bytes = sift_f2_scope_read_object(rel, sift_part_open(rel, ref->header_blk, ref->meta_gen, 0), &len);
	if (bytes == NULL || !sift_scope_meta_open(own, bytes, len))
		SIFT_CORRUPT(rel, "The summary of scope %d is damaged.", ord);
	return own;
}

/* live: the rows the summary lacks or counts too many for the snapshot are read from the parts (f2_scope_over.c); NULL when it cannot be served from the summary and the parts */
static SiftF2Scope *
scope_open(Relation rel, int ord, Snapshot snap, int excl_col, bool live, const int64 *keys, uint32 nkeys)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	const SiftPlanSnapshot *plan = sift_plan_cache_get(rel, CurrentMemoryContext);
	SiftManifestRead mr;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope reader");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftF2Scope *sc = palloc0(sizeof(SiftF2Scope));
	SiftF2ScopeCfg *cfg = palloc(sizeof(SiftF2ScopeCfg));
	SiftF2ScopeParts *p;
	const SiftScopeMeta *meta;
	Relation	heap = sift_f2s_is_table(store) ? rel : table_open(sift_scope_fact_oid(rel), AccessShareLock);

	meta = scope_object(rel, plan, ord, cx, live, &sc->meta);
	if (meta == NULL)
		return scope_abandon(cx, old, rel, heap);
	if (meta != &sc->meta)
	{
		sc->meta = *meta;
		sc->cached = meta;
	}
	sift_f2s_cfg(rel, store, ord, cfg);
	sift_f2s_cfg_rings_of(cfg, store, &sc->meta);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	p = sift_f2s_parts_new(rel, store, heap, cfg, snap, NULL, sc->meta.hdr.nparts);
	p->excl_leaf = excl_col > 0 ? store->att_first[excl_col] : -1;
	for (uint32 i = 0; i < sc->meta.hdr.nparts; i++)
	{
		const SiftScopePartMapEntry *e = &sc->meta.parts[i];
		bool		found = false;

		if (e->part_gen == 0)
			continue;
		for (int k = 0; k < plan->npartitions && !found; k++)
			for (int j = 0; j < plan->partlists[k].nparts && !found; j++)
				if (plan->partlists[k].parts[j].part_gen == e->part_gen)
				{
					(void) sift_f2s_parts_add(p, &plan->partlists[k].parts[j], plan->partitions[k].part_id, e->base_granule_idx, e->ngranules);
					found = true;
				}
		if (!found && live)
			return scope_abandon(cx, old, rel, heap);
		if (!found)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("the summary of scope %d of \"%s\" describes part %llu, which a compaction has replaced", ord,
																					 RelationGetRelationName(rel), (unsigned long long) e->part_gen),
							errhint("Run VACUUM on the table: it builds the summary again.")));
	}
	if (p->excl_leaf >= 0 && !sift_f2s_leaf_direct(p, p->excl_leaf))
		return scope_abandon(cx, old, rel, heap);
	if (live && !(sift_f2s_deltas_clear(rel, store, ord, &mr) && sift_f2s_pair_edits(rel, ord, &sc->meta, snap, keys, nkeys, &sc->changed, &sc->nchanged)))
		return scope_abandon(cx, old, rel, heap);
	p->pair_gen = live ? sift_f2_scope_cache_ref(plan, ord)->meta_gen : 0;
	if (live && sc->nchanged > 0)
	{
		p->pm_object = sift_f2s_pairs(p);
		p->changed = sc->changed;
		p->nchanged = sc->nchanged;
		p->pm = sift_f2s_edited_pairs(heap, rel, cfg, snap, p->pm_object, keys, nkeys, sc->changed, sc->nchanged);
	}
	if (live && !sift_f2s_overlay_open(p, &mr, &sc->meta, sc->cached))
		return scope_abandon(cx, old, rel, heap);
	sc->rel = rel;
	sc->cx = cx;
	sc->ord = ord;
	sc->snap = snap;
	sc->max_part_gen = sc->meta.hdr.max_part_gen;
	sc->entity = sift_f2s_entity(p);
	sc->heap = heap;
	sc->nrings = cfg->nrings;
	if (cfg->nrings > 0)
		sc->ring_col[0] = store->schema->cols[cfg->time_leaf].attno;
	for (int q = 1; q < cfg->nrings; q++)
		sc->ring_col[q] = store->schema->cols[cfg->ring_leaf[q - 1]].attno;
	MemoryContextSwitchTo(old);
	return sc;
}

/* An opened summary; excl_col (an attribute of the relation, 0: none) is the leaf of a query that keeps the rows where it is NULL */
SiftF2Scope *
sift_f2_scope_open(Relation rel, int ord, Snapshot snap, int excl_col)
{
	return scope_open(rel, ord, snap, excl_col, false, NULL, 0);
}

/* The summary read for the snapshot of a statement whatever the parts and the slabs hold that it does not count, and the pairs of the keys it reads as the journal says they changed (NULL: it cannot be served, the join answers) */
SiftF2Scope *
sift_f2_scope_open_keys(Relation rel, int ord, Snapshot snap, int excl_col, const int64 *keys, uint32 nkeys)
{
	SiftF2Store *store = sift_f2_store_open(rel);

	if (snap->snapshot_type != SNAPSHOT_MVCC || sift_f2s_unreadable(rel, store, snap) || !sift_f2s_supported(rel, store, ord))
		return NULL;
	return scope_open(rel, ord, snap, excl_col, true, keys, nkeys);
}

void
sift_f2_scope_close(SiftF2Scope *sc)
{
	Relation	heap = sc->heap;
	bool		own_heap = heap != sc->rel;

	if (sc->cached == NULL)
		sift_scope_meta_close(&sc->meta);
	MemoryContextDelete(sc->cx);
	if (own_heap)
		table_close(heap, NoLock);
}

bool
sift_f2_scope_rings(SiftF2Scope *sc)
{
	if (sc->cached == NULL)
		return true;
	if (!sift_f2_scope_cache_rings(sc->cached))
		return false;
	sc->meta = *sc->cached;
	return true;
}

int
sift_f2_scope_ring_of(const SiftF2Scope *sc, int col)
{
	for (int q = 0; q < sc->nrings; q++)
		if (sc->ring_col[q] == col)
			return q;
	return -1;
}

bool
sift_f2_scope_ring_eligible(Relation rel, int col)
{
	const SiftF2Store *store = sift_f2_store_open(rel);

	return store->att_first[col] >= 0 && sift_f2_store_attr_leaves(store, col) == 1 && sift_f2s_ring_kind(&store->schema->cols[store->att_first[col]]) != 0;
}

bool
sift_f2_scope_excl_ok(Relation rel, int ord, int col)
{
	const SiftF2Store *store = sift_f2_store_open(rel);
	const int	leaf = store->att_first[col];

	return leaf >= 0 && sift_f2_store_attr_leaves(store, col) == 1 && sift_f2s_plain_leaf(&store->schema->cols[leaf]) && store->schema->cols[leaf].store == SIFT_COLS_INT &&
		sift_f2s_kept(store, sift_scope_specs(rel)->s[ord], leaf);
}

bool
sift_f2_scope_layout(Relation rel, int ord, SiftF2ScopeLayout *out)
{
	const SiftF2Store *store = sift_f2_store_open(rel);
	SiftF2ScopeCfg sc;

	if (!sift_f2s_supported(rel, store, ord))
		return false;
	sift_f2s_cfg(rel, store, ord, &sc);
	*out = (SiftF2ScopeLayout) {.member_col = member_def(store, ord)->attno,.time_col = sc.time_leaf >= 0 ? store->schema->cols[sc.time_leaf].attno : 0,.nmeas = sc.shape.nmeas,
	.float_meas = sc.shape.float_meas};
	for (int m = 0; m < sc.shape.nmeas; m++)
	{
		out->meas_col[m] = store->schema->cols[sc.meas_leaf[m]].attno;
		out->meas_scale[m] = store->schema->cols[sc.meas_leaf[m]].scale;
	}
	return true;
}
