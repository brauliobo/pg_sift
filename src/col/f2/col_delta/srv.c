/*
 * col/f2/col_delta/srv.c - the column delta chain of a sift_col index on the server (col/f2/col_delta/idx.h): the chain read for a snapshot, the hooks of the scan, and what the
 * AFTER UPDATE row trigger of track_changes stages instead of a tombstone and a new entry.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/f2.h"
#include "col/f2/cache.h"
#include "col/f2/col_delta/idx.h"
#include "col/f2/gbuf/gbuf.h"
#include "col/f2/meta.h"
#include "col/f2/proj/proj.h"
#include "col/f2/scope/scope.h"
#include "col/f2/store/store.h"
#include "col/f2/l0/stage_int.h"
#include "col/f2/part/vis.h"
#include "col/f2/scan/int.h"
#include "core/hash.h"
#include "core/memtrack.h"
#include "core/xid_cache.h"
#include "table/f2/col_delta/dir.h"

/* ---- what the chain asks of transactions ---- */

static bool
snapshot_sees(void *ctx, TransactionId xmin, CommandId cid)
{
	return sift_vis_inserted_cached(ctx, xmin, cid);
}

static bool
committed_or_current(void *ctx, TransactionId xmin, CommandId cid)
{
	return TransactionIdIsCurrentTransactionId(xmin) || TransactionIdDidCommit(xmin);
}

static bool
chain_of(Relation rel, const SiftPartitionEntry *e, SiftColDeltaChain *chain)
{
	return (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 && sift_col_delta_load_chain(rel, e, 0, chain);
}

SiftCdRemapMap *
sift_cdi_remap_load(Relation rel, const SiftPartitionEntry *entry)
{
	SiftColDeltaChain chain;
	SiftColDeltaOps ops;
	SiftCdRemapMap *rm;

	if (!chain_of(rel, entry, &chain))
		return NULL;
	sift_col_delta_ops_make(&ops, NULL, committed_or_current);
	rm = sift_cd_remap_build(&chain, &ops);
	sift_col_delta_chain_free(&chain);
	return rm;
}

/* ---- the scan ---- */

struct SiftCdiScan
{
	SiftXidCache xc;
	bool	   *pruned_leaf;
	SiftCdiMapSlot own;			/* the map of a scan that reads one part after another */
	const SiftColDeltaPartMap *map;	/* the map of the part being read: own's, or the one a cursor of an ordered scan keeps of its part */
};

static SiftCdiScan *
scan_state(F2Scan *s, const SiftColSchema *schema, Snapshot snap)
{
	MemoryContext old;

	if (s->cd != NULL)
		return s->cd;
	old = MemoryContextSwitchTo(s->cx);
	s->cd = palloc0(sizeof(SiftCdiScan));
	s->cd->pruned_leaf = palloc0(schema->ncols * sizeof(bool));
	for (int k = 0; k < s->npreds; k++)
		s->cd->pruned_leaf[s->preds[k].col] = true;
	if (s->bound != NULL)
		s->cd->pruned_leaf[s->sink_leaves[s->bound_sink_leaf]] = true;
	sift_xid_cache_init(&s->cd->xc, snap);
	s->cd->own.cx = s->cx;
	MemoryContextSwitchTo(old);
	return s->cd;
}

/* The pages of the chain that can hold a patch of the part (those the directory of the chain names for it, or for no one part): the other parts' are not read */
static bool
part_chain_of(Relation rel, const SiftPartitionEntry *e, uint64 part_gen, SiftColDeltaChain *chain)
{
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = (uint32) e->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	SiftColDeltaPrune prune = {.part_gen = part_gen};
	SiftColDeltaAnchor anchor;
	const char *why;

	memset(chain, 0, sizeof(*chain));
	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return false;
	anchor = sift_manifest_read_col_delta(rel, e);
	why = sift_col_delta_chain_read_pruned(&anchor, &reader, &prune, chain);
	if (why != NULL)
	{
		if (RecoveryInProgress())
			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("sift index \"%s\": a column delta page was recycled during the scan, retry", RelationGetRelationName(rel)),
							errhint("Set hot_standby_feedback = on, or keep sift.standby_conflicts on at the primary.")));
		SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) e->part_id);
	}
	return sift_col_delta_anchor_has_chain(&anchor);
}

SiftColDeltaPartMap *
sift_cdi_part_map(Relation rel, const SiftPartitionEntry *e, const SiftPart *part, SiftXidCache *xc)
{
	SiftColDeltaChain chain;
	SiftColDeltaOps ops;
	const char *why;
	SiftColDeltaPartMap *map = NULL;

	if (part_chain_of(rel, e, part->hdr.part_gen, &chain))
	{
		sift_col_delta_ops_make(&ops, xc, snapshot_sees);
		map = sift_col_delta_build_part_map(&chain, part->hdr.part_gen, part->hdr.ngranules, &ops, &why);
		if (why != NULL)
			SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) e->part_id);
	}
	return map;
}

static void
slot_drop(SiftCdiMapSlot *slot)
{
	sift_col_delta_part_map_free(slot->map);
	slot->map = NULL;
	slot->built = false;
}

/* The overlay of the part for the snapshot, kept in the slot for the granules of the part the scan reads next; NULL when the snapshot sees no patch of it */
static const SiftColDeltaPartMap *
part_map(SiftCdiScan *cd, SiftCdiMapSlot *slot, Relation rel, const SiftPartitionEntry *e, const SiftPart *part)
{
	MemoryContext old;

	if (slot->built && slot->part_id == e->part_id && slot->part_gen == part->hdr.part_gen)
		return slot->map;
	slot_drop(slot);
	slot->part_id = e->part_id;
	slot->part_gen = part->hdr.part_gen;
	old = MemoryContextSwitchTo(slot->cx);
	slot->map = sift_cdi_part_map(rel, e, part, &cd->xc);
	slot->built = true;
	MemoryContextSwitchTo(old);
	return slot->map;
}

const SiftVariantExcSummary *
sift_cdi_scan_exceptions(F2Scan *s, Relation rel, const SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPart *part, Snapshot snap, const SiftVariantExcSummary *exc, int *nexc)
{
	return sift_cdi_slot_exceptions(s, NULL, rel, schema, e, part, snap, exc, nexc);
}

const SiftVariantExcSummary *
sift_cdi_slot_exceptions(F2Scan *s, SiftCdiMapSlot *slot, Relation rel, const SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPart *part, Snapshot snap,
						 const SiftVariantExcSummary *exc, int *nexc)
{
	SiftCdiScan *cd;
	const SiftColDeltaPartMap *map;
	const SiftVariantExcSummary *out;
	MemoryContext old;

	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
	{
		if (s->cd != NULL)
		{
			slot_drop(&s->cd->own);
			s->cd->map = NULL;
		}
		return exc;
	}
	cd = scan_state(s, schema, snap);
	map = cd->map = part_map(cd, slot != NULL ? slot : &cd->own, rel, e, part);
	if (map == NULL)
		return exc;
	old = MemoryContextSwitchTo(s->cx);
	out = sift_cdi_exceptions(map, exc, *nexc, cd->pruned_leaf, schema->ncols, nexc);
	MemoryContextSwitchTo(old);
	return out;
}

static const SiftColDeltaGranMap *
scan_granule_map(const F2Scan *s, uint32 g)
{
	return s->cd != NULL && s->cd->map != NULL ? sift_col_delta_gran_map(s->cd->map, g) : NULL;
}


bool
sift_cdi_scan_patch(F2Scan *s, const SiftColSchema *schema, uint32 g, ColView *views, uint64 *tids, int n, MemoryContext cx)
{
	const SiftColDeltaGranMap *gm = scan_granule_map(s, g);

	if (gm == NULL)
		return false;
	sift_cdi_patch_granule(gm, schema->ncols, views, tids, n, cx);
	return sift_cdi_gran_remaps(gm);
}

void
sift_cdi_scan_remap_tids(const F2Scan *s, uint32 g, uint64 *tids, int n)
{
	const SiftColDeltaGranMap *gm = scan_granule_map(s, g);

	if (gm != NULL)
		sift_cdi_remap_tids(gm, tids, n);
}

bool
sift_cdi_scan_granule_patched(const F2Scan *s, uint32 g)
{
	return scan_granule_map(s, g) != NULL;
}

bool
sift_cdi_scan_remaps(const F2Scan *s, uint32 g)
{
	return sift_cdi_gran_remaps(scan_granule_map(s, g));
}

const SiftColDeltaPartMap *
sift_cdi_scan_swap_map(F2Scan *s, const SiftColDeltaPartMap *map)
{
	const SiftColDeltaPartMap *had;

	if (s->cd == NULL)
		return NULL;
	had = s->cd->map;
	s->cd->map = map;
	return had;
}

/* ---- the UPDATE ---- */

/*
 * What an UPDATE asks of the chain of a partition, kept for the next of the transaction: the address changes (a map built from every page of the chain), the pages the chain has, and the
 * bytes of the records the transaction staged for the partition. The chain is the same while the partition entry has the generation it was read at (an append to the chain is a commit of
 * the manifest), and the staged bytes are counted again when the stage's records are not the ones counted (a subtransaction ended, the rows were written).
 */
typedef struct CdCacheEntry
{
	uint64		part_id;
	uint64		manifest_gen;
	bool		loaded;
	uint32		chain_npages;
	SiftCdRemapMap *rm;
	uint64		staged_bytes;
	bool		staged_valid;
} CdCacheEntry;

struct SiftCdRemapCache
{
	MemoryContext cx;
	CdCacheEntry *entries;
	int			n;
	int			cap;
	uint32	   *ht;				/* the place in entries of a part_id, plus one */
	uint32		mask;
};

static struct SiftCdRemapCache *
cache_of(SiftTxnStage *st)
{
	struct SiftCdRemapCache *c = st->cd_cache;

	if (c != NULL)
		return c;
	c = MemoryContextAllocZero(TopTransactionContext, sizeof(*c));
	c->cx = AllocSetContextCreate(TopTransactionContext, "sift col delta remap cache", ALLOCSET_SMALL_SIZES);
	c->cap = 8;
	c->entries = MemoryContextAlloc(c->cx, c->cap * sizeof(CdCacheEntry));
	c->mask = 15;
	c->ht = MemoryContextAllocZero(c->cx, (c->mask + 1) * sizeof(uint32));
	st->cd_cache = c;
	return c;
}

static void
cache_grow(struct SiftCdRemapCache *c)
{
	c->mask = c->mask * 2 + 1;
	pfree(c->ht);
	c->ht = MemoryContextAllocZero(c->cx, (c->mask + 1) * sizeof(uint32));
	for (int i = 0; i < c->n; i++)
	{
		uint32		h = (uint32) (sift_fmix64(c->entries[i].part_id) & c->mask);

		while (c->ht[h] != 0)
			h = (h + 1) & c->mask;
		c->ht[h] = i + 1;
	}
}

static CdCacheEntry *
cache_entry(struct SiftCdRemapCache *c, uint64 part_id)
{
	uint32		h = (uint32) (sift_fmix64(part_id) & c->mask);

	for (; c->ht[h] != 0; h = (h + 1) & c->mask)
		if (c->entries[c->ht[h] - 1].part_id == part_id)
			return &c->entries[c->ht[h] - 1];
	if (c->n == c->cap)
	{
		c->cap *= 2;
		c->entries = repalloc(c->entries, c->cap * sizeof(CdCacheEntry));
	}
	c->entries[c->n] = (CdCacheEntry) {.part_id = part_id};
	c->ht[h] = ++c->n;
	if ((uint32) c->n * 2 > c->mask + 1)
		cache_grow(c);
	return &c->entries[c->n - 1];
}

void
sift_cdi_cache_staged_changed(SiftTxnStage *st)
{
	for (int i = 0; st->cd_cache != NULL && i < st->cd_cache->n; i++)
		st->cd_cache->entries[i].staged_valid = false;
}

void
sift_cdi_cache_free(SiftTxnStage *st)
{
	if (st->cd_cache == NULL)
		return;
	MemoryContextDelete(st->cd_cache->cx);
	pfree(st->cd_cache);
	st->cd_cache = NULL;
}

/* The entry of the partition as the chain is now: read again when the partition entry is of another generation than the one the map was built from */
static CdCacheEntry *
chain_entry(Relation index, SiftTxnStage *st, const SiftPartitionEntry *e)
{
	struct SiftCdRemapCache *c = cache_of(st);
	CdCacheEntry *ent = cache_entry(c, e->part_id);
	MemoryContext old;

	if (ent->loaded && ent->manifest_gen == e->manifest_gen)
		return ent;
	sift_cd_remap_free(ent->rm);
	old = MemoryContextSwitchTo(c->cx);
	ent->rm = sift_cdi_remap_load(index, e);
	MemoryContextSwitchTo(old);
	ent->chain_npages = (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 ? sift_manifest_read_col_delta(index, e).npages : 0;
	ent->manifest_gen = e->manifest_gen;
	ent->loaded = true;
	return ent;
}

typedef struct RowNow
{
	Relation	index;
	SiftTxnStage *st;
	uint64		tid;
} RowNow;

/* A row that moved on has left the address, and the heap may have given it to another row: of the rows of the address in the parts, the one that still has it */
static bool
not_moved_on(const SiftF2GranuleBuf *gb, const SiftPartitionEntry *entry, int row, void *arg)
{
	RowNow	   *n = arg;
	SiftCdRemapMap *rm = n->st != NULL ? chain_entry(n->index, n->st, entry)->rm : sift_cdi_remap_load(n->index, entry);
	bool		now = !sift_cd_remap_supersedes(rm, gb->pin->part_gen, gb->granule_id, row, n->tid);

	if (n->st == NULL)
		sift_cd_remap_free(rm);
	return now;
}

bool
sift_f2_locate_row_now(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	RowNow		n = {rel, NULL, tid};

	if (rel->rd_rel->relkind != RELKIND_INDEX)
		return sift_f2_locate_published(rel, store, tid, loc);
	return sift_f2_locate_published_if(rel, store, tid, not_moved_on, &n, loc);
}

static bool
locate_row(Relation index, SiftF2Store *store, SiftTxnStage *st, uint64 tid, SiftF2Loc *loc, CdCacheEntry **ent)
{
	RowNow		n = {index, st, tid};

	if (!sift_f2_locate_published_if(index, store, tid, not_moved_on, &n, loc))
		return false;
	*ent = chain_entry(index, st, &loc->entry);
	return true;
}

static void
key_absent_of(const SiftF2Rel *f, const SiftF2LeafDiff *d, bool *absent)
{
	for (int i = 0; i < d->nchanged; i++)
		absent[i] = f->key_leaf != NULL && f->key_leaf[d->leaf[i]] && !sift_key_row_present(d->cell[i].isnull, d->cell[i].tag);
}

static uint32
stage_pages(const SiftTxnStage *st, CdCacheEntry *ent)
{
	if (st->host_stage == NULL)
		return 0;
	if (!ent->staged_valid)
	{
		ent->staged_bytes = sift_col_delta_stage_part_bytes(st->host_stage, ent->part_id);
		ent->staged_valid = true;
	}
	return (uint32) ((ent->staged_bytes + SIFT_COL_DELTA_PAGE_ROOM - 1) / SIFT_COL_DELTA_PAGE_ROOM);
}

static void
stage_record(SiftTxnStage *st, CdCacheEntry *ent, const SiftPartitionEntry *e, const uint8 *rec)
{
	SiftColDeltaEntryHdr h;

	if (st->host_stage == NULL)
	{
		st->host_stage = MemoryContextAllocZero(TopTransactionContext, sizeof(SiftColDeltaStage));
		st->host_event = sift_col_delta_stage_event;
		st->host_holds_in = sift_col_delta_stage_holds_in;
	}
	memcpy(&h, rec, sizeof(h));
	sift_col_delta_stage_add(st->host_stage, e->part_id, e->key_lo, GetCurrentSubTransactionId(), rec);
	ent->staged_bytes += h.rec_bytes;
	st->host_pending++;
}

static void
flush_stage_if_full(Relation index, SiftTxnStage *st)
{
	if (((const SiftColDeltaStage *) st->host_stage)->bytes < SIFT_COL_DELTA_STAGE_BYTES)
		return;
	sift_col_delta_stage_event(index, st, SIFT_STAGE_FLUSH, 0, 0);
	sift_cdi_cache_staged_changed(st);
}

/* Only an update whose new row is still staged can become a column delta; one an earlier flush of the statement wrote takes the delete path, before any diff or part is read */
static bool
new_row_staged(Relation index, uint64 tid)
{
	bool		staged = false;

	sift_stage_rows_among(index, &tid, 1, &staged);
	return staged;
}

/* A leaf of the key of a UNIQUE index changed: the new key must be looked up as a new entry, not patched into the old one */
static bool
unique_key_changed(Relation index, const SiftF2LeafDiff *d)
{
	const SiftColSchema *schema = sift_col_rel(index)->schema;	/* check-memory: allow nothing below reads the catalog */

	for (int i = 0; i < d->nchanged && index->rd_index->indisunique; i++)
		if (schema->cols[d->leaf[i]].attno <= IndexRelationGetNumberOfKeyAttributes(index))
			return true;
	return false;
}

/*
 * The leaves the projections of an index read, as a bloom of their numbers (a false hit refuses a patch, never admits one): the definitions are a scan of the catalog, so they are read once
 * for each generation of the projection directory that Block 0 names.
 */
typedef struct ProjLeaves
{
	RelFileLocator locator;
	uint64		gen;
	uint64		bloom;
} ProjLeaves;

static ProjLeaves proj_leaves;

static uint64
leaf_bit(int leaf)
{
	return UINT64CONST(1) << (leaf % 64);
}

static uint64
load_proj_leaves(Relation index)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift projected leaves"),
				old = MemoryContextSwitchTo(cx);
	SiftProjColumns cols = sift_proj_columns_of_store(index, sift_f2_store_open(index));
	bool	   *used = palloc(Max(cols.n, 1) * sizeof(bool));
	int			ndefs;
	SiftProjDef *defs = sift_proj_defs_load(index, &cols, &ndefs);
	uint64		bloom = 0;

	for (int i = 0; i < ndefs; i++)
	{
		if (!defs[i].spec_ok)
		{
			bloom = ~UINT64CONST(0);
			continue;
		}
		sift_proj_spec_uses(&defs[i].spec, &cols, used);
		for (int c = 0; c < cols.n; c++)
			bloom |= used[c] ? leaf_bit(c) : 0;
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return bloom;
}

static bool
projected_leaf_changed(Relation index, const SiftF2LeafDiff *d)
{
	SiftColMetaPageData meta = sift_col_meta_read(index);

	if (meta.nprojs == 0)
		return false;
	if (!RelFileLocatorEquals(proj_leaves.locator, index->rd_locator) || proj_leaves.gen != meta.proj_dir_gen)
		proj_leaves = (ProjLeaves) {.locator = index->rd_locator,.gen = meta.proj_dir_gen,.bloom = load_proj_leaves(index)};
	for (int i = 0; i < d->nchanged; i++)
		if (proj_leaves.bloom & leaf_bit(d->leaf[i]))
			return true;
	return false;
}

/* PostgreSQL hints indexUnchanged from the key columns only: an entry with INCLUDE columns may still have changed leaves */
static bool
entry_unchanged(Relation index, bool index_unchanged)
{
	return index_unchanged && IndexRelationGetNumberOfKeyAttributes(index) == IndexRelationGetNumberOfAttributes(index);
}

bool
sift_cdi_stage_update(const SiftF2DiffEnv *env, SiftTxnStage *st, HeapTuple oldt, HeapTuple newt, uint64 old_tid, bool index_unchanged)
{
	SiftF2Rel  *f = env->f;
	SiftF2Store *store = sift_f2_store_open(env->index);
	SiftF2DiffResult res;
	SiftF2Loc	loc;
	CdCacheEntry *ent;
	SiftF2AdmitIn in;
	SiftColDeltaTarget target;
	bool		absent[SIFT_F2_DIFF_MAX_LEAVES];
	uint8		rec[SIFT_COL_DELTA_MAX_REC_BYTES];
	uint32		len;
	const uint64 new_tid = sift_f2_tid(&newt->t_self);

	if (!new_row_staged(env->index, new_tid))
		return false;
	if (entry_unchanged(env->index, index_unchanged))
		res = (SiftF2DiffResult) {.pred = SIFT_PRED_BOTH};
	else
		sift_f2_diff_update(env, oldt, newt, &res);
	if (res.pred != SIFT_PRED_BOTH || res.key_changed || res.leaves.rejected || unique_key_changed(env->index, &res.leaves) || projected_leaf_changed(env->index, &res.leaves) ||
		sift_f2_scope_reads_any(env->index, res.leaves.leaf, res.leaves.nchanged) || !locate_row(env->index, store, st, old_tid, &loc, &ent))
		return false;
	target = (SiftColDeltaTarget) {.part_gen = loc.gb->pin->part_gen,.granule_id = loc.gb->granule_id,.row_ordinal = loc.row,.tid = old_tid};
	key_absent_of(f, &res.leaves, absent);
	len = sift_cdi_encode(rec, sizeof(rec), &target, GetCurrentTransactionId(), GetCurrentCommandId(true), GetCurrentTransactionNestLevel() > 1, sift_cdi_time_flags(f->spec.time_cols, res.leaves.leaf, res.leaves.nchanged), new_tid, f->kinds,
						  res.leaves.leaf, res.leaves.cell, absent, res.leaves.nchanged);
	in = (SiftF2AdmitIn) {.tracked = true,.pred = res.pred,.nchanged_leaves = res.leaves.nchanged,.total_leaves = f->ncols,.payload_bytes = len,
		.chain_pages = ent->chain_npages,.staged_pages = stage_pages(st, ent)};
	if (len == 0 || sift_f2_col_delta_admit_index(&in) != SIFT_F2A_OK || !sift_stage_cancel_tid(st, new_tid))
	{
		sift_f2_gbuf_unref(loc.gb);
		return false;
	}
	stage_record(st, ent, &loc.entry, rec);
	sift_f2_gbuf_unref(loc.gb);
	flush_stage_if_full(env->index, st);
	return true;
}
