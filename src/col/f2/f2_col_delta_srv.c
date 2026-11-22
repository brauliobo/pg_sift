/*
 * col/f2/f2_col_delta_srv.c - the column delta chain of a sift_col index on the server (col/f2/f2_col_delta_idx.h): the chain read for a snapshot, the hooks of the scan, and what the
 * AFTER UPDATE row trigger of track_changes stages instead of a tombstone and a new entry.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_store.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_vis.h"
#include "col/f2/scan/scan_int.h"
#include "core/xid_cache.h"

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
	bool	   *pred_leaf;
	uint64		part_id;
	uint64		part_gen;
	SiftColDeltaPartMap *map;
};

static SiftCdiScan *
scan_state(F2Scan *s, const SiftColSchema *schema, Snapshot snap)
{
	MemoryContext old;

	if (s->cd != NULL)
		return s->cd;
	old = MemoryContextSwitchTo(s->cx);
	s->cd = palloc0(sizeof(SiftCdiScan));
	s->cd->pred_leaf = palloc0(schema->ncols * sizeof(bool));
	for (int k = 0; k < s->npreds; k++)
		s->cd->pred_leaf[s->preds[k].col] = true;
	sift_xid_cache_init(&s->cd->xc, snap);
	MemoryContextSwitchTo(old);
	return s->cd;
}

SiftColDeltaPartMap *
sift_cdi_part_map(Relation rel, const SiftPartitionEntry *e, const SiftPart *part, SiftXidCache *xc)
{
	SiftColDeltaChain chain;
	SiftColDeltaOps ops;
	const char *why;
	SiftColDeltaPartMap *map = NULL;

	if (chain_of(rel, e, &chain))
	{
		sift_col_delta_ops_make(&ops, xc, snapshot_sees);
		map = sift_col_delta_build_part_map(&chain, part->hdr.part_gen, part->hdr.ngranules, &ops, &why);
		if (why != NULL)
			SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) e->part_id);
	}
	return map;
}

/* The overlay of the part for the snapshot, kept for the granules of the part the scan reads next; NULL when the snapshot sees no patch of it */
static const SiftColDeltaPartMap *
part_map(F2Scan *s, SiftCdiScan *cd, Relation rel, const SiftPartitionEntry *e, const SiftPart *part)
{
	MemoryContext old;

	if (cd->map != NULL && cd->part_id == e->part_id && cd->part_gen == part->hdr.part_gen)
		return cd->map;
	sift_col_delta_part_map_free(cd->map);
	cd->part_id = e->part_id;
	cd->part_gen = part->hdr.part_gen;
	old = MemoryContextSwitchTo(s->cx);
	cd->map = sift_cdi_part_map(rel, e, part, &cd->xc);
	MemoryContextSwitchTo(old);
	return cd->map;
}

const SiftVariantExcSummary *
sift_cdi_scan_exceptions(F2Scan *s, Relation rel, const SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPart *part, Snapshot snap, const SiftVariantExcSummary *exc, int *nexc)
{
	SiftCdiScan *cd;
	const SiftColDeltaPartMap *map;
	const SiftVariantExcSummary *out;
	MemoryContext old;

	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
	{
		if (s->cd != NULL)
		{
			sift_col_delta_part_map_free(s->cd->map);
			s->cd->map = NULL;
		}
		return exc;
	}
	cd = scan_state(s, schema, snap);
	map = part_map(s, cd, rel, e, part);
	if (map == NULL)
		return exc;
	old = MemoryContextSwitchTo(s->cx);
	out = sift_cdi_exceptions(map, exc, *nexc, cd->pred_leaf, schema->ncols, nexc);
	MemoryContextSwitchTo(old);
	return out;
}

static const SiftColDeltaGranMap *
scan_granule_map(const F2Scan *s, uint32 g)
{
	return s->cd != NULL && s->cd->map != NULL ? sift_col_delta_gran_map(s->cd->map, g) : NULL;
}

static bool
remaps(const SiftColDeltaGranMap *gm)
{
	return gm != NULL && (gm->attnum_bloom & sift_cd_att_bit(SIFT_CD_ATT_TID_REMAP)) != 0;
}

bool
sift_cdi_scan_patch(F2Scan *s, const SiftColSchema *schema, uint32 g, ColView *views, uint64 *tids, int n, MemoryContext cx)
{
	const SiftColDeltaGranMap *gm = scan_granule_map(s, g);

	if (gm == NULL)
		return false;
	sift_cdi_patch_granule(gm, schema->ncols, views, tids, n, cx);
	return remaps(gm);
}

void
sift_cdi_scan_remap_tids(const F2Scan *s, uint32 g, uint64 *tids, int n)
{
	const SiftColDeltaGranMap *gm = scan_granule_map(s, g);

	if (gm != NULL)
		sift_cdi_remap_tids(gm, tids, n);
}

bool
sift_cdi_scan_remaps(const F2Scan *s, uint32 g)
{
	return remaps(scan_granule_map(s, g));
}

/* ---- the UPDATE ---- */

/* Where the row at tid is in a part, and that it still has the address there: a row that moved on has left it, and the heap may have given the address to another row */
static bool
locate_row(Relation index, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	SiftCdRemapMap *rm;
	bool		moved;

	if (!sift_f2_locate_published(index, store, tid, loc))
		return false;
	rm = sift_cdi_remap_load(index, &loc->entry);
	moved = rm != NULL && sift_cd_remap_supersedes(rm, loc->gb->pin->part_gen, loc->gb->granule_id, loc->row, tid);
	sift_cd_remap_free(rm);
	if (moved)
		sift_f2_gbuf_unref(loc->gb);
	return !moved;
}

static void
key_absent_of(const SiftF2Rel *f, const SiftF2LeafDiff *d, bool *absent)
{
	for (int i = 0; i < d->nchanged; i++)
		absent[i] = f->key_leaf != NULL && f->key_leaf[d->leaf[i]] && !sift_key_row_present(d->cell[i].isnull, d->cell[i].tag);
}

static uint32
stage_pages(const SiftTxnStage *st, uint64 part_id)
{
	return st->host_stage == NULL ? 0 : (uint32) ((sift_col_delta_stage_part_bytes(st->host_stage, part_id) + SIFT_COL_DELTA_PAGE_ROOM - 1) / SIFT_COL_DELTA_PAGE_ROOM);
}

static void
stage_record(SiftTxnStage *st, const SiftPartitionEntry *e, const uint8 *rec)
{
	if (st->host_stage == NULL)
	{
		st->host_stage = MemoryContextAllocZero(TopTransactionContext, sizeof(SiftColDeltaStage));
		st->host_event = sift_col_delta_stage_event;
	}
	sift_col_delta_stage_add(st->host_stage, e->part_id, e->key_lo, GetCurrentSubTransactionId(), rec);
	st->host_pending++;
}

bool
sift_cdi_stage_update(const SiftF2DiffEnv *env, SiftTxnStage *st, HeapTuple oldt, HeapTuple newt, uint64 old_tid)
{
	SiftF2Rel  *f = env->f;
	SiftF2Store *store = sift_f2_store_open(env->index);
	SiftF2DiffResult res;
	SiftF2Loc	loc;
	SiftF2AdmitIn in;
	SiftColDeltaTarget target;
	bool		absent[SIFT_F2_DIFF_MAX_LEAVES];
	uint8		rec[SIFT_COL_DELTA_MAX_REC_BYTES];
	uint32		len;

	if (f->spec.preagg || sift_proj_relation_has(env->index))
		return false;
	sift_f2_diff_update(env, oldt, newt, &res);
	if (res.pred != SIFT_PRED_BOTH || res.key_changed || res.leaves.rejected ||
		sift_f2_scope_reads_any(env->index, res.leaves.leaf, res.leaves.nchanged) || !locate_row(env->index, store, old_tid, &loc))
		return false;
	target = (SiftColDeltaTarget) {.part_gen = loc.gb->pin->part_gen,.granule_id = loc.gb->granule_id,.row_ordinal = loc.row,.tid = old_tid};
	key_absent_of(f, &res.leaves, absent);
	len = sift_cdi_encode(rec, sizeof(rec), &target, GetCurrentTransactionId(), GetCurrentCommandId(false), GetCurrentTransactionNestLevel() > 1, false, sift_f2_tid(&newt->t_self), f->kinds,
						  res.leaves.leaf, res.leaves.cell, absent, res.leaves.nchanged);
	in = (SiftF2AdmitIn) {.tracked = true,.pred = res.pred,.nchanged_leaves = res.leaves.nchanged,.total_leaves = f->ncols,.payload_bytes = len,
		.chain_pages = (loc.entry.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 ? sift_manifest_read_col_delta(env->index, &loc.entry).npages : 0,.staged_pages = stage_pages(st, loc.entry.part_id)};
	if (len == 0 || sift_f2_col_delta_admit_index(&in) != SIFT_F2A_OK || !sift_stage_cancel_tid(st, sift_f2_tid(&newt->t_self)))
	{
		sift_f2_gbuf_unref(loc.gb);
		return false;
	}
	stage_record(st, &loc.entry, rec);
	sift_f2_gbuf_unref(loc.gb);
	return true;
}
