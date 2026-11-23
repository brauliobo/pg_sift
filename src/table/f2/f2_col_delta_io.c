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
#include "core/crash_point.h"
#include "core/f2/f2_gate.h"
#include "core/f2/fsm.h"
#include "core/page_io.h"
#include "core/part_lock.h"
#include "core/worker.h"
#include "col/f2/f2_store_schema.h"
#include "core/u64set.h"
#include "table/f2/f2_chain_cache.h"
#include "table/f2/f2_col_delta.h"
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

typedef struct DiskReader
{
	Relation	rel;
	uint32		part_id;
	uint64		snap_manifest_ver;
} DiskReader;

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
read_page(void *ctx, BlockNumber blk, char *dst)
{
	DiskReader *d = ctx;
	Buffer		buf = sift_blk_read(d->rel, blk, BUFFER_LOCK_SHARE);
	Page		page = BufferGetPage(buf);

	sift_page_expect_snap(d->rel, page, blk, SIFT_PT_COL_DELTA, 0, d->part_id, d->snap_manifest_ver);
	if (!sift_page_sum_ok(page))
		retry_if_recycled(d->rel, d->snap_manifest_ver);
	memcpy(dst, page, BLCKSZ);
	UnlockReleaseBuffer(buf);
	return NULL;
}

static bool
load_chain(Relation rel, const SiftPartitionEntry *entry, uint64 snap_manifest_ver, SiftColDeltaChain *chain)
{
	SiftColDeltaAnchor anchor;
	DiskReader	d = {.rel = rel,.part_id = (uint32) entry->part_id,.snap_manifest_ver = snap_manifest_ver};
	SiftColDeltaPageReader reader = {.read = read_page,.ctx = &d};
	const char *why;

	memset(chain, 0, sizeof(*chain));
	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return false;
	anchor = sift_manifest_read_col_delta(rel, entry);
	why = sift_col_delta_chain_read(&anchor, &reader, chain);
	if (why != NULL)
	{
		retry_if_recycled(rel, snap_manifest_ver);
		SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) entry->part_id);
	}
	return sift_col_delta_anchor_has_chain(&anchor);
}

bool
sift_col_delta_load_chain(Relation rel, const SiftPartitionEntry *entry, uint64 snap_manifest_ver, SiftColDeltaChain *chain)
{
	return load_chain(rel, entry, snap_manifest_ver, chain);
}

SiftColDeltaPartMap *
sift_col_delta_load_part_map(Relation rel, const SiftPartitionEntry *entry, uint64 part_gen, uint32 ngranules, SiftTblVis *vis, uint64 snap_manifest_ver)
{
	SiftColDeltaChain chain;
	SiftColDeltaOps ops;
	SiftColDeltaPartMap *map;
	const char *why;

	if (!load_chain(rel, entry, snap_manifest_ver, &chain))
		return NULL;
	sift_col_delta_ops_init(&ops, vis);
	map = sift_col_delta_build_part_map(&chain, part_gen, ngranules, &ops, &why);
	if (why != NULL)
		SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) entry->part_id);
	return map;
}

/* ---- the overlay of a scan ---- */

struct SiftColDeltaScan
{
	Relation	rel;
	SiftTblVis *vis;
	MemoryContext cx;
	bool		valid;
	uint64		part_id;
	uint64		part_gen;
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
	sift_col_delta_part_map_unref(ds->map);
	ds->map = NULL;
	ds->valid = false;
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

const SiftColDeltaGranMap *
sift_col_delta_scan_map(SiftColDeltaScan *ds, const SiftPartitionEntry *entry, const SiftF2GranuleBuf *gb)
{
	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return NULL;
	if (!ds->valid || ds->part_id != entry->part_id || ds->part_gen != gb->pin->part_gen)
	{
		MemoryContext map_cx = sift_col_delta_part_map_context(ds->cx);
		MemoryContext old = MemoryContextSwitchTo(map_cx);
		SiftColDeltaPartMap *loaded;

		sift_col_delta_scan_reset(ds);
		loaded = sift_col_delta_load_part_map(ds->rel, entry, gb->pin->part_gen, gb->pin->part->hdr.ngranules, ds->vis, 0);
		MemoryContextSwitchTo(old);
		ds->map = sift_col_delta_part_map_share(map_cx, loaded);
		ds->part_id = entry->part_id;
		ds->part_gen = gb->pin->part_gen;
		ds->valid = true;
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

static SiftColDeltaLast
latest_of(Relation rel, const SiftF2Loc *loc, uint64 tid)
{
	const SiftColDeltaLog *log = sift_col_delta_log(rel, &loc->entry);
	SiftColDeltaOps ops;
	SiftColDeltaTarget t = target_of(loc, tid);

	if (log == NULL)
		return (SiftColDeltaLast) {0};
	sift_col_delta_ops_init(&ops, NULL);
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
		d->hits_index |= member_of(x->hot_blocking_attrs, a) || member_of(x->summarized_attrs, a);
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

typedef struct DiskChain
{
	Relation	rel;
	const SiftPartitionEntry *entry;
	SiftColDeltaAnchor *capture;	/* the anchor is the caller's to publish */
	uint64		gen_next;		/* generations reserved ahead for the pages of all the batches of a chain */
	uint32		gen_left;
} DiskChain;

static void
disk_commit_anchor(void *ctx, const SiftColDeltaAnchor *a)
{
	DiskChain  *d = ctx;
	SiftManifestEdit edit = {.part_id = d->entry->part_id,.key_lo = d->entry->key_lo,.set_col_delta = true,.new_col_delta = *a};

	if (d->capture != NULL)
		*d->capture = *a;
	else
		sift_manifest_commit(d->rel, &edit);
}

/* A row whose part a compaction replaced since the record was staged is where its address says now; one whose part a TTL drop took is gone, and its record with it */
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

		memcpy(&h, it->rec, sizeof(h));
		if (sift_u64set_has(&gens, h.part_gen))
			continue;
		if (!sift_f2_locate_cached(rel, store, sift_col_delta_hdr_tid(&h), &loc))
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

static bool
disk_append_head(void *ctx, BlockNumber head, SiftColDeltaBatch *b, FullTransactionId next)
{
	DiskChain  *d = ctx;
	Relation	rel = d->rel;
	uint32		part_id = (uint32) d->entry->part_id;
	Buffer		buf = sift_blk_read(rel, head, BUFFER_LOCK_EXCLUSIVE);
	Page		page = BufferGetPage(buf);
	SiftColDeltaBatch part;
	uint32		room;
	SiftXlog	x;
	bool		appended;

	sift_page_expect(rel, page, head, SIFT_PT_COL_DELTA, 0, part_id);
	if (!sift_page_sum_ok(page))
		SIFT_CORRUPT(rel, "Block %u of the column delta chain does not match its checksum.", head);
	room = (sift_col_delta_page_hdr(page)->flags & SIFT_CDPG_SEALED) != 0 ? 0 : SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page);
	sift_col_delta_batch_split(b, room, next, &part);
	appended = part.nentries > 0;
	if (!appended)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	x = sift_xlog_start(rel);
	SIFT_INVARIANT(sift_col_delta_page_append(sift_xlog_register(&x, buf, 0), &part), "a column delta batch that fit the page did not");
	sift_xlog_done(&x);
	sift_col_delta_batch_free(&part);
	return true;
}

static uint64
disk_reserve_gens(void *ctx, uint32 n)
{
	DiskChain  *d = ctx;
	uint64		first;

	if (d->gen_left < n)
		return sift_manifest_new_gen(d->rel, n);
	first = d->gen_next;
	d->gen_next += n;
	d->gen_left -= n;
	return first;
}

static BlockNumber
disk_new_page(void *ctx, const SiftColDeltaBatch *b, BlockNumber prev, uint16 depth, uint64 gen)
{
	DiskChain  *d = ctx;
	Relation	rel = d->rel;
	uint32		part_id = (uint32) d->entry->part_id;
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);
	SiftXlog	x = sift_xlog_start(rel);
	Page		page = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

	sift_col_delta_page_init(page, gen, part_id, prev, depth);
	SIFT_INVARIANT(sift_col_delta_page_append(page, b), "a column delta batch of one page did not fit an empty page");
	sift_xlog_done(&x);
	return blk;
}

void
sift_col_delta_write_chain(Relation rel, const SiftPartitionEntry *entry, SiftColDeltaBatch *batches, int nbatches, FullTransactionId next, SiftColDeltaAnchor *anchor)
{
	DiskChain	d = {.rel = rel,.entry = entry,.capture = anchor};
	SiftColDeltaWriter w = {.ctx = &d,.append_head = disk_append_head,.reserve_gens = disk_reserve_gens,.new_page = disk_new_page,.commit_anchor = disk_commit_anchor};

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

/* Everything of the batch into the chain of the partition, under its lock */
static void
append_locked(Relation rel, const SiftPartitionEntry *e, SiftColDeltaBatch *b, FullTransactionId next)
{
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(rel, e);
	DiskChain	d = {.rel = rel,.entry = e};
	SiftColDeltaWriter w = {.ctx = &d,.append_head = disk_append_head,.reserve_gens = disk_reserve_gens,.new_page = disk_new_page,.commit_anchor = disk_commit_anchor};

	sift_col_delta_chain_append(&w, &a, b, next);
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

		for (hi = lo; hi < nlive && cd->items[ord[hi]].part_id == part_id; hi++)
			;
		LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		e = fresh_entry(rel, part_id, cd->items[ord[lo]].key_lo);
		parts = sift_manifest_read_parts(rel, &e, &nparts);
		relocate_stale(rel, store, cd, ord, lo, hi, parts, nparts);
		b = sift_col_delta_stage_take_run(cd, ord, lo, hi, next);
		if (b.nentries > 0)
			append_locked(rel, &e, &b, next);
		SIFT_CRASH_POINT(f2_col_delta_post_append_pre_commit);
		SIFT_F2_GATE(SIFT_F2_GATE_COL_DELTA);
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
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

SiftHorizonStatus
sift_col_delta_horizon_status(void *ctx, TransactionId xid, FullTransactionId *fxid)
{
	SiftXidHorizonCache *hc = ctx;

	*fxid = sift_xid_fxid_at(hc->next, xid);
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
	int			nbatches;

	batches = sift_col_delta_compact_carry(plan, next, NULL, &nbatches);
	if (nbatches >= chain->npages)
	{
		for (int i = 0; i < nbatches; i++)
			sift_col_delta_batch_free(&batches[i]);
		pfree(batches);
		return false;
	}
	sift_col_delta_write_chain(rel, e, batches, nbatches, next, &anchor);
	release = palloc(sizeof(SiftExtent) * chain->npages);
	for (int i = 0; i < chain->npages; i++)
		release[i] = (SiftExtent) {chain->blks[i], 1};
	sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = e->part_id,.key_lo = e->key_lo,.set_col_delta = true,.new_col_delta = anchor,.release = release,.nrelease = chain->npages});
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

		release = palloc(sizeof(SiftExtent) * chain.npages);
		for (int i = 0; i < chain.npages; i++)
			release[i] = (SiftExtent) {chain.blks[i], 1};
		edit.release = release;
		edit.nrelease = chain.npages;
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
