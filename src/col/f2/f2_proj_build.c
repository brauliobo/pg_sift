/*
 * col/f2/f2_proj_build.c - the projections of a relation of format major 2 bound to its parts: the columns a spec may name, the shape of the parts of a projection, the rows of a base
 * part, and the refresh that writes the projection parts of the base parts that have none and commits them with their links.
 *
 * A refresh of a partition holds the partition's part lock from the read of its parts to the commit, so that the base parts it links cannot be dropped in between (every commit that
 * drops parts takes the same lock); the commit that adds the projection parts is an edit of the partition with no part change, only a delta of the projection directory.
 */
#include "core/host.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"
#include "storage/lmgr.h"

#include "col/f2/compact.h"
#include "col/f2/delvec.h"
#include "col/f2/f2_part_idx.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_store.h"
#include "col/f2/part_format.h"
#include "core/crash_point.h"
#include "core/f2/fsm.h"
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
	SiftRowBuf *out;
	SiftCell   *cells;
	int			ncols;
	bool		where_exc;		/* a row holds a value its WHERE could not judge on the cells: the part is read from its base part */
} BuildRows;

static void
add_out_row(BuildRows *b, uint64 tid, const SiftCell *cells)
{
	uint8		key[SIFT_BOUND_BYTES];

	sift_row_key(&b->pps->spec, cells, tid, key);
	sift_rowbuf_add(b->out, tid, FrozenTransactionId, 0, 0, key, cells, NULL);
}

static void
build_row(void *arg, const SiftRowBuf *rows, int r)
{
	BuildRows  *b = arg;

	for (int c = 0; c < b->ncols; c++)
		b->cells[c] = sift_rowbuf_cell(rows, c, r);
	if (b->spec->nwhere > 0)
	{
		b->where_exc |= !sift_proj_spec_preds_decidable(b->spec->where, b->spec->nwhere, b->cells);
		if (!sift_proj_spec_preds_pass(b->spec->where, b->spec->nwhere, b->cells))
			return;
	}
	if (b->cube != NULL)
		(void) sift_proj_cube_add(b->cube, b->cells, 1);
	else
	{
		SiftCell   *kept = palloc(b->pps->nlay * sizeof(SiftCell));

		for (int i = 0; i < b->pps->nlay; i++)
			kept[i] = b->cells[b->pps->lay[i].leaf];
		add_out_row(b, rows->tid[r], kept);
		pfree(kept);
	}
}

/* The projection part of one base part, written and its link made; false when the base part has no row left to project */
static bool
write_part(Relation rel, const SiftF2Store *store, const SiftManifestProjRef *ref, const SiftProjSpec *spec, const SiftProjColumns *cols, const SiftProjPartSpec *pps,
		   const SiftPartDesc *base, uint32 base_part_id, SiftPartDesc *out, SiftProjLink *link)
{
	BuildRows	b = {spec, pps, NULL, sift_rowbuf_new(&pps->spec), palloc(Max(store->ncols, 1) * sizeof(SiftCell)), store->ncols, false};
	SiftColIn  *in = palloc(pps->nlay * sizeof(SiftColIn));
	uint64		gen;
	SiftRows	rows;
	SiftCompressTier tier;

	if (spec->kind == SIFT_PROJ_AGG)
		b.cube = sift_proj_cube_new(spec, cols);
	sift_proj_scan_part(rel, base, base_part_id, &store->spec, false, build_row, &b);
	if (b.cube != NULL)
	{
		const SiftCell *cells;
		int			ncols;
		int			n = sift_proj_cube_rows(b.cube, &cells, &ncols);

		for (int g = 0; g < n; g++)
			add_out_row(&b, (uint64) g + 1, cells + (size_t) g * ncols);
		sift_proj_cube_free(b.cube);
	}
	if (b.out->n == 0)
	{
		sift_rowbuf_free(b.out);
		return false;
	}
	gen = sift_manifest_new_gen(rel, 1);
	sift_rowbuf_sort(b.out, &pps->spec);
	/* STOR[STOR-COL-03]: the projection part inherits the base part level and tier (cold base: cold codecs) */
	tier = base->level >= SIFT_MAX_PART_LEVEL ? SIFT_TIER_COLD_L2 : SIFT_TIER_WARM_L1;
	(void) sift_rowbuf_cluster(b.out, &pps->spec, tier);
	rows = sift_rowbuf_rows(b.out, in, &pps->spec);
	*out = sift_part_write_rows(rel, SIFT_PROJ_PART_ID, gen, Max(base->level, 1), tier, &rows);
	*link = sift_proj_link_make(ref, spec, base, gen, b.where_exc);
	sift_rowbuf_free(b.out);
	return true;
}

typedef struct Built
{
	uint64	   *drop_proj;
	uint64	   *drop_src;
	int			ndrop;
	SiftPartDesc *parts;
	SiftProjLink *links;
	int			n;
	uint16		proj_id;
} Built;

static int
refresh_projection(Relation rel, const SiftF2Store *store, const SiftProjColumns *cols, const SiftManifestProjRef *ref, const SiftProjSpec *spec, const SiftPartDesc *base, int nbase,
				   uint64 base_part_id, Built *out)
{
	SiftProjParts cur = sift_manifest_read_proj_parts(rel, ref);
	SiftProjRefreshPlan plan = sift_proj_refresh_plan(ref, base, nbase, &cur);
	SiftProjPartSpec *pps = palloc(sizeof(SiftProjPartSpec));
	SiftPartGenIndex by_gen;
	SiftU64Set	built_src;

	sift_proj_part_spec(store, spec, cols, pps);
	out->parts = palloc(Max(plan.nbuild, 1) * sizeof(SiftPartDesc));
	out->links = palloc(Max(plan.nbuild, 1) * sizeof(SiftProjLink));
	out->proj_id = ref->proj_id;
	sift_part_gen_idx_init(&by_gen, base, nbase);
	for (int i = 0; i < plan.nbuild; i++)
		if (write_part(rel, store, ref, spec, cols, pps, sift_part_gen_idx_find(&by_gen, plan.build_gens[i]), (uint32) base_part_id, &out->parts[out->n], &out->links[out->n]))
			out->n++;
	sift_part_gen_idx_free(&by_gen);
	out->ndrop = 0;
	out->drop_proj = palloc(Max(plan.ndrop, 1) * sizeof(uint64));
	out->drop_src = palloc(Max(plan.ndrop, 1) * sizeof(uint64));
	sift_u64_set_init(&built_src, out->n);
	for (int k = 0; k < out->n; k++)
		sift_u64_set_add(&built_src, out->links[k].src_gen);
	for (int i = 0; i < plan.ndrop; i++)
		if (sift_u64_set_has(&built_src, plan.drop_src_gens[i]))
		{
			out->drop_proj[out->ndrop] = plan.drop_proj_gens[i];
			out->drop_src[out->ndrop++] = plan.drop_src_gens[i];
		}
	sift_u64_set_free(&built_src);
	sift_proj_refresh_plan_free(&plan);
	return out->n;
}

static const SiftProjDef *
def_of(const SiftProjDef *defs, int ndefs, const SiftManifestProjRef *ref)
{
	for (int i = 0; i < ndefs; i++)
		if (strcmp(defs[i].name, ref->name) == 0 && defs[i].spec_ok && defs[i].hash == ref->spec_hash)
			return &defs[i];
	return NULL;
}

static int
refresh_partition(Relation rel, const SiftF2Store *store, const SiftProjColumns *cols, const SiftProjDef *defs, int ndefs, const SiftPartitionEntry *entry)
{
	int			built = 0;
	SiftManifestRead mr;
	SiftColMetaPageData meta;
	SiftManifestProjRef *refs;
	int			nrefs,
				nbase,
				nd = 0;
	SiftPartDesc *base;
	SiftProjDelta *deltas;
	Built	   *bs;

	LockPage(rel, SIFT_PART_LOCK_BLKNO(entry->part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, entry->key_lo, entry->key_hi);
	meta = sift_col_meta_read(rel);
	refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	if (mr.n != 1 || mr.entries[0].part_id != entry->part_id || nrefs == 0 || (mr.entries[0].flags & SIFT_PARTENTRY_HAS_COL_DELTA))
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(entry->part_id), ExclusiveLock);
		return 0;
	}
	base = sift_manifest_read_parts(rel, &mr.entries[0], &nbase);
	deltas = palloc(nrefs * sizeof(SiftProjDelta));
	bs = palloc0(nrefs * sizeof(Built));
	for (int i = 0; i < nrefs; i++)
	{
		const SiftProjDef *def = refs[i].state == SIFT_PROJ_STALE ? NULL : def_of(defs, ndefs, &refs[i]);

		if (def == NULL || refresh_projection(rel, store, cols, &refs[i], &def->spec, base, nbase, entry->part_id, &bs[nd]) == 0)
			continue;
		deltas[nd] = (SiftProjDelta) {.proj_id = refs[i].proj_id, .drop_proj_gens = bs[nd].drop_proj, .ndrop_parts = bs[nd].ndrop, .add_parts = bs[nd].parts, .nadd_parts = bs[nd].n,
									  .drop_src_gens = bs[nd].drop_src, .ndrop_links = bs[nd].ndrop, .add_links = bs[nd].links, .nadd_links = bs[nd].n};
		built += bs[nd].n;
		nd++;
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

int
sift_proj_refresh(Relation rel, const SiftF2Store *store)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs,
				ndefs,
				built = 0;
	SiftProjColumns cols;
	SiftProjDef *defs;
	SiftManifestRead mr;

	if (meta.nprojs == 0)
		return 0;
	(void) sift_manifest_read_projs(rel, &meta, &nrefs);
	cols = sift_proj_columns_of_store(rel, store);
	defs = sift_proj_defs_load(rel, &cols, &ndefs);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	for (int i = 0; i < mr.n; i++)
		built += refresh_partition(rel, store, &cols, defs, ndefs, &mr.entries[i]);
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

void
sift_proj_create(Relation rel, const SiftF2Store *store, const char *name, const SiftProjSpec *spec, const SiftProjColumns *cols, const char *canonical)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs;
	SiftManifestProjRef *refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	SiftProjDelta d = {.create = true};
	SiftManifestProjUpdate upd = {.deltas = &d, .ndeltas = 1};
	SiftManifestEdit e;

	(void) cols;
	d.proj_id = free_proj_id(refs, nrefs);
	d.ref.proj_id = d.proj_id;
	d.ref.kind = spec->kind;
	d.ref.state = SIFT_PROJ_BUILDING;
	d.ref.spec_hash = sift_proj_spec_hash(canonical);
	d.ref.flags = sift_proj_spec_flags(spec);
	strlcpy(d.ref.name, name, sizeof(d.ref.name));
	e = directory_edit(rel, &upd);
	sift_manifest_commit(rel, &e);
	(void) sift_proj_refresh(rel, store);
	d = (SiftProjDelta) {.proj_id = d.proj_id, .set_state = true, .state = SIFT_PROJ_READY};
	e = directory_edit(rel, &upd);
	SIFT_CRASH_POINT(f2_proj_build_pre_commit);
	sift_manifest_commit(rel, &e);
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
