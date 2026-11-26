/*
 * col/f2/scope/build.c - the build and the refresh of the summaries of a scope (f2_scope.h), for an index and for a sift_table: the part each summary is kept in, the build over the entity parts of the
 * manifest, the refresh of the ones that are not what the parts and the pairs are now, and the blocks of a summary.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "utils/snapmgr.h"

#include "col/f2/scope/scope.h"
#include "col/f2/part/vis.h"
#include "col/scope/scope.h"
#include "core/f2/gate.h"
#include "core/cmp.h"
#include "core/memtrack.h"
#include "core/page_io.h"

#include "col/f2/scope/int.h"
#include "col/f2/scope/mem.h"

/* The part a summary is kept in: one section, the object */
static SiftManifestScopeRef
write_object(Relation rel, int ord, const StringInfoData *obj, uint64 gen, uint32 nkeys)
{
	StringInfoData packed;
	SiftSecInput sec = {.tag = SEC_SCOPE_HDR,.flags = SIFT_SEC_FLAG_REQUIRED};
	SiftPartIn	in = {.part_id = 0,.part_gen = gen,.level = 0,.ncols = 0,.flags = SIFT_PART_FLAG_SCOPE,.nrows = nkeys,.ngranules = 0,.granule_rows = SIFT_GRANULE_ROWS,
	.min_xid = sift_xid_to_fxid(FrozenTransactionId),.max_xid = sift_xid_to_fxid(FrozenTransactionId),.sections = &sec,.nsections = 1,.inline_extent_cap = -1,.bulk_tail = true};
	SiftPartDesc d;

	sift_f2_scope_pack(obj, &packed);
	sec.data = packed.data;
	sec.len = packed.len;
	d = sift_part_write(rel, &in);
	pfree(packed.data);
	return (SiftManifestScopeRef) {.scope_ord = ord,.flags = SIFT_SCOPEREF_ACTIVE,.header_blk = d.header_blk,.meta_gen = gen};
}

static void
build_stamps(Relation rel, int ord, const SiftScopeSpec *sp, Snapshot snap, SiftScopeBuildSpec *spec)
{
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS] = {0};

	StaticAssertStmt(sizeof(SiftConnStamp) <= sizeof(SiftScopeStampImage), "a journal stamp fits the image the summary keeps");
	if (sift_scope_take(rel, ord, snap, stamp) != SIFT_CONN_OK)
		for (int j = 0; j < sift_scope_njournals(sp); j++)
			stamp[j] = (SiftConnStamp) {.xmin = snap->xmin};
	for (int j = 0; j < sift_scope_njournals(sp); j++)
		memcpy(&spec->stamps[j], &stamp[j], sizeof(SiftConnStamp));
}

static void
spec_init(Relation rel, const SiftF2Store *store, const SiftF2ScopeCfg *sc, Snapshot snap, uint64 gen, SiftScopeBuildSpec *spec)
{
	*spec = (SiftScopeBuildSpec) {.shape = sc->shape,.scope_ord = sc->ord,.member_leaf = sc->member_leaf,.time_leaf = sc->time_leaf,.ntopk_rings = sc->nrings,
	.schema_hash = store->schema->hash,.mem_budget = Max((uint64) sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 4), SIFT_SCOPE_BUILD_MEM_MIN),.meta_epoch = gen};
	memset(spec->extra_leaf, 0xFF, sizeof(spec->extra_leaf));
	for (int q = 1; q < sc->nrings; q++)
		spec->extra_leaf[q - 1] = (int16) sc->ring_leaf[q - 1];
	build_stamps(rel, sc->ord, sc->spec, snap, spec);
	if (sift_scope_pairs_multiset(rel, sc->spec))
		spec->flags |= SIFT_SMETA_MULTISET;
}

/* The order leaves of the rings: the ones of the leaves of the relation that the zones of the parts say are set in enough rows; an integer column only when its zones rise, as a key or a sequence does, not a hash or a measure */
static void
pick_rings(SiftF2ScopeParts *p, SiftF2ScopeCfg *cfg, const SiftF2Store *store, SiftScopeBuildSpec *spec)
{
	SiftScopeRingCand *cand = palloc(Max(store->ncols, 1) * sizeof(SiftScopeRingCand));
	uint32		n = 0;

	if (cfg->time_leaf < 0)
		return;
	for (int i = 0; i < store->ncols; i++)
	{
		const int	kind = sift_f2s_ring_kind(&store->schema->cols[i]);
		SiftScopeRingCand c = {.leaf = (int16) i,.kind = (uint8) kind};
		SiftF2LeafShape sh;

		if (kind == 0 || i == cfg->member_leaf || i == cfg->time_leaf || !sift_f2s_kept(store, cfg->spec, i) || !sift_f2s_leaf_direct(p, i))
			continue;
		sift_f2s_leaf_shape(p, i, &sh);
		if (kind == SIFT_RING_KIND_ORDINAL && sh.nrise * 100 < sh.nzones * SIFT_SCOPE_RING_RISING_PCT)
			continue;
		c.nrows = sh.nrows;
		c.nnull = sh.nnull;
		cand[n++] = c;
	}
	cfg->nrings = (int) sift_scope_rings_pick(spec, cand, n);
	for (int q = 1; q < cfg->nrings; q++)
	{
		cfg->ring_leaf[q - 1] = spec->extra_leaf[q - 1];
		cfg->ring_unit[q - 1] = store->schema->cols[cfg->ring_leaf[q - 1]].type == SIFT_COLT_DATE ? USECS_PER_DAY : 1;
	}
	pfree(cand);
}

#define CLUSTER_SAMPLE_GRANULES	16

static uint32
distinct_homes(const SiftEntHome *runs, uint32 n)
{
	int64	   *h = palloc(Max(n, 1) * sizeof(int64));
	uint32		u = 0;

	for (uint32 i = 0; i < n; i++)
		h[i] = runs[i].home;
	qsort(h, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		u += i == 0 || h[i] != h[i - 1];
	pfree(h);
	return u;
}

/* STOR[STOR-SCOPE-03]: entity route kept only if granules are clustered (<= max 2 or keys/8 homes) */
static uint32
physical_clustering(SiftF2ScopeParts *p, const SiftScopePairMap *pm)
{
	const uint32 want = sift_spm_cluster_auto(pm);
	const SiftScopeGranSource src = sift_f2s_source(p);
	SiftEntHome *runs;
	uint32		total = 0,
				sampled = 0;
	uint64		homes = 0;

	for (int i = 0; want != 0 && i < p->n; i++)
		total += p->s[i].ng;
	if (want == 0 || total == 0)
		return 0;
	runs = palloc(sizeof(*runs) << SIFT_GRANULE_ROW_BITS);
	for (uint32 g = 0; g < total; g += Max(total / CLUSTER_SAMPLE_GRANULES, 1), sampled++)
		homes += distinct_homes(runs, src.runs(src.arg, g, runs));
	pfree(runs);
	return homes <= (uint64) sampled * Max(2, pm->nkeys / 8) ? want : 0;
}

/* A summary of a table is built over parts whose leaves of the cubes read as the table's own and no column delta of which patches what the summary reads: of an index always */
static bool
buildable(Relation heap, Relation rel, SiftF2Store *store, const SiftF2ScopeCfg *cfg, const SiftManifestRead *mr, const SiftManifestEdit *edit, Snapshot snap)
{
	SiftF2ScopeParts *p;
	bool		ok;

	if (!sift_f2s_is_table(store))
		return true;
	p = sift_f2s_parts_new(rel, store, heap, cfg, snap, NULL, sift_f2s_parts_in_manifest(rel, mr, edit));
	sift_f2s_parts_of_manifest(p, mr, edit);
	ok = sift_f2s_parts_exact(p) && sift_f2s_deltas_clear(rel, store, cfg->ord, mr);
	MemoryContextDelete(p->gcx);
	return ok;
}

static SiftManifestScopeRef
build_one(Relation heap, Relation rel, SiftF2Store *store, const SiftF2ScopeCfg *base, const SiftManifestRead *mr, const SiftManifestEdit *edit, Snapshot snap)
{
	SiftF2ScopeCfg cfg = *base;
	SiftScopeBuildSpec spec;
	SiftScopePairMap *pm;
	SiftF2ScopeParts *p;
	SiftScopeBuild *b;
	SiftScopeGranSource src;
	StringInfoData out;
	uint64		gen = sift_manifest_new_gen(rel, 1);
	SiftManifestScopeRef ref;

	cfg.nrings = cfg.time_leaf >= 0 ? 1 : 0;
	spec_init(rel, store, &cfg, snap, gen, &spec);
	cfg.mem = sift_f2s_mem_open(heap, rel, store, &cfg, true);
	pm = sift_f2s_build_pairs(heap, rel, &cfg, snap, sift_f2s_key_map(heap, rel, cfg.spec, true));
	cfg.mem = sift_f2s_mem_open(heap, rel, store, &cfg, false);
	spec.flags |= cfg.mem != NULL ? SIFT_SMETA_CODED_MEM : 0;
	p = sift_f2s_parts_new(rel, store, heap, &cfg, snap, pm, sift_f2s_parts_in_manifest(rel, mr, edit));
	sift_f2s_parts_of_manifest(p, mr, edit);
	pick_rings(p, &cfg, store, &spec);
	spec.flags |= physical_clustering(p, pm);
	spec.nrows = 0;
	for (int i = 0; i < p->n; i++)
		spec.nrows += p->s[i].d.nrows;
	b = sift_scope_build_begin(&spec, pm);
	for (int i = 0; i < p->n; i++)
		(void) sift_scope_build_add_part(b, p->s[i].part_gen, p->s[i].ng);
	src = sift_f2s_source(p);
	sift_scope_build_pass2(b, &src);
	initStringInfo(&out);
	sift_scope_build_end(b, &out, NULL);
	ref = write_object(rel, cfg.ord, &out, gen, pm->nkeys);
	pfree(out.data);
	sift_spm_free(pm);
	MemoryContextDelete(p->gcx);
	return ref;
}

typedef SiftManifestScopeRef (*ScopeBuilder) (Relation heap, Relation rel, SiftF2Store *store, const SiftF2ScopeCfg *base, const SiftManifestRead *mr, const SiftManifestEdit *edit, Snapshot snap);

/* What one summary takes to build (the parts it opens, the pairs, the object) lives in a context of its own, the reference to the part it wrote is all that stays */
static SiftManifestScopeRef
build_scratch(ScopeBuilder one, Relation heap, Relation rel, SiftF2Store *store, const SiftF2ScopeCfg *base, const SiftManifestRead *mr, const SiftManifestEdit *edit, Snapshot snap)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope summary"),
				old = MemoryContextSwitchTo(cx);
	SiftManifestScopeRef ref = one(heap, rel, store, base, mr, edit, snap);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return ref;
}

/* The active summary of each scope ord below nscope (flags 0 for one that has none) */
static SiftManifestScopeRef *
current_refs(Relation rel, int nscope)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			n;
	SiftManifestScopeRef *old = sift_manifest_read_scopes(rel, &meta, &n);
	SiftManifestScopeRef *cur = palloc0(Max(nscope, 1) * sizeof(SiftManifestScopeRef));

	for (int i = 0; i < n; i++)
		if (old[i].scope_ord < nscope && (old[i].flags & SIFT_SCOPEREF_ACTIVE))
			cur[old[i].scope_ord] = old[i];
	return cur;
}

/* The extents of the summaries the directory names now and the replaced ones hold: released by the commit that names their successors */
static SiftExtent *
replaced_extents(Relation rel, int nscope, const bool *replaced, int *nrel)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nold;
	SiftManifestScopeRef *old = sift_manifest_read_scopes(rel, &meta, &nold);
	SiftExtent *release = NULL;

	*nrel = 0;
	for (int i = 0; i < nold; i++)
		if (old[i].header_blk != InvalidBlockNumber && old[i].scope_ord < nscope && replaced[old[i].scope_ord])
		{
			int			nb;
			SiftExtent *ext = sift_part_blocks(sift_part_open(rel, old[i].header_blk, old[i].meta_gen, 0), &nb);

			release = release ? repalloc(release, (*nrel + nb) * sizeof(SiftExtent)) : palloc(nb * sizeof(SiftExtent));
			memcpy(release + *nrel, ext, nb * sizeof(SiftExtent));
			*nrel += nb;
		}
	return release;
}

/* The directory names the new summaries; the blocks of the ones they replace are released in the same commit */
static void
commit_scopes(Relation rel, const SiftManifestRead *mr, SiftManifestScopeRef *refs, int nscope, const bool *replaced)
{
	int			nrel;
	SiftExtent *release = replaced_extents(rel, nscope, replaced, &nrel);

	SIFT_F2_GATE(SIFT_F2_GATE_SCOPE);
	sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = mr->entries[0].part_id,.key_lo = mr->entries[0].key_lo,.update_scopes = refs,.nupdate_scopes = nscope,
	.release = release,.nrelease = nrel});
}

void
sift_f2_scope_build(Relation heap, Relation rel)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	const int	nscope = sift_f2s_nscopes(store);
	SiftManifestRead mr = sift_f2_settled_manifest(rel, &store->spec);
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());
	SiftManifestScopeRef *refs = palloc(nscope * sizeof(SiftManifestScopeRef));
	bool	   *replaced = palloc(nscope * sizeof(bool));

	for (int s = 0; s < nscope; s++)
	{
		SiftF2ScopeCfg sc;

		refs[s] = (SiftManifestScopeRef) {0};
		replaced[s] = sift_f2s_supported(rel, store, s);
		if (!replaced[s])
			continue;
		sift_f2s_cfg(rel, store, s, &sc);
		refs[s] = build_scratch(build_one, heap, rel, store, &sc, &mr, NULL, snap);
	}
	commit_scopes(rel, &mr, refs, nscope, replaced);
	UnregisterSnapshot(snap);
}

/*
 * The refresh of a summary that is not what the parts and the pairs are now: when only the parts changed (a compaction retired some, an insert or a flush wrote others) the nodes with a retired ref
 * are built again and the new granules are added (sift_scope_refresh_parts); when only some pairs changed (the journal names their members) the nodes that may hold a changed member are built
 * again (sift_scope_refresh_dirty). Both at once, or pairs the journal cannot name, build the summary again. So do new parts of more rows than the summary holds: the refresh keeps the
 * nodes of the parts it names and adds those of the new ones, which a build over all of them packs several times tighter.
 */
#define REFRESH_MEMBERS_MAX	8192

SiftF2ScopeRefreshes sift_f2_scope_refreshes;

typedef struct PartDiff
{
	uint64	   *retired;
	uint32		nretired;
	SiftScopeNewPart *added;
	uint32		nadded;
	uint64		added_rows;
	uint64		kept_rows;
} PartDiff;

/* A summary that lacks only a tenth of the rows or less waits: writing it again costs its whole size, and the next flush may bring the rest */
#define SIFT_SCOPE_GROWTH_PCT	10

/* The granules and the rows of a part, read from its header in a context that goes at once: the parts of a relation are not kept open together */
static void
part_shape(Relation rel, const SiftPartDesc *d, uint32 part_id, uint32 *ngranules, uint64 *nrows)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift scope part shape");
	MemoryContext old = MemoryContextSwitchTo(scratch);
	const SiftPart *part = sift_part_open_desc(rel, d, part_id);

	*ngranules = part->hdr.ngranules;
	*nrows = part->hdr.nrows;
	MemoryContextSwitchTo(old);
	MemoryContextDelete(scratch);
}

static void
diff_parts(Relation rel, const SiftScopeMeta *old, const SiftManifestRead *mr, const SiftManifestEdit *edit, PartDiff *d)
{
	uint32		nman = (uint32) sift_f2s_parts_in_manifest(rel, mr, edit);
	bool	   *seen = palloc0(Max(old->hdr.nparts, 1));
	SiftGenMap	named;

	sift_f2s_meta_map(old, &named);
	*d = (PartDiff) {.retired = palloc(Max(old->hdr.nparts, 1) * sizeof(uint64)),.added = palloc(Max(nman, 1) * sizeof(SiftScopeNewPart))};
	for (int k = 0; k < mr->n; k++)
	{
		int			np;
		SiftPartDesc *parts = sift_f2s_entry_parts(rel, &mr->entries[k], edit, &np);

		for (int j = 0; j < np; j++)
		{
			uint32		ngranules,
						at;
			uint64		nrows;

			part_shape(rel, &parts[j], (uint32) mr->entries[k].part_id, &ngranules, &nrows);
			if (ngranules == 0)
				continue;
			if (!sift_f2s_gen_map_find(&named, parts[j].part_gen, &at))
			{
				d->added[d->nadded++] = (SiftScopeNewPart) {parts[j].part_gen, ngranules};
				d->added_rows += nrows;
			}
			else
			{
				seen[at] = true;
				d->kept_rows += nrows;
			}
		}
		pfree(parts);
	}
	for (uint32 e = 0; e < old->hdr.nparts; e++)
		if (old->parts[e].part_gen != 0 && !seen[e])
			d->retired[d->nretired++] = old->parts[e].part_gen;
	pfree(seen);
}

static int
slot_by_base(const void *a, const void *b)
{
	const SiftF2ScopeSlot *x = a,
			   *y = b;

	return x->base < y->base ? -1 : x->base > y->base;
}

/* The parts of the manifest where the summary numbers their granules: the ones it names at their bases, the new ones after its last */
static SiftF2ScopeParts *
refresh_parts(Relation heap, Relation rel, SiftF2Store *store, const SiftF2ScopeCfg *cfg, Snapshot snap, SiftScopePairMap *pm, const SiftManifestRead *mr, const SiftManifestEdit *edit, const SiftScopeMeta *old)
{
	SiftF2ScopeParts *p = sift_f2s_parts_new(rel, store, heap, cfg, snap, pm, sift_f2s_parts_in_manifest(rel, mr, edit));
	uint32		next = (uint32) old->hdr.total_granules;
	SiftGenMap	named;

	sift_f2s_meta_map(old, &named);
	for (int k = 0; k < mr->n; k++)
	{
		int			np;
		SiftPartDesc *parts = sift_f2s_entry_parts(rel, &mr->entries[k], edit, &np);

		for (int j = 0; j < np; j++)
		{
			uint32		base = next,
						ngranules,
						at;
			uint64		nrows;

			part_shape(rel, &parts[j], (uint32) mr->entries[k].part_id, &ngranules, &nrows);
			if (ngranules == 0)
				continue;
			if (sift_f2s_gen_map_find(&named, parts[j].part_gen, &at))
				base = old->parts[at].base_granule_idx;
			next += base == next ? ngranules : 0;
			(void) sift_f2s_parts_add(p, &parts[j], mr->entries[k].part_id, base, ngranules);
		}
		pfree(parts);
	}
	qsort(p->s, p->n, sizeof(SiftF2ScopeSlot), slot_by_base);
	return p;
}

/* The members of the journal, ascending and without repeats */
static uint32
sorted_members(int64 *v, uint32 n)
{
	uint32		u = 0;

	qsort(v, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (u == 0 || v[u - 1] != v[i])
			v[u++] = v[i];
	return u;
}

/* The summary after a compaction, pair edits, or both at once: each takes the one pass that rebuilds only the nodes it touches */
static void
refresh_object(const SiftScopeMeta *old, const SiftScopeBuildSpec *spec, SiftScopePairMap *pm, const PartDiff *diff, const SiftConnKeys *changed, const SiftScopeGranSource *src, StringInfo out)
{
	SiftScopePartSwap swap = {.old_gens = diff->retired,.nold = diff->nretired,.added = diff->added,.nadded = diff->nadded};
	SiftScopeDirty *dirty = NULL;
	const bool	parts = diff->nretired > 0 || diff->nadded > 0;
	uint32		nd = 0;

	if (changed->n > 0)
	{
		int64	   *members = memcpy(palloc(changed->n * sizeof(int64)), changed->keys, changed->n * sizeof(int64));

		nd = sift_scope_dirty_fast(old, pm, members, sorted_members(members, changed->n), &dirty);
	}
	if (parts && nd > 0)
	{
		sift_scope_refresh_combined(old, spec, pm, &swap, dirty, nd, src, out, NULL);
		sift_f2_scope_refreshes.combined++;
	}
	else if (parts)
	{
		sift_scope_refresh_parts(old, spec, pm, &swap, src, out, NULL);
		sift_f2_scope_refreshes.parts++;
	}
	else
	{
		sift_scope_refresh_dirty(old, spec, pm, dirty, nd, src, out, NULL);
		sift_f2_scope_refreshes.dirty++;
	}
}

static bool
rebuild_smaller(const PartDiff *d)
{
	return d->added_rows > d->kept_rows;
}

/* The parts added since the summary are left to the overlay of the reads while they are a tenth of the rows it names at most and fewer than half of the rows an overlay may hold */
static bool
overlay_serves(const PartDiff *d)
{
	return d->added_rows * 100 <= d->kept_rows * SIFT_SCOPE_GROWTH_PCT && d->added_rows < SIFT_SCOPE_OVERLAY_MAX_ROWS / 2;
}

static SiftManifestScopeRef
refresh_one(Relation heap, Relation rel, SiftF2Store *store, const SiftF2ScopeCfg *base, const SiftManifestRead *mr, const SiftManifestEdit *edit, Snapshot snap)
{
	SiftScopeMeta old;
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS] = {0};
	SiftConnKeys changed = {0};
	PartDiff	diff;
	SiftF2ScopeCfg cfg = *base;
	SiftScopeBuildSpec spec;
	SiftScopePairMap *pm;
	SiftF2ScopeParts *p;
	SiftScopeGranSource src;
	StringInfoData out;
	uint64		gen;
	SiftManifestScopeRef ref;
	bool		known;

	if (!buildable(heap, rel, store, base, mr, edit, snap))
		return (SiftManifestScopeRef) {0};
	if (!sift_f2s_object(rel, base->ord, &old, 0))
	{
		sift_f2_scope_refreshes.full++;
		return build_one(heap, rel, store, base, mr, edit, snap);
	}
	if (old.hdr.schema_hash != store->schema->hash)
	{
		sift_scope_meta_close(&old);
		sift_f2_scope_refreshes.full++;
		return build_one(heap, rel, store, base, mr, edit, snap);
	}
	diff_parts(rel, &old, mr, edit, &diff);
	for (int j = 0; j < sift_scope_njournals(base->spec); j++)
		memcpy(&stamp[j], old.hdr.stamps[j].raw, sizeof(SiftConnStamp));
	known = sift_scope_vouch(rel, base->ord, stamp, snap) == SIFT_CONN_OK &&
		sift_scope_changed(rel, base->ord, stamp, snap, REFRESH_MEMBERS_MAX, NULL, 0, NULL, &changed) == SIFT_CONN_OK && (changed.n == 0 || !sift_scope_member_text(rel, base->ord));
	if (known && changed.n == 0 && diff.nretired == 0 && diff.nadded > 0 && overlay_serves(&diff))
	{
		sift_scope_meta_close(&old);
		return (SiftManifestScopeRef) {0};
	}
	if (!known || (changed.n == 0 && diff.nretired == 0 && diff.nadded == 0) || rebuild_smaller(&diff))
	{
		sift_scope_meta_close(&old);
		sift_f2_scope_refreshes.full++;
		return build_one(heap, rel, store, base, mr, edit, snap);
	}
	sift_f2s_cfg_rings_of(&cfg, store, &old);
	gen = sift_manifest_new_gen(rel, 1);
	spec_init(rel, store, &cfg, snap, gen, &spec);
	spec.flags |= old.hdr.flags & (SIFT_SMETA_CLUSTERED | SIFT_SMETA_CODED_MEM);
	cfg.mem = sift_f2s_mem_open(heap, rel, store, &cfg, true);
	pm = sift_f2s_load_pairs(heap, rel, &cfg, snap, sift_f2s_key_map(heap, rel, cfg.spec, true), true);
	cfg.mem = sift_f2s_mem_open(heap, rel, store, &cfg, false);
	p = refresh_parts(heap, rel, store, &cfg, snap, pm, mr, edit, &old);
	src = sift_f2s_source(p);
	initStringInfo(&out);
	refresh_object(&old, &spec, pm, &diff, &changed, &src, &out);
	ref = write_object(rel, cfg.ord, &out, gen, pm->nkeys);
	pfree(out.data);
	sift_spm_free(pm);
	MemoryContextDelete(p->gcx);
	sift_scope_meta_close(&old);
	return ref;
}

static int
refresh_stale(Relation rel)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	const int	nscope = sift_f2s_nscopes(store);
	SiftManifestScopeRef *news = current_refs(rel, nscope);
	bool	   *replaced = palloc0(nscope * sizeof(bool));
	SiftManifestRead mr;
	Snapshot	snap;
	Relation	heap;
	int			stale = 0;

	for (int s = 0; s < nscope; s++)
		replaced[s] = sift_f2s_ready(rel, store, s) && !sift_f2_scope_current(rel, s);
	for (int s = 0; s < nscope; s++)
		stale += replaced[s];
	if (stale == 0)
		return 0;
	heap = sift_f2s_is_table(store) ? rel : table_open(sift_scope_fact_oid(rel), AccessShareLock);
	mr = sift_f2_settled_manifest(rel, &store->spec);
	snap = RegisterSnapshot(GetLatestSnapshot());
	for (int s = 0; s < nscope; s++)
	{
		SiftF2ScopeCfg sc;
		SiftManifestScopeRef ref;

		if (!replaced[s])
			continue;
		sift_f2s_cfg(rel, store, s, &sc);
		ref = build_scratch(refresh_one, heap, rel, store, &sc, &mr, NULL, snap);
		replaced[s] = ref.flags != 0;
		stale -= !replaced[s];
		if (replaced[s])
			news[s] = ref;
	}
	if (stale > 0)
		commit_scopes(rel, &mr, news, nscope, replaced);
	UnregisterSnapshot(snap);
	if (heap != rel)
		table_close(heap, NoLock);
	return stale;
}

bool
sift_f2_scope_blocks(Relation rel, int ord, SiftExtent **ext, int *n)
{
	SiftManifestScopeRef ref = current_refs(rel, ord + 1)[ord];

	if (!(ref.flags & SIFT_SCOPEREF_ACTIVE))
		return false;
	*ext = sift_part_blocks(sift_part_open(rel, ref.header_blk, ref.meta_gen, 0), n);
	return true;
}

void
sift_f2_scope_move(Relation rel, int ord)
{
	int			nscope = sift_f2s_nscopes(sift_f2_store_open(rel));
	SiftManifestScopeRef *refs = current_refs(rel, nscope);
	SiftPart   *part = sift_part_open(rel, refs[ord].header_blk, refs[ord].meta_gen, 0);
	StringInfoData obj = {0};
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	bool	   *replaced = palloc0(nscope * sizeof(bool));
	uint64		len;

	obj.data = sift_f2_scope_read_object(rel, part, &len);
	obj.len = (int) len;
	replaced[ord] = true;
	refs[ord] = write_object(rel, ord, &obj, sift_manifest_new_gen(rel, 1), (uint32) part->hdr.nrows);
	commit_scopes(rel, &mr, refs, nscope, replaced);
}

/* Whether the summary of scope ord names a part the edit drops: the compaction it belongs to leaves a summary that no part of the manifest answers for */
static bool
names_dropped(Relation rel, int ord, const SiftManifestEdit *edit)
{
	SiftScopeMeta old;
	bool		named = false;

	if (!sift_f2s_object(rel, ord, &old, 0))
		return false;
	for (uint32 i = 0; i < old.hdr.nparts && !named; i++)
		for (int d = 0; d < edit->ndrop && !named; d++)
			named = old.parts[i].part_gen == edit->drop_gens[d];
	sift_scope_meta_close(&old);
	return named;
}

static void
refresh_for(Relation rel, SiftManifestEdit *e)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	const int	nscope = sift_f2s_nscopes(store);
	SiftManifestScopeRef *news = current_refs(rel, nscope);
	bool	   *replaced = palloc0(nscope * sizeof(bool));
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	Snapshot	snap;
	Relation	heap;
	SiftExtent *release,
			   *all;
	int			nrel,
				nreplaced = 0;

	for (int s = 0; s < nscope; s++)
		replaced[s] = sift_f2s_ready(rel, store, s) && names_dropped(rel, s, e);
	heap = sift_f2s_is_table(store) ? rel : table_open(sift_scope_fact_oid(rel), AccessShareLock);
	snap = RegisterSnapshot(GetLatestSnapshot());
	for (int s = 0; s < nscope; s++)
	{
		SiftF2ScopeCfg sc;
		SiftManifestScopeRef ref;

		if (!replaced[s])
			continue;
		sift_f2s_cfg(rel, store, s, &sc);
		ref = build_scratch(refresh_one, heap, rel, store, &sc, &mr, e, snap);
		replaced[s] = ref.flags != 0;
		nreplaced += replaced[s];
		if (replaced[s])
			news[s] = ref;
	}
	UnregisterSnapshot(snap);
	if (heap != rel)
		table_close(heap, NoLock);
	if (nreplaced == 0)
		return;
	release = replaced_extents(rel, nscope, replaced, &nrel);
	all = palloc((e->nrelease + nrel + 1) * sizeof(SiftExtent));
	memcpy(all, e->release, e->nrelease * sizeof(SiftExtent));
	memcpy(all + e->nrelease, release, nrel * sizeof(SiftExtent));
	e->release = all;
	e->nrelease += nrel;
	e->update_scopes = news;
	e->nupdate_scopes = nscope;
}

/* A parallel operation (the index cleanup of a parallel VACUUM) cannot take the fresh snapshot a rebuild counts the rows of the parts by: the summaries stay as they are and the maintenance refreshes them */
int
sift_f2_scope_refresh(Relation rel)
{
	return IsInParallelMode() ? 0 : refresh_stale(rel);
}

void
sift_f2_scope_refresh_for(Relation rel, SiftManifestEdit *e)
{
	if (!IsInParallelMode())
		refresh_for(rel, e);
}
