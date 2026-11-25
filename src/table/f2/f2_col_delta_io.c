/*
 * table/f2/f2_col_delta_io.c - the chain of column delta pages on disk, and what the table asks of it: the plan of an UPDATE, the write of the stage, the read of a snapshot's overlay
 * and the verdict of a modify.
 *
 * The pages are read through the buffers and copied (no buffer stays pinned), changed only through generic WAL (core/xlog.h) and published by the manifest: the anchor of a partition
 * names the newest page. A page is appended to in place under the partition lock; a new page is allocated from the metadata slab and committed with the manifest.
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"

#include "col/f2/col_meta.h"
#include "core/cachebudget.h"
#include "core/crash_point.h"
#include "core/f2/f2_gate.h"
#include "core/f2/fsm.h"
#include "core/page_io.h"
#include "core/part_lock.h"
#include "core/worker.h"
#include "col/f2/f2_vacuum_run.h"
#include "col/f2/f2_store_schema.h"
#include "core/u64set.h"
#include "table/f2/f2_chain_cache.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_col_delta_dir.h"
#include "table/f2/f2_col_delta_io.h"
#include "table/f2/f2_gcache.h"

#ifdef SIFT_TESTING
bool		sift_test_col_delta = true;
#endif

/* ---- the transactions of a chain ---- */

static bool
op_visible(void *ctx, TransactionId xmin, CommandId cid)
{
	SIFT_INVARIANT(ctx != NULL, "a column delta was asked for its visibility without a snapshot");
	return sift_tbl_f2_delta_visible((SiftTblVis *) ctx, xmin, cid);
}

static bool
op_is_current(void *ctx pg_attribute_unused(), TransactionId xid)
{
	return TransactionIdIsCurrentTransactionId(xid);
}

static bool
op_in_progress(void *ctx pg_attribute_unused(), TransactionId xid)
{
	return TransactionIdIsInProgress(xid);
}

static bool
op_is_aborted(void *ctx pg_attribute_unused(), TransactionId xid)
{
	return !TransactionIdIsCurrentTransactionId(xid) && !TransactionIdDidCommit(xid) && !TransactionIdIsInProgress(xid);
}

void
sift_col_delta_ops_make(SiftColDeltaOps *ops, void *ctx, bool (*visible) (void *ctx, TransactionId xmin, CommandId cid))
{
	*ops = (SiftColDeltaOps) {.ctx = ctx,.visible = visible,.is_current = op_is_current,.in_progress = op_in_progress,.is_aborted = op_is_aborted};
}

void
sift_col_delta_ops_init(SiftColDeltaOps *ops, SiftTblVis *vis)
{
	sift_col_delta_ops_make(ops, vis, op_visible);
}

/* ---- the pages of a chain ---- */

/* A standby replays the records that recycle a page of the chain while a scan reads it: once the manifest has moved on that is a retry, not damage */
static void
retry_if_recycled(Relation rel, uint64 snap_manifest_ver)
{
	if (RecoveryInProgress() && (snap_manifest_ver == 0 || sift_col_meta_read(rel).manifest_version != snap_manifest_ver))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("sift table \"%s\": a column delta page was recycled during the scan, retry", RelationGetRelationName(rel)),
				 errhint("Set hot_standby_feedback = on, or keep sift.standby_conflicts on at the primary.")));
}

static const char *
read_typed(SiftColDeltaDiskReader *d, SiftPageType type, BlockNumber blk, char *dst)
{
	Buffer		buf = sift_blk_read(d->rel, blk, BUFFER_LOCK_SHARE);
	Page		page = BufferGetPage(buf);

	sift_page_expect_snap(d->rel, page, blk, type, 0, d->part_id, d->snap_manifest_ver);
	if (!sift_page_sum_ok(page))
		retry_if_recycled(d->rel, d->snap_manifest_ver);
	memcpy(dst, page, BLCKSZ);
	UnlockReleaseBuffer(buf);
	return NULL;
}

static const char *
read_page(void *ctx, BlockNumber blk, char *dst)
{
	return read_typed(ctx, SIFT_PT_COL_DELTA, blk, dst);
}

static const char *
read_dir_page(void *ctx, BlockNumber blk, char *dst)
{
	return read_typed(ctx, SIFT_PT_COL_DELTA_DIR, blk, dst);
}

SiftColDeltaPageReader
sift_col_delta_disk_reader(SiftColDeltaDiskReader *d)
{
	return (SiftColDeltaPageReader) {.read = read_page,.read_dir = read_dir_page,.ctx = d};
}

static bool
load_chain_pruned(Relation rel, const SiftPartitionEntry *entry, uint64 snap_manifest_ver, const SiftColDeltaPrune *prune, SiftColDeltaChain *chain)
{
	SiftColDeltaAnchor anchor;
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = (uint32) entry->part_id,.snap_manifest_ver = snap_manifest_ver};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	const char *why;

	memset(chain, 0, sizeof(*chain));
	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return false;
	anchor = sift_manifest_read_col_delta(rel, entry);
	why = sift_col_delta_chain_read_pruned(&anchor, &reader, prune, chain);
	if (why != NULL)
	{
		retry_if_recycled(rel, snap_manifest_ver);
		SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) entry->part_id);
	}
	return sift_col_delta_anchor_has_chain(&anchor);
}

static bool
load_chain(Relation rel, const SiftPartitionEntry *entry, uint64 snap_manifest_ver, SiftColDeltaChain *chain)
{
	return load_chain_pruned(rel, entry, snap_manifest_ver, NULL, chain);
}

bool
sift_col_delta_load_chain(Relation rel, const SiftPartitionEntry *entry, uint64 snap_manifest_ver, SiftColDeltaChain *chain)
{
	return load_chain(rel, entry, snap_manifest_ver, chain);
}

/* The visibility of a chain's records as one snapshot has it, and whether it can still change: a writer that is in progress, and that is not this transaction, may commit */
typedef struct SettledOps
{
	SiftTblVis *vis;
	TransactionId last_xid;
	bool		unsettled;
} SettledOps;

static bool
settled_visible(void *ctx, TransactionId xmin, CommandId cid)
{
	SettledOps *so = ctx;

	if (so->vis->snap->snapshot_type == SNAPSHOT_ANY && xmin != so->last_xid && TransactionIdIsNormal(xmin) && !TransactionIdIsCurrentTransactionId(xmin))
	{
		so->last_xid = xmin;
		so->unsettled |= TransactionIdIsInProgress(xmin);
	}
	return sift_tbl_f2_delta_visible(so->vis, xmin, cid);
}

static SiftColDeltaPartMap *
load_part_map(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaPrune *prune, uint32 ngranules, SiftTblVis *vis, uint64 snap_manifest_ver, bool *settled)
{
	SiftColDeltaChain chain;
	SiftColDeltaOps ops;
	SettledOps	so = {.vis = vis};
	SiftColDeltaPartMap *map;
	const char *why;

	if (!load_chain_pruned(rel, entry, snap_manifest_ver, prune, &chain))
		return NULL;
	sift_col_delta_ops_make(&ops, &so, settled_visible);
	map = sift_col_delta_build_part_map(&chain, prune->part_gen, ngranules, &ops, &why);
	if (why != NULL)
		SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) entry->part_id);
	if (settled != NULL)
		*settled = !so.unsettled;
	return map;
}

SiftColDeltaPartMap *
sift_col_delta_load_part_map(Relation rel, const SiftPartitionEntry *entry, uint64 part_gen, uint32 ngranules, SiftTblVis *vis, uint64 snap_manifest_ver)
{
	SiftColDeltaPrune prune = {.part_gen = part_gen};

	return load_part_map(rel, entry, &prune, ngranules, vis, snap_manifest_ver, NULL);
}

/* ---- the overlay of a scan ---- */

/* What the retained map of a scan was made for: while all of it holds, no row's patches the snapshot sees can have changed */
typedef struct MapKey
{
	uint64		part_id;
	uint64		part_gen;
	bool		one_granule;	/* the map holds the patches of the pages that can name granule only */
	uint32		granule;
	BlockNumber head_blk;		/* the newest page of the chain as the anchor names it, and its generation, last change and content: a page of a table that is not logged keeps LSN 0 */
	uint64		head_gen;
	XLogRecPtr	head_lsn;
	uint32		head_used;
	uint32		head_crc;
	uint64		snap_xcc;		/* the transactions that had completed when the snapshot was taken: an MVCC snapshot with the same count sees the same writers, however its address is reused */
	TransactionId snap_xmin;
	TransactionId snap_xmax;
	CommandId	curcid;
	SubTransactionId subxid;
} MapKey;

struct SiftColDeltaScan
{
	Relation	rel;
	SiftTblVis *vis;
	MemoryContext cx;
	bool		valid;			/* map answers for the granule asked: no reset since it was made */
	uint64		attnum_bloom;	/* 0: every attribute */
	bool		retained;		/* key says what map was made for: after a reset it is used again when nothing it depends on changed */
	MapKey		key;
	SiftColDeltaPartMap *map;
};

/* The scan's context ends, by an error or not: its map is given back */
static void
scan_release(void *arg)
{
	SiftColDeltaScan *ds = arg;

	sift_col_delta_part_map_unref(ds->map);
	ds->map = NULL;
}

SiftColDeltaScan *
sift_col_delta_scan_begin(Relation rel, SiftTblVis *vis, MemoryContext cx)
{
	SiftColDeltaScan *ds = MemoryContextAllocZero(cx, sizeof(SiftColDeltaScan));
	MemoryContextCallback *cb = MemoryContextAlloc(cx, sizeof(*cb));

	ds->rel = rel;
	ds->vis = vis;
	ds->cx = cx;
	cb->func = scan_release;
	cb->arg = ds;
	MemoryContextRegisterResetCallback(cx, cb);
	return ds;
}

void
sift_col_delta_scan_reset(SiftColDeltaScan *ds)
{
	ds->valid = false;
}

static void
scan_drop_map(SiftColDeltaScan *ds)
{
	sift_col_delta_part_map_unref(ds->map);
	ds->map = NULL;
	ds->retained = false;
	ds->valid = false;
}

void
sift_col_delta_scan_set_attnums(SiftColDeltaScan *ds, uint64 attnum_bloom)
{
	ds->attnum_bloom = attnum_bloom | sift_cd_att_bit(SIFT_CD_ATT_TID_REMAP);
	scan_drop_map(ds);
}

SiftColDeltaPartMap *
sift_col_delta_scan_part_map(const SiftColDeltaScan *ds)
{
	return ds->map;
}

/* The cells of column phys of the granule have just been decoded: the patches of the leaf that has it go over them */
static void
patch_decoded(SiftF2GranuleBuf *gb, int phys)
{
	const SiftF2Store *store = gb->store;
	const SiftF2ColMap *map = gb->pin->col_map;
	int			leaf = map == NULL ? phys : phys < map->phys_ncols ? map->leaf_of_phys[phys] : -1;
	SiftColDeltaCells cells;

	if (leaf < 0)
		return;
	cells = (SiftColDeltaCells) {.kind = store->kinds[leaf],.i = gb->col_i[phys],.f = gb->col_f[phys],.s = gb->col_s[phys],.isnull = gb->col_isnull[phys],
								 .tag = gb->col_tag[phys],.cx = gb->cx};
	(void) sift_col_delta_apply_cells(&cells, gb->patch_arg, store->leaf_attno[leaf]);
}

/* The key a map asked for now would have; false for a snapshot whose view of a record can change without the chain changing */
static bool
map_key_now(const SiftColDeltaScan *ds, const SiftPartitionEntry *entry, const SiftF2GranuleBuf *gb, bool one_granule, MapKey *k)
{
	Snapshot	snap = ds->vis->snap;
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(ds->rel, entry);
	Buffer		buf;
	Page		page;

	if (snap->snapshot_type != SNAPSHOT_ANY && (snap->snapshot_type != SNAPSHOT_MVCC || snap->snapXactCompletionCount == 0))
		return false;
	buf = sift_blk_read(ds->rel, a.delta_head_blkno, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	sift_page_expect_snap(ds->rel, page, a.delta_head_blkno, (a.flags & SIFT_CDLA_INDEXED_DIR) != 0 ? SIFT_PT_COL_DELTA_DIR : SIFT_PT_COL_DELTA, 0, (uint32) entry->part_id, 0);
	*k = (MapKey) {.part_id = entry->part_id,.part_gen = gb->pin->part_gen,.one_granule = one_granule,.granule = one_granule ? gb->granule_id : 0,.head_blk = a.delta_head_blkno,
		.head_gen = SiftPageGetOpaque(page)->gen,.head_lsn = PageGetLSN(page),.head_used = SiftPageGetUsed(page),.head_crc = *(const uint32 *) (SiftPageGetData(page) + SIFT_PAGE_CRC_OFF),
		.snap_xcc = snap->snapXactCompletionCount,.snap_xmin = snap->xmin,.snap_xmax = snap->xmax,.curcid = snap->curcid,.subxid = GetCurrentSubTransactionId()};
	UnlockReleaseBuffer(buf);
	return true;
}

static bool
map_key_same(const MapKey *a, const MapKey *b)
{
	return a->part_id == b->part_id && a->part_gen == b->part_gen && a->one_granule == b->one_granule && a->granule == b->granule && a->head_blk == b->head_blk &&
		a->head_gen == b->head_gen && a->head_lsn == b->head_lsn && a->head_used == b->head_used && a->head_crc == b->head_crc && a->snap_xcc == b->snap_xcc && a->snap_xmin == b->snap_xmin && a->snap_xmax == b->snap_xmax &&
		a->curcid == b->curcid && a->subxid == b->subxid;
}

/* The map is of the part the granule is in; a second granule of the part is a scan's, and reads the whole of it once. The first granule a scan asks of a part reads only the pages of the chain that can name it */
const SiftColDeltaGranMap *
sift_col_delta_scan_map(SiftColDeltaScan *ds, const SiftPartitionEntry *entry, const SiftF2GranuleBuf *gb)
{
	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return NULL;
	if (!ds->valid || ds->key.part_id != entry->part_id || ds->key.part_gen != gb->pin->part_gen || (ds->key.one_granule && ds->key.granule != gb->granule_id))
	{
		bool		same_part = ds->valid && ds->key.part_id == entry->part_id && ds->key.part_gen == gb->pin->part_gen;
		MapKey		now;
		bool		keyed = map_key_now(ds, entry, gb, !same_part, &now);

		if (ds->retained && keyed && map_key_same(&now, &ds->key))
			ds->valid = true;
		else
		{
			MemoryContext map_cx = sift_col_delta_part_map_context(ds->cx);
			MemoryContext old = MemoryContextSwitchTo(map_cx);
			SiftColDeltaPrune prune = {.part_gen = gb->pin->part_gen,.by_granule = !same_part,.granule_lo = gb->granule_id,.granule_hi = gb->granule_id,
								   .attnum_bloom = ds->attnum_bloom};
			bool		settled = false;
			SiftColDeltaPartMap *loaded;

			scan_drop_map(ds);
			loaded = load_part_map(ds->rel, entry, &prune, gb->pin->part->hdr.ngranules, ds->vis, 0, &settled);
			MemoryContextSwitchTo(old);
			ds->map = sift_col_delta_part_map_share(map_cx, loaded);
			ds->key = keyed ? now : (MapKey) {.part_id = entry->part_id,.part_gen = gb->pin->part_gen,.one_granule = !same_part,.granule = same_part ? 0 : gb->granule_id};
			ds->retained = keyed && settled;
			ds->valid = true;
		}
	}
	return ds->map != NULL ? sift_col_delta_gran_map(ds->map, gb->granule_id) : NULL;
}

void
sift_col_delta_scan_attach(SiftColDeltaScan *ds, const SiftPartitionEntry *entry, SiftF2GranuleBuf *gb)
{
	const SiftColDeltaGranMap *gm = sift_col_delta_scan_map(ds, entry, gb);

	if (gm == NULL)
		return;
	sift_col_delta_part_map_hold_in(gb->cx, ds->map);
	gb->patch_arg = gm;
	gb->patch = patch_decoded;
}

static SiftColDeltaTarget
target_of(const SiftF2Loc *loc, uint64 tid)
{
	return (SiftColDeltaTarget) {.part_gen = loc->gb->pin->part_gen,.granule_id = loc->gb->granule_id,.row_ordinal = loc->row,.tid = tid};
}

/* A chain the cache of the chains can keep whole in a quarter of its budget; a longer one is read for the records of other transactions only */
static bool
chain_fits_cache(Relation rel, const SiftPartitionEntry *entry)
{
	uint64		pages = Max(SIFT_CDDIR_FLAT_PAGES, (uint64) sift_backend_cache_kb * 1024 / (4 * BLCKSZ));

	return sift_manifest_read_col_delta(rel, entry).npages <= pages;
}

static SiftColDeltaLast
latest_of(Relation rel, const SiftF2Loc *loc, uint64 tid)
{
	SiftColDeltaOps ops;
	SiftColDeltaTarget t = target_of(loc, tid);
	const SiftColDeltaLog *log;

	sift_col_delta_ops_init(&ops, NULL);
	if ((loc->entry.flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return (SiftColDeltaLast) {0};
	if (!chain_fits_cache(rel, &loc->entry))
		return sift_col_delta_foreign_latest(rel, &loc->entry, &t, &ops);
	log = sift_col_delta_log(rel, &loc->entry);
	if (log == NULL)
		return (SiftColDeltaLast) {0};
	for (int i = sift_col_delta_log_newest(log, tid); i >= 0; i = sift_col_delta_log_prev(log, i))
	{
		SiftColDeltaEntryHdr h;

		memcpy(&h, sift_col_delta_log_rec(log, i), sizeof(h));
		if (sift_col_delta_rec_is_of_row(&h, &t) && !ops.is_aborted(ops.ctx, h.xmin))
			return (SiftColDeltaLast) {.found = true,.xid = h.xmin,.cid = h.cid};
	}
	return (SiftColDeltaLast) {0};
}

SiftColDeltaVerdict
sift_col_delta_modify_verdict(Relation rel, const SiftF2Loc *loc, uint64 tid, CommandId cid, Snapshot snap)
{
	SiftTblVis	vis;
	SiftColDeltaOps ops;
	SiftColDeltaLast last = latest_of(rel, loc, tid);

	sift_tbl_f2_vis_begin(&vis, rel, snap);
	sift_col_delta_ops_init(&ops, &vis);
	return sift_col_delta_judge(&rel->rd_locator, tid, cid, &last, &ops);
}

void
sift_col_delta_ack_latest(Relation rel, const SiftF2Loc *loc, uint64 tid, CommandId cid)
{
	SiftColDeltaLast last = latest_of(rel, loc, tid);

	if (last.found)
		sift_epq_ack_record(&rel->rd_locator, tid, cid, &last);
}

/* ---- the plan of an UPDATE ---- */

/* What a scope or a connect keeps of the rows: the member key, the time column and the columns named by cols(..) (all of them when none are), and every column of a connect (its key is not told from the others here) */
static Bitmapset *
protect_declared(Bitmapset *s, const SiftF2Store *store)
{
	for (int i = 0; i < store->nscopes; i++)
	{
		const struct SiftScopeDecl *d = &store->scopes[i];
		ListCell   *lc;

		s = bms_add_member(s, store->leaf_attno[d->member_leaf] - FirstLowInvalidHeapAttributeNumber);
		if (d->time_leaf >= 0)
			s = bms_add_member(s, store->leaf_attno[d->time_leaf] - FirstLowInvalidHeapAttributeNumber);
		for (int l = 0; l < store->ncols && d->spec->cols == NIL; l++)
			s = bms_add_member(s, store->leaf_attno[l] - FirstLowInvalidHeapAttributeNumber);
		foreach(lc, d->spec->cols)
			for (int l = 0; l < store->ncols; l++)
				if (strcmp(store->schema->cols[l].name, (const char *) lfirst(lc)) == 0)
					s = bms_add_member(s, store->leaf_attno[l] - FirstLowInvalidHeapAttributeNumber);
	}
	for (int l = 0; l < store->ncols && store->opts.connect[0] != '\0'; l++)
		s = bms_add_member(s, store->leaf_attno[l] - FirstLowInvalidHeapAttributeNumber);
	return s;
}

Bitmapset *
sift_col_delta_protected_attrs(const SiftF2Store *store)
{
	Bitmapset  *s = protect_declared(NULL, store);

	for (int k = 0; k < store->nkey; k++)
		s = bms_add_member(s, store->leaf_attno[store->key_cols[k]] - FirstLowInvalidHeapAttributeNumber);
	for (int l = 0; l < store->ncols; l++)
		if (store->att_first[store->leaf_attno[l]] != l)
			s = bms_add_member(s, store->leaf_attno[l] - FirstLowInvalidHeapAttributeNumber);
	return s;
}

static int
live_attrs(const SiftF2Store *store)
{
	int			n = 0;

	for (int a = 1; a <= store->natts; a++)
		n += store->att_first[a] >= 0;
	return n;
}

static bool
after_update_triggers(Relation rel)
{
	const TriggerDesc *td = rel->trigdesc;

	return td != NULL && (td->trig_update_after_row || td->trig_update_old_table || td->trig_update_new_table);
}

static bool
too_big(Form_pg_attribute att, Datum d)
{
	if (att->attlen > 0)
		return false;
	if (att->attlen == -1)
		return toast_raw_datum_size(d) > SIFT_COL_DELTA_MAX_PAYLOAD;
	return strlen(DatumGetCString(d)) + 1 > SIFT_COL_DELTA_MAX_PAYLOAD;
}

/* A value the record cannot carry: a null cell that keeps the datum in an exception (the NaN of a numeric) */
static bool
fits_cell(Form_pg_attribute att, Datum d)
{
	return !sift_f2_attr_may_have_exceptions(att) || !numeric_is_nan(DatumGetNumeric(d));
}

static uint8
time_flag_of(const SiftF2Store *store, int16 attnum)
{
	uint8		f = 0;

	for (int k = 0; k < SIFT_MANIFEST_TIME_COLS; k++)
		if (store->spec.time_cols[k] > 0 && store->leaf_attno[store->spec.time_cols[k] - 1] == attnum)
			f |= k == 0 ? SIFT_CDENT_FLAG_HAS_TIME0 : SIFT_CDENT_FLAG_HAS_TIME1;
	return f;
}

static bool
member_of(const Bitmapset *s, int attnum)
{
	return bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber, s);
}

typedef struct Diff
{
	int			nchanged;
	bool		hits_index;
	bool		hits_summarized;
	bool		hits_protected;
	bool		oversize;
} Diff;

static void
diff_row(Relation rel, SiftF2Store *store, TupleTableSlot *old, TupleTableSlot *new, SiftColDeltaPlan *plan, Diff *d)
{
	TupleDesc	desc = RelationGetDescr(rel);
	SiftTblF2Extra *x = sift_tbl_extra(store);

	slot_getallattrs(old);
	slot_getallattrs(new);
	for (int a = 1; a <= store->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);
		int			leaf = store->att_first[a];

		if (leaf < 0 || sift_tbl_f2_attr_equal(att, old->tts_values[a - 1], old->tts_isnull[a - 1], new->tts_values[a - 1], new->tts_isnull[a - 1]))
			continue;
		d->hits_index |= member_of(x->hot_blocking_attrs, a);
		d->hits_summarized |= member_of(x->summarized_attrs, a);
		d->hits_protected |= member_of(x->protected_attrs, a);
		d->oversize |= !new->tts_isnull[a - 1] && (too_big(att, new->tts_values[a - 1]) || !fits_cell(att, new->tts_values[a - 1]));
		plan->flags |= time_flag_of(store, (int16) a);
		if (d->nchanged < SIFT_COL_DELTA_MAX_COLS_PER_REC && !d->oversize)
		{
			SiftColDeltaCol *c = &plan->cols[d->nchanged];

			c->attnum = (int16) a;
			c->kind = store->kinds[leaf];
			c->cell = (SiftCell) {.isnull = new->tts_isnull[a - 1]};
			if (!c->cell.isnull)
				sift_f2_cell_of_datum(store, att, new->tts_values[a - 1], CurrentMemoryContext, false, &c->cell);
		}
		d->nchanged++;
	}
}

void
sift_col_delta_plan(Relation rel, SiftF2Store *store, const SiftF2Loc *loc, TupleTableSlot *old, TupleTableSlot *new, SiftColDeltaPlan *plan)
{
	Diff		d = {0};
	SiftColDeltaPolicyIn in = {0};
	SiftColDeltaStage *staged = sift_stage_get(rel, &store->spec)->host_stage;

	memset(plan, 0, sizeof(*plan));
	diff_row(rel, store, old, new, plan, &d);
	in.live_attrs = live_attrs(store);
	in.row_in_part = loc->gb != NULL && loc->gb->pin != NULL && loc->gb->pin->part_gen != 0;
	in.after_update_triggers = after_update_triggers(rel);
	in.nchanged = d.nchanged;
	in.hits_index = d.hits_index;
	in.hits_protected = d.hits_protected;
	in.payload_bytes = d.oversize || d.nchanged > SIFT_COL_DELTA_MAX_COLS_PER_REC ? UINT32_MAX : sift_col_delta_payload_bytes(plan->cols, d.nchanged);
	if (in.row_in_part && (loc->entry.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		in.chain_pages = sift_manifest_read_col_delta(rel, &loc->entry).npages;
	if (staged != NULL)
		in.staged_pages = (uint32) ((sift_col_delta_stage_part_bytes(staged, loc->entry.part_id) + SIFT_COL_DELTA_PAGE_ROOM - 1) / SIFT_COL_DELTA_PAGE_ROOM);
	plan->reason = sift_col_delta_admit(&in);
	if (plan->reason != SIFT_CD_OK)
		return;
	plan->nchanged = d.nchanged;
	plan->summarizing = d.hits_summarized;
	plan->part_id = loc->entry.part_id;
	memcpy(plan->key_lo, loc->entry.key_lo, SIFT_BOUND_BYTES);
	plan->target = target_of(loc, sift_tbl_tid_pack(&old->tts_tid));
}

void
sift_col_delta_stage_plan(Relation rel, const SiftColDeltaPlan *plan, CommandId cid)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftTxnStage *st = sift_stage_get(rel, &store->spec);
	uint8		rec[SIFT_COL_DELTA_MAX_REC_BYTES];
	uint8		flags = plan->flags | (GetCurrentTransactionNestLevel() > 1 ? SIFT_CDENT_FLAG_SUBXID : 0);
	uint32		n = sift_col_delta_encode(rec, sizeof(rec), &plan->target, GetCurrentTransactionId(), cid, flags | (plan->nchanged == 0 ? SIFT_CDENT_FLAG_NOOP : 0), plan->cols, plan->nchanged);

	SIFT_INVARIANT(n > 0, "an admitted column delta does not fit a record");
	if (st->host_stage == NULL)
	{
		/* MEM[MEM-TBL-16]: TopTransactionContext staged col_deltas (items[] + rec copies) */
		st->host_stage = MemoryContextAllocZero(TopTransactionContext, sizeof(SiftColDeltaStage));
		st->host_event = sift_col_delta_stage_event;
		st->host_holds_in = sift_col_delta_stage_holds_in;
	}
	sift_col_delta_stage_add(st->host_stage, plan->part_id, plan->key_lo, GetCurrentSubTransactionId(), rec);
	st->host_pending++;
	if (((const SiftColDeltaStage *) st->host_stage)->bytes >= (uint64) work_mem * 512)
		sift_col_delta_stage_event(rel, st, SIFT_STAGE_FLUSH, 0, 0);
}

/* ---- writing the stage ---- */

static SiftPartitionEntry
fresh_entry(Relation rel, uint64 part_id, const uint8 *key_lo)
{
	SiftPartitionEntry e;

	if (!sift_manifest_find_entry(rel, part_id, key_lo, &e))
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	return e;
}

/* A row whose part a compaction replaced since the record was staged is where its address says now; one whose part a TTL drop took is gone, and its record with it. The stage is the one being
 * flushed and still holds the row: the lookup leaves it alone (a lookup that flushes a stage holding its address would flush this one again) */
static void
relocate_stale(Relation rel, SiftF2Store *store, SiftColDeltaStage *cd, const int *ord, int lo, int hi, const SiftPartDesc *parts, int nparts)
{
	SiftU64Set	gens;

	sift_u64set_init(&gens, nparts);
	for (int p = 0; p < nparts; p++)
		(void) sift_u64set_add(&gens, parts[p].part_gen);
	for (int k = lo; k < hi; k++)
	{
		SiftStageColDelta *it = &cd->items[ord[k]];
		SiftColDeltaEntryHdr h;
		SiftF2Loc	loc;

		CHECK_FOR_INTERRUPTS();
		memcpy(&h, it->rec, sizeof(h));
		if (sift_u64set_has(&gens, h.part_gen))
			continue;
		if (!sift_f2_locate_published(rel, store, sift_col_delta_hdr_tid(&h), &loc))
		{
			sift_col_delta_stage_release(cd, ord[k]);
			continue;
		}
		SIFT_INVARIANT(loc.entry.part_id == it->part_id, "the row of a staged column delta is not in its partition");
		h.part_gen = loc.gb->pin->part_gen;
		h.granule_id = loc.gb->granule_id;
		h.row_ordinal = loc.row;
		memcpy(it->rec, &h, sizeof(h));
		sift_f2_gbuf_unref(loc.gb);
	}
	sift_u64set_free(&gens);
}

void
sift_col_delta_write_chain(Relation rel, const SiftPartitionEntry *entry, SiftColDeltaBatch *batches, int nbatches, FullTransactionId next, SiftColDeltaAnchor *anchor)
{
	SiftColDeltaDiskChain d = {.rel = rel,.entry = entry,.capture = anchor};
	SiftColDeltaWriter w = sift_col_delta_disk_writer(&d);
	uint32		pages = 0;

	for (int i = 0; i < nbatches; i++)
		pages += sift_col_delta_batch_pages(&batches[i]);
	if (pages > 0)
	{
		d.gen_next = sift_manifest_new_gen(rel, pages);
		d.gen_left = pages;
	}
	*anchor = sift_col_delta_anchor_none();
	for (int i = 0; i < nbatches; i++)
	{
		sift_col_delta_chain_append(&w, anchor, &batches[i], next);
		sift_col_delta_batch_free(&batches[i]);
	}
}

/* Everything of the batch into the chain of the partition, under its lock; the pages the chain has after */
static uint32
append_locked(Relation rel, const SiftPartitionEntry *e, SiftColDeltaBatch *b, FullTransactionId next)
{
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(rel, e);
	SiftColDeltaDiskChain d = {.rel = rel,.entry = e};
	SiftColDeltaWriter w = sift_col_delta_disk_writer(&d);

	sift_col_delta_chain_append(&w, &a, b, next);
	return a.npages;
}

static void
flush_stage(Relation rel, SiftColDeltaStage *cd)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	FullTransactionId next = ReadNextFullTransactionId();
	int			nlive;
	int		   *ord = sift_col_delta_stage_by_part(cd, &nlive);

	for (int lo = 0, hi; lo < nlive; lo = hi)
	{
		uint64		part_id = cd->items[ord[lo]].part_id;
		SiftPartitionEntry e;
		SiftPartDesc *parts;
		int			nparts;
		SiftColDeltaBatch b;
		uint32		npages = 0;

		CHECK_FOR_INTERRUPTS();
		for (hi = lo; hi < nlive && cd->items[ord[hi]].part_id == part_id; hi++)
			;
		LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		e = fresh_entry(rel, part_id, cd->items[ord[lo]].key_lo);
		parts = sift_manifest_read_parts(rel, &e, &nparts);
		relocate_stale(rel, store, cd, ord, lo, hi, parts, nparts);
		b = sift_col_delta_stage_take_run(cd, ord, lo, hi, next);
		if (b.nentries > 0)
			npages = append_locked(rel, &e, &b, next);
		SIFT_CRASH_POINT(f2_col_delta_post_append_pre_commit);
		SIFT_F2_GATE(SIFT_F2_GATE_COL_DELTA);
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		if (npages >= SIFT_COL_DELTA_FOLD_PAGES)
			(void) sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, part_id);
		sift_col_delta_batch_free(&b);
		pfree(parts);
	}
	pfree(ord);
	sift_col_delta_stage_clear(cd);
}

void
sift_col_delta_stage_event(Relation rel, SiftTxnStage *st, SiftStageEvent ev, SubTransactionId sub, SubTransactionId parent)
{
	SiftColDeltaStage *cd = st->host_stage;

	switch (ev)
	{
		case SIFT_STAGE_SUB_ABORT:
			sift_col_delta_stage_drop_sub(cd, sub);
			break;
		case SIFT_STAGE_SUB_COMMIT:
			sift_col_delta_stage_promote_sub(cd, sub, parent);
			break;
		case SIFT_STAGE_FREE:
			sift_col_delta_stage_free(cd);
			st->host_stage = NULL;
			break;
		case SIFT_STAGE_FLUSH_AT_END:
			sift_col_delta_stage_normalize_top(cd, GetTopTransactionId());
			(void) sift_col_delta_stage_coalesce(cd);
			flush_stage(rel, cd);
			st->host_pending = 0;
			break;
		/* STOR[STOR-TBL-07]: mid-txn col_delta stage coalesce (eliminates dup >=40B recs before 32-page cap) */
		case SIFT_STAGE_FLUSH:
			flush_stage(rel, cd);
			st->host_pending = 0;
			break;
	}
}

/* ---- vacuum ---- */

FullTransactionId
sift_col_delta_min_xid(Relation rel, const SiftPartitionEntry *entry)
{
	SiftColDeltaChain chain;
	FullTransactionId lo;

	if (!load_chain(rel, entry, 0, &chain))
		return InvalidFullTransactionId;
	lo = sift_col_delta_chain_min_xid(&chain);
	sift_col_delta_chain_free(&chain);
	return lo;
}

static int
gen_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

uint64 *
sift_col_delta_sorted_gens(const SiftPartDesc *parts, int nparts)
{
	uint64	   *gens = palloc(sizeof(uint64) * Max(nparts, 1));

	for (int i = 0; i < nparts; i++)
		gens[i] = parts[i].part_gen;
	qsort(gens, nparts, sizeof(uint64), gen_cmp);
	return gens;
}

/* A writer that began after the pass read the next id appends to the chain the pass reads: its id is newer than the next id of the pass, and a full id is made from an id no newer than the next id it is made by */
SiftHorizonStatus
sift_col_delta_horizon_status(void *ctx, TransactionId xid, FullTransactionId *fxid)
{
	SiftXidHorizonCache *hc = ctx;
	bool		began_after = !TransactionIdPrecedesOrEquals(xid, XidFromFullTransactionId(hc->next));

	*fxid = sift_xid_fxid_at(began_after ? ReadNextFullTransactionId() : hc->next, xid);
	return sift_xid_horizon_status(hc, xid);
}

/* The records that stay, put on fewer pages than the chain has, in the one change of the manifest that replaces the chain; false when they would not take fewer */
static bool
shrink_chain(Relation rel, const SiftPartitionEntry *e, const SiftColDeltaChain *chain, const SiftColDeltaCompactPlan *plan)
{
	FullTransactionId next = ReadNextFullTransactionId();
	SiftColDeltaAnchor anchor;
	SiftColDeltaBatch *batches;
	SiftExtent *release;
	int			nbatches,
				nrelease;

	batches = sift_col_delta_compact_carry(plan, next, NULL, &nbatches);
	if (nbatches >= chain->npages)
	{
		for (int i = 0; i < nbatches; i++)
			sift_col_delta_batch_free(&batches[i]);
		pfree(batches);
		return false;
	}
	sift_col_delta_write_chain(rel, e, batches, nbatches, next, &anchor);
	release = sift_col_delta_chain_extents(chain, &nrelease);
	sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = e->part_id,.key_lo = e->key_lo,.set_col_delta = true,.new_col_delta = anchor,.release = release,.nrelease = nrelease});
	pfree(release);
	pfree(batches);
	return true;
}

/*
 * Every record is an aborted writer's or a gone part's: the pages go and the partition has no chain, and no row of any part is written. False when a record stays; the records that stay
 * are then written onto fewer pages when the chain holds dead ones.
 */
bool
sift_col_delta_gc(Relation rel, uint64 part_id, const uint8 *key_lo, FullTransactionId oldest_fx)
{
	SiftPartitionEntry e;
	SiftColDeltaChain chain;
	SiftColDeltaCompactPlan plan;
	SiftPartDesc *parts;
	SiftExtent *release;
	SiftXidHorizonCache hc;
	SiftColDeltaHorizon hz = {.ctx = &hc,.status = sift_col_delta_horizon_status};
	uint64	   *live;
	int			nparts;
	bool		gone;

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	e = fresh_entry(rel, part_id, key_lo);
	if (!load_chain(rel, &e, 0, &chain))
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	parts = sift_manifest_read_parts(rel, &e, &nparts);
	live = sift_col_delta_sorted_gens(parts, nparts);
	sift_xid_horizon_init(&hc, oldest_fx);
	sift_col_delta_compact_plan(&chain, NULL, 0, live, nparts, &hz, UINT32_MAX, &plan);
	gone = plan.ncarry == 0;
	if (gone)
	{
		SiftManifestEdit edit = {.part_id = part_id,.key_lo = e.key_lo,.set_col_delta = true,.new_col_delta = sift_col_delta_anchor_none()};

		release = sift_col_delta_chain_extents(&chain, &edit.nrelease);
		edit.release = release;
		sift_manifest_commit(rel, &edit);
		pfree(release);
	}
	/* STOR[STOR-TBL-08]: partial col_delta chain compaction (reclaim up to 31/32 dead 8KiB pages when ncarry>0) */
	else if (plan.ndropped > 0)
		(void) shrink_chain(rel, &e, &chain, &plan);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	sift_col_delta_compact_plan_free(&plan);
	sift_col_delta_chain_free(&chain);
	pfree(live);
	pfree(parts);
	return gone;
}

/* The anchor and the entry flag say the same, and the chain they name is whole: what sift_manifest_check asks of a partition */
void
sift_col_delta_check(Relation rel, const SiftPartitionEntry *entry)
{
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(rel, entry);
	SiftColDeltaChain chain;
	bool		flagged = (entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0;

	if (flagged != sift_col_delta_anchor_has_chain(&a))
		SIFT_CORRUPT(rel, "Partition %llu says %s a column delta chain and its anchor says %s.", (unsigned long long) entry->part_id, flagged ? "it has" : "it has no",
					 flagged ? "it has none" : "it has one");
	if (load_chain(rel, entry, 0, &chain))
		sift_col_delta_chain_free(&chain);
}
