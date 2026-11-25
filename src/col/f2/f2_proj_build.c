/*
 * col/f2/f2_proj_build.c - the projections of a relation of format major 2 bound to its parts: the columns a spec may name, the shape of the parts of a projection, the rows of a base
 * part, and the refresh that writes the projection parts of the base parts that have none and commits them with their links.
 *
 * A refresh of a partition builds its projection parts without the partition's part lock, which would stop every writer of the partition for as long as the build: it takes the lock for the
 * commit, finds then which base parts are still there with the deletion vector they were built over (every commit that drops parts or changes a deletion vector takes the same lock) and links
 * those only, the others are released; the commit that adds the projection parts is an edit of the partition with no part change, only a delta of the projection directory.
 */
#include "core/host.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"
#include "storage/lmgr.h"

#include "col/f2/compact.h"
#include "col/f2/delvec.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/f2_part_idx.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_store.h"
#include "col/f2/part_format.h"
#include "core/crash_point.h"
#include "core/f2/fsm.h"
#include "core/memtrack.h"
#include "core/part_lock.h"

void
sift_proj_part_spec(const SiftF2Store *store, const SiftProjSpec *s, const SiftProjColumns *cols, SiftProjPartSpec *out)
{
	memset(out, 0, sizeof(*out));
	out->nlay = sift_proj_layout(s, cols, out->lay);
	for (int i = 0; i < out->nlay; i++)
	{
		bool		value = out->lay[i].role == SPR_KEY || out->lay[i].role == SPR_INCLUDE || out->lay[i].role == SPR_MIN || out->lay[i].role == SPR_MAX;

		out->kinds[i] = out->lay[i].kind;
		out->hints[i] = value ? store->hints[out->lay[i].leaf] : SIFT_CHINT_AUTO;
		out->scales[i] = value ? store->scales[out->lay[i].leaf] : 0;
	}
	out->spec.ncols = out->nlay;
	out->spec.kinds = out->kinds;
	out->spec.hints = out->hints;
	out->spec.scales = out->scales;
	out->spec.nsort = s->nkeys;
	out->spec.sort_cols = out->sort;
	for (int i = 0; i < s->nkeys; i++)
	{
		out->sort[i] = i;
		out->spec.sort_flags[i] = (s->key[i].desc ? SIFT_KEY_DESC : 0) | (s->key[i].nulls_first ? SIFT_KEY_NULLS_FIRST : 0);
	}
}

SiftProjColumns
sift_proj_columns_of_store(Relation rel, const SiftF2Store *store)
{
	TupleDesc	td = RelationGetDescr(rel);
	SiftProjColumn *col = palloc0(Max(store->ncols, 1) * sizeof(SiftProjColumn));

	for (int l = 0; l < store->ncols; l++)
	{
		int			attno = store->leaf_attno[l];
		bool		own = attno >= 1 && attno <= td->natts;
		Form_pg_attribute att = TupleDescAttr(td, own ? attno - 1 : 0);
		bool		keyed = store->key_leaf != NULL && store->key_leaf[l];
		bool		grouped = store->parent != NULL && store->parent[l] >= 0;
		bool		plain = own && !keyed && !grouped && att->atttypid != JSONBOID && !type_is_array(att->atttypid);

		col[l].name = plain ? pstrdup(NameStr(att->attname)) : psprintf("#%d", l);
		col[l].kind = store->kinds[l];
		col[l].usable = plain;
		col[l].time = !plain ? SPT_NONE : att->atttypid == TIMESTAMPOID ? SPT_TIMESTAMP : att->atttypid == DATEOID ? SPT_DATE : att->atttypid == TIMESTAMPTZOID ? SPT_TIMESTAMPTZ : SPT_NONE;
		col[l].numeric = plain && att->atttypid == NUMERICOID;
		col[l].scale = col[l].numeric ? store->scales[l] : 0;
		col[l].boolean = plain && att->atttypid == BOOLOID;
	}
	return (SiftProjColumns) {store->ncols, col};
}

typedef struct BuildRows
{
	const SiftProjSpec *spec;
	const SiftProjPartSpec *pps;
	SiftProjCube *cube;
	MemoryContext cube_cx;		/* what the cube holds: it is made and fed in it, so that its size is the size of the context */
	SiftRowBuf *out;
	SiftCell   *cells;
	int			ncols;
	uint64		budget;
	uint32		added;
	bool		where_exc;		/* a row holds a value its WHERE could not judge on the cells: the part is read from its base part */
	bool		over;			/* what the part holds in memory passed the budget of the refresh */
} BuildRows;

static void
add_out_row(BuildRows *b, uint64 tid, const SiftCell *cells)
{
	uint8		key[SIFT_BOUND_BYTES];

	sift_row_key(&b->pps->spec, cells, tid, key);
	sift_rowbuf_add(b->out, tid, FrozenTransactionId, 0, 0, key, cells, NULL);
}

static bool
cube_over(const BuildRows *b)
{
	return MemoryContextMemAllocated(b->cube_cx, true) > b->budget;
}

static void
build_row(BuildRows *b, const SiftRowBuf *rows, int r)
{
	for (int c = 0; c < b->ncols; c++)
		b->cells[c] = sift_rowbuf_cell(rows, c, r);
	if (b->spec->nwhere > 0)
	{
		b->where_exc |= !sift_proj_spec_preds_decidable(b->spec->where, b->spec->nwhere, b->cells);
		if (!sift_proj_spec_preds_pass(b->spec->where, b->spec->nwhere, b->cells))
			return;
	}
	if (b->cube != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(b->cube_cx);

		(void) sift_proj_cube_add(b->cube, b->cells, 1);
		MemoryContextSwitchTo(old);
		b->over = (++b->added & 255) == 0 && cube_over(b);
	}
	else
	{
		SiftCell   *kept = palloc(b->pps->nlay * sizeof(SiftCell));

		for (int i = 0; i < b->pps->nlay; i++)
			kept[i] = b->cells[b->pps->lay[i].leaf];
		add_out_row(b, rows->tid[r], kept);
		pfree(kept);
		b->over = b->out->mem_bytes > b->budget;
	}
}

/* A base part written under another schema of the table (a column added, dropped or retyped since) holds its columns elsewhere than the leaves of the store: it is projected once a compaction has written it again */
static bool
has_schema_of_store(Relation rel, const SiftF2Store *store, const SiftPartDesc *base, uint32 part_id)
{
	return store->host->build_col_map == NULL || store->host->build_col_map(store, rel, sift_part_open_desc(rel, base, part_id), CurrentMemoryContext) == NULL;
}

/*
 * The projection part of one base part, written and its link made, in the memory context the caller resets after it. false when the base part has no row left to project, or when what the
 * projection holds before it is written passes the budget: no projection part is made then, the base part keeps none and a read takes its rows from the base part, until a compaction cuts it
 * to a size the budget holds. The projection of a base part is one part, so that it is not cut.
 */
static bool
write_part(Relation rel, const SiftF2Store *store, const SiftManifestProjRef *ref, const SiftProjSpec *spec, const SiftProjColumns *cols, const SiftProjPartSpec *pps,
		   const SiftPartDesc *base, uint32 base_part_id, uint64 budget, SiftPartDesc *out, SiftProjLink *link)
{
	BuildRows	b = {.spec = spec,.pps = pps,.cube_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift projection cube"),.out = sift_rowbuf_new(&pps->spec),
		.cells = palloc(Max(store->ncols, 1) * sizeof(SiftCell)),.ncols = store->ncols,.budget = budget};
	SiftColIn  *in = palloc(pps->nlay * sizeof(SiftColIn));
	SiftProjCursor *cur = sift_proj_cursor_open(rel, base, base_part_id, &store->spec, false);
	const SiftRowBuf *rows;
	int			r;
	uint64		gen;
	SiftRows	rowset;
	SiftCompressTier tier;

	if (spec->kind == SIFT_PROJ_AGG)
	{
		MemoryContext old = MemoryContextSwitchTo(b.cube_cx);

		b.cube = sift_proj_cube_new(spec, cols);
		MemoryContextSwitchTo(old);
	}
	while (!b.over && sift_proj_cursor_next(cur, &rows, &r))
		build_row(&b, rows, r);
	sift_proj_cursor_close(cur);
	if (b.over)
		return false;
	if (b.cube != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(b.cube_cx);
		const SiftCell *cells;
		int			ncols;
		int			n = sift_proj_cube_rows(b.cube, &cells, &ncols);

		MemoryContextSwitchTo(old);
		for (int g = 0; g < n && b.out->mem_bytes <= budget; g++)
			add_out_row(&b, (uint64) g + 1, cells + (size_t) g * ncols);
		if (b.out->mem_bytes > budget)
			return false;
	}
	if (b.out->n == 0)
		return false;
	gen = sift_manifest_new_gen(rel, 1);
	sift_rowbuf_sort(b.out, &pps->spec);
	/* STOR[STOR-COL-03]: the projection part inherits the base part level and tier (cold base: cold codecs) */
	tier = base->level >= SIFT_MAX_PART_LEVEL ? SIFT_TIER_COLD_L2 : SIFT_TIER_WARM_L1;
	(void) sift_rowbuf_cluster(b.out, &pps->spec, tier);
	rowset = sift_rowbuf_rows(b.out, in, &pps->spec);
	*out = sift_part_write_rows(rel, SIFT_PROJ_PART_ID, gen, Max(base->level, 1), tier, &rowset);
	*link = sift_proj_link_make(ref, spec, base, gen, b.where_exc);
	return true;
}

/* The projection parts a refresh built for one projection, and the parts of the projection they replace (candidates: a replaced part goes only when the part that replaces it is committed) */
typedef struct Built
{
	uint64	   *cand_proj;
	uint64	   *cand_src;
	int			ncand;
	uint64	   *drop_proj;
	uint64	   *drop_src;
	int			ndrop;
	SiftPartDesc *parts;
	SiftProjLink *links;
	int			n;
	uint16		proj_id;
} Built;

static void
refresh_projection(Relation rel, const SiftF2Store *store, const SiftProjColumns *cols, const SiftManifestProjRef *ref, const SiftProjSpec *spec, const SiftPartDesc *base, int nbase,
				   uint64 base_part_id, uint64 budget, Built *out)
{
	SiftProjParts cur = sift_manifest_read_proj_parts(rel, ref);
	SiftProjRefreshPlan plan = sift_proj_refresh_plan(ref, base, nbase, &cur);
	SiftProjPartSpec *pps = palloc(sizeof(SiftProjPartSpec));
	MemoryContext part_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift projection part");
	SiftPartGenIndex by_gen;

	sift_proj_part_spec(store, spec, cols, pps);
	out->parts = palloc(Max(plan.nbuild, 1) * sizeof(SiftPartDesc));
	out->links = palloc(Max(plan.nbuild, 1) * sizeof(SiftProjLink));
	out->proj_id = ref->proj_id;
	sift_part_gen_idx_init(&by_gen, base, nbase);
	for (int i = 0; i < plan.nbuild; i++)
	{
		MemoryContext old = MemoryContextSwitchTo(part_cx);
		const SiftPartDesc *base_part = sift_part_gen_idx_find(&by_gen, plan.build_gens[i]);
		bool		made = has_schema_of_store(rel, store, base_part, (uint32) base_part_id) &&
		write_part(rel, store, ref, spec, cols, pps, base_part, (uint32) base_part_id, budget, &out->parts[out->n], &out->links[out->n]);

		MemoryContextSwitchTo(old);
		MemoryContextReset(part_cx);
		out->n += made;
	}
	MemoryContextDelete(part_cx);
	sift_part_gen_idx_free(&by_gen);
	out->ncand = plan.ndrop;
	out->cand_proj = memcpy(palloc(Max(plan.ndrop, 1) * sizeof(uint64)), plan.drop_proj_gens, plan.ndrop * sizeof(uint64));
	out->cand_src = memcpy(palloc(Max(plan.ndrop, 1) * sizeof(uint64)), plan.drop_src_gens, plan.ndrop * sizeof(uint64));
	sift_proj_refresh_plan_free(&plan);
}

/* What the refresh of a partition read of its base parts before it built from them: a projection part is linked only to a base part that is still there with the deletion vector it was built over */
typedef struct BaseSnap
{
	const SiftPartDesc *parts;
	int			nparts;
	uint32	   *dv_len;			/* the length of the stream of each part's deletion vector (a delta is appended to it without a new generation) */
} BaseSnap;

static BaseSnap
base_snap(Relation rel, const SiftPartDesc *parts, int nparts)
{
	BaseSnap	s = {.parts = parts,.nparts = nparts,.dv_len = palloc0(Max(nparts, 1) * sizeof(uint32))};

	for (int i = 0; i < nparts; i++)
		s.dv_len[i] = parts[i].dv_blk == InvalidBlockNumber ? 0 : sift_dv_stream_len(rel, parts[i].dv_blk);
	return s;
}

static bool
base_unchanged(const BaseSnap *was, const BaseSnap *now, uint64 src_gen)
{
	const SiftPartDesc *w = NULL,
			   *n = NULL;
	int			wi = 0,
				ni = 0;

	for (int i = 0; i < was->nparts && w == NULL; i++)
		if (was->parts[i].part_gen == src_gen)
		{
			w = &was->parts[i];
			wi = i;
		}
	for (int i = 0; i < now->nparts && n == NULL; i++)
		if (now->parts[i].part_gen == src_gen)
		{
			n = &now->parts[i];
			ni = i;
		}
	return w != NULL && n != NULL && w->dv_gen == n->dv_gen && w->dv_blk == n->dv_blk && was->dv_len[wi] == now->dv_len[ni];
}

static void
release_part(Relation rel, const SiftPartDesc *d)
{
	int			nb;
	SiftExtent *extents = sift_proj_part_extents(rel, d, &nb);

	sift_fsm_release(rel, extents, nb, InvalidFullTransactionId);
}

/* The parts built from a base part that changed since are released, the others stay; the parts of the projection they replace go with them */
static void
keep_unchanged(Relation rel, Built *b, const BaseSnap *was, const BaseSnap *now)
{
	SiftU64Set	built_src;
	int			kept = 0;

	for (int k = 0; k < b->n; k++)
	{
		if (base_unchanged(was, now, b->links[k].src_gen))
		{
			b->parts[kept] = b->parts[k];
			b->links[kept++] = b->links[k];
		}
		else
			release_part(rel, &b->parts[k]);
	}
	b->n = kept;
	b->ndrop = 0;
	b->drop_proj = palloc(Max(b->ncand, 1) * sizeof(uint64));
	b->drop_src = palloc(Max(b->ncand, 1) * sizeof(uint64));
	sift_u64_set_init(&built_src, b->n);
	for (int k = 0; k < b->n; k++)
		sift_u64_set_add(&built_src, b->links[k].src_gen);
	for (int i = 0; i < b->ncand; i++)
		if (sift_u64_set_has(&built_src, b->cand_src[i]))
		{
			b->drop_proj[b->ndrop] = b->cand_proj[i];
			b->drop_src[b->ndrop++] = b->cand_src[i];
		}
	sift_u64_set_free(&built_src);
}

static const SiftProjDef *
def_of(const SiftProjDef *defs, int ndefs, const SiftManifestProjRef *ref)
{
	for (int i = 0; i < ndefs; i++)
		if (strcmp(defs[i].name, ref->name) == 0 && defs[i].spec_ok && defs[i].hash == ref->spec_hash)
			return &defs[i];
	return NULL;
}

/* The partition is projected as it is found: no row of it is held up for the build, and the part lock is taken for the commit only, which first finds what changed since */
static int
refresh_partition(Relation rel, const SiftF2Store *store, const SiftProjColumns *cols, const SiftProjDef *defs, int ndefs, const SiftPartitionEntry *entry)
{
	uint64		budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2);
	SiftManifestRead mr = sift_manifest_read_range(rel, entry->key_lo, entry->key_hi);
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	SiftManifestProjRef *refs;
	int			nrefs,
				nbase,
				nnow,
				nd = 0,
				built = 0;
	SiftPartDesc *base,
			   *now;
	BaseSnap	was,
				after;
	SiftProjDelta *deltas;
	Built	   *bs;

	refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	if (mr.n != 1 || mr.entries[0].part_id != entry->part_id || nrefs == 0 || (mr.entries[0].flags & SIFT_PARTENTRY_HAS_COL_DELTA))
		return 0;
	base = sift_manifest_read_parts(rel, &mr.entries[0], &nbase);
	was = base_snap(rel, base, nbase);
	bs = palloc0(nrefs * sizeof(Built));
	for (int i = 0; i < nrefs; i++)
	{
		const SiftProjDef *def = refs[i].state == SIFT_PROJ_STALE ? NULL : def_of(defs, ndefs, &refs[i]);

		if (def != NULL)
			refresh_projection(rel, store, cols, &refs[i], &def->spec, base, nbase, entry->part_id, budget, &bs[i]);
	}
	LockPage(rel, SIFT_PART_LOCK_BLKNO(entry->part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, entry->key_lo, entry->key_hi);
	now = mr.n == 1 && mr.entries[0].part_id == entry->part_id && !(mr.entries[0].flags & SIFT_PARTENTRY_HAS_COL_DELTA) ? sift_manifest_read_parts(rel, &mr.entries[0], &nnow) : NULL;
	after = base_snap(rel, now, now != NULL ? nnow : 0);
	deltas = palloc(nrefs * sizeof(SiftProjDelta));
	for (int i = 0; i < nrefs; i++)
	{
		if (bs[i].parts != NULL)
			keep_unchanged(rel, &bs[i], &was, &after);
		if (bs[i].n == 0)
			continue;
		deltas[nd++] = (SiftProjDelta) {.proj_id = refs[i].proj_id, .drop_proj_gens = bs[i].drop_proj, .ndrop_parts = bs[i].ndrop, .add_parts = bs[i].parts, .nadd_parts = bs[i].n,
									  .drop_src_gens = bs[i].drop_src, .ndrop_links = bs[i].ndrop, .add_links = bs[i].links, .nadd_links = bs[i].n};
		built += bs[i].n;
	}
	if (nd > 0)
	{
		SiftManifestProjUpdate upd = {.deltas = deltas, .ndeltas = nd};
		SiftManifestEdit e = {.part_id = entry->part_id, .key_lo = entry->key_lo, .update_projs = &upd};

		SIFT_CRASH_POINT(f2_proj_refresh_post_p1_pre_p2);
		sift_manifest_commit(rel, &e);
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(entry->part_id), ExclusiveLock);
	return built;
}

static SiftManifestEdit
directory_edit(Relation rel, const SiftManifestProjUpdate *upd)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	return (SiftManifestEdit) {.part_id = mr.entries[0].part_id, .key_lo = mr.entries[0].key_lo, .update_projs = upd};
}

static uint16
free_proj_id(const SiftManifestProjRef *refs, int nrefs)
{
	uint16		id = 1;

	for (int i = 0; i < nrefs && refs[i].proj_id == id; i++)
		id++;
	if (id > SIFT_PROJ_ID_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: the relation has the most projections there can be (%d)", SIFT_PROJ_ID_MAX),
				 errhint("Drop a projection with sift.drop_projection() before creating another.")));
	return id;
}

static uint16
register_projection(Relation rel, const char *name, const SiftProjSpec *spec, uint32 spec_hash)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs;
	SiftManifestProjRef *refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	SiftProjDelta d = {.create = true};
	SiftManifestProjUpdate upd = {.deltas = &d, .ndeltas = 1};
	SiftManifestEdit e;

	d.proj_id = free_proj_id(refs, nrefs);
	d.ref.proj_id = d.proj_id;
	d.ref.kind = spec->kind;
	d.ref.state = SIFT_PROJ_BUILDING;
	d.ref.spec_hash = spec_hash;
	d.ref.flags = sift_proj_spec_flags(spec);
	strlcpy(d.ref.name, name, sizeof(d.ref.name));
	e = directory_edit(rel, &upd);
	sift_manifest_commit(rel, &e);
	return d.proj_id;
}

static void
set_ready(Relation rel, uint16 proj_id)
{
	SiftProjDelta d = {.proj_id = proj_id, .set_state = true, .state = SIFT_PROJ_READY};
	SiftManifestProjUpdate upd = {.deltas = &d, .ndeltas = 1};
	SiftManifestEdit e = directory_edit(rel, &upd);

	SIFT_CRASH_POINT(f2_proj_build_pre_commit);
	sift_manifest_commit(rel, &e);
}

/*
 * The directory follows sift.projection_defs, which is transactional and the directory is not: a projection that no definition names is what a rolled-back sift.create_projection or a
 * dropped definition leaves, and one whose definition parses to another spec or layout (the table changed a column it keeps) is a different projection. Both are dropped; the second is
 * entered again below.
 */
static void
drop_unmatched(Relation rel, const SiftProjDef *defs, int ndefs)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs;
	const SiftManifestProjRef *refs = sift_manifest_read_projs(rel, &meta, &nrefs);

	for (int i = 0; i < nrefs; i++)
	{
		const SiftProjDef *def = NULL;

		for (int k = 0; k < ndefs; k++)
			if (strcmp(defs[k].name, refs[i].name) == 0)
				def = &defs[k];
		if (def == NULL || (def->spec_ok && def->hash != refs[i].spec_hash))
			(void) sift_proj_drop(rel, refs[i].name);
	}
}

/* The definitions of sift.projection_defs the directory has no projection of (a restore, a REINDEX or a rewrite of the relation, the reset of an unlogged one: the directory starts empty) are entered again, building */
static int
register_lost(Relation rel, const SiftProjDef *defs, int ndefs, uint16 *ids)
{
	SiftColMetaPageData meta;
	int			nrefs,
				n = 0;
	const SiftManifestProjRef *refs;

	drop_unmatched(rel, defs, ndefs);
	meta = sift_col_meta_read(rel);
	refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	for (int i = 0; i < ndefs; i++)
		if (defs[i].spec_ok && sift_proj_find(refs, nrefs, defs[i].name) == NULL)
			ids[n++] = register_projection(rel, defs[i].name, &defs[i].spec, defs[i].hash);
	return n;
}

int
sift_proj_refresh(Relation rel, const SiftF2Store *store)
{
	int			ndefs,
				nlost,
				built = 0;
	SiftProjColumns cols = sift_proj_columns_of_store(rel, store);
	SiftProjDef *defs = sift_proj_defs_load(rel, &cols, &ndefs);
	uint16	   *lost = palloc(Max(ndefs, 1) * sizeof(uint16));
	SiftManifestRead mr;
	MemoryContext partition_cx;

	nlost = register_lost(rel, defs, ndefs, lost);
	if (sift_col_meta_read(rel).nprojs == 0)
		return 0;
	partition_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift projection partition");
	mr = sift_manifest_read_range(rel, NULL, NULL);
	for (int i = 0; i < mr.n; i++)
	{
		MemoryContext old = MemoryContextSwitchTo(partition_cx);

		built += refresh_partition(rel, store, &cols, defs, ndefs, &mr.entries[i]);
		MemoryContextSwitchTo(old);
		MemoryContextReset(partition_cx);
	}
	MemoryContextDelete(partition_cx);
	for (int i = 0; i < nlost; i++)
		set_ready(rel, lost[i]);
	sift_mem_release_os(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	return built;
}

bool
sift_proj_relation_has(Relation rel)
{
	return sift_col_meta_read(rel).nprojs > 0;
}

SiftExtent *
sift_proj_part_extents(Relation rel, const SiftPartDesc *d, int *n)
{
	return sift_part_blocks(sift_part_open(rel, d->header_blk, d->part_gen, SIFT_PROJ_PART_ID), n);
}

void
sift_proj_create(Relation rel, const SiftF2Store *store, const char *name, const SiftProjSpec *spec, const SiftProjColumns *cols, const char *canonical)
{
	uint16		proj_id = register_projection(rel, name, spec, sift_proj_spec_hash(canonical, spec, cols));

	(void) sift_proj_refresh(rel, store);
	set_ready(rel, proj_id);
}

bool
sift_proj_drop(Relation rel, const char *name)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs;
	SiftManifestProjRef *refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	const SiftManifestProjRef *ref = sift_proj_find(refs, nrefs, name);
	uint16		id;
	SiftManifestProjUpdate upd = {.drop_ids = &id, .ndrop = 1};
	SiftManifestEdit e;

	if (ref == NULL)
		return false;
	id = ref->proj_id;
	e = directory_edit(rel, &upd);
	sift_manifest_commit(rel, &e);
	SIFT_CRASH_POINT(f2_proj_drop_post_p2);
	return true;
}

static const SiftManifestProjRef *
ref_by_id(const SiftManifestProjRef *refs, int nrefs, uint16 id)
{
	for (int i = 0; i < nrefs; i++)
		if (refs[i].proj_id == id)
			return &refs[i];
	return NULL;
}

/* The commit of a move: the directory is left as it is when the projection, the part or its link is gone by then (a compaction or a drop got there first); true when it was applied */
static bool
commit_move(Relation rel, SiftProjDelta *d)
{
	SiftManifestProjUpdate upd = {.deltas = d,.ndeltas = 1};
	SiftManifestEdit e = directory_edit(rel, &upd);
	bool		applied = false;

	d->move_if_present = true;
	d->move_applied = &applied;
	sift_manifest_commit(rel, &e);
	return applied;
}

bool
sift_proj_move_part(Relation rel, uint16 proj_id, uint64 proj_gen)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs,
				nb;
	const SiftManifestProjRef *refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	const SiftManifestProjRef *ref = ref_by_id(refs, nrefs, proj_id);
	SiftProjParts cur;
	const SiftPartDesc *d = NULL;
	SiftProjLink *link = NULL;
	SiftPart   *src;
	SiftPartDesc moved;
	SiftProjLink moved_link;
	uint64		gen,
				granules,
				writer_bytes;
	SiftProjDelta delta;

	if (ref == NULL)
		return false;
	cur = sift_manifest_read_proj_parts(rel, ref);
	for (int i = 0; i < cur.nparts; i++)
		d = cur.parts[i].part_gen == proj_gen ? &cur.parts[i] : d;
	for (int i = 0; i < cur.nlinks; i++)
		link = cur.links[i].proj_gen == proj_gen ? &cur.links[i] : link;
	if (d == NULL || link == NULL)
		return false;
	src = sift_part_open_desc(rel, d, SIFT_PROJ_PART_ID);
	if (!sift_parts_joinable(rel, &src, 1))
		return false;
	gen = sift_manifest_new_gen(rel, 1);
	moved = sift_part_write_joined(rel, SIFT_PROJ_PART_ID, gen, d->level, &src, d, 1, NULL, &granules, &writer_bytes);
	moved_link = *link;
	moved_link.proj_gen = gen;
	delta = (SiftProjDelta) {.proj_id = proj_id,.drop_proj_gens = &proj_gen,.ndrop_parts = 1,.add_parts = &moved,.nadd_parts = 1,
		.drop_src_gens = &link->src_gen,.ndrop_links = 1,.add_links = &moved_link,.nadd_links = 1};
	if (commit_move(rel, &delta))
		return true;
	SiftExtent *extents = sift_part_blocks(sift_part_open_desc(rel, &moved, SIFT_PROJ_PART_ID), &nb);

	sift_fsm_release(rel, extents, nb, InvalidFullTransactionId);
	return false;
}

bool
sift_proj_move_dir(Relation rel, uint16 proj_id)
{
	SiftProjDelta delta = {.proj_id = proj_id};

	return commit_move(rel, &delta);
}
