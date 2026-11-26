/*
 * table/f2/col_delta/flush.c - the write of the column deltas a transaction staged (col_delta.h, SiftColDeltaStage): the records of each partition are encoded as one run on the parts the
 * manifest names, then appended to the chain of the partition under its lock, and the events of the stage (a subtransaction that ends, a flush, the end of the transaction).
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"

#include "col/f2/meta.h"
#include "core/crash_point.h"
#include "core/f2/gate.h"
#include "core/part_lock.h"
#include "col/f2/vacuum/run.h"
#include "core/u64set.h"
#include "table/f2/col_delta/batch_enc.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/col_delta/io.h"

static void
gens_of(SiftU64Set *gens, const SiftPartDesc *parts, int nparts)
{
	sift_u64set_init(gens, nparts);
	for (int p = 0; p < nparts; p++)
		(void) sift_u64set_add(gens, parts[p].part_gen);
}

/* The record goes to the partition that holds its row now: the books of the stage follow it */
static void
rehome(SiftColDeltaStage *cd, SiftStageColDelta *it, const SiftPartitionEntry *to, uint32 bytes)
{
	sift_col_delta_stage_book(cd, it, -(int64) bytes, -1);
	it->part_id = to->part_id;
	memcpy(it->key_lo, to->key_lo, SIFT_BOUND_BYTES);
	sift_col_delta_stage_book(cd, it, bytes, 1);
}

/* A row whose part a compaction replaced since the record was staged is where its address says now, and in the partition it lies in now when a merge took its partition away; one whose part a TTL drop
 * took is gone, and its record with it. The stage is the one being flushed and still holds the row: the lookup leaves it alone (a lookup that flushes a stage holding its address would flush this one
 * again). Returns the records that left for another partition: the run is not the run of the partition any more */
static int
relocate_stale(Relation rel, SiftF2Store *store, SiftColDeltaStage *cd, const int *ord, int lo, int hi, const SiftPartDesc *parts, int nparts)
{
	SiftU64Set	gens;
	int			moved = 0;

	gens_of(&gens, parts, nparts);
	for (int k = lo; k < hi; k++)
	{
		SiftStageColDelta *it = &cd->items[ord[k]];
		SiftColDeltaEntryHdr h;
		SiftF2Loc	loc;

		CHECK_FOR_INTERRUPTS();
		memcpy(&h, it->rec, sizeof(h));
		if (sift_u64set_has(&gens, h.part_gen))
			continue;
		if (!sift_f2_locate_row_now(rel, store, sift_col_delta_hdr_tid(&h), &loc))
		{
			sift_col_delta_stage_release(cd, ord[k]);
			continue;
		}
		h.part_gen = loc.gb->pin->part_gen;
		h.granule_id = loc.gb->granule_id;
		h.row_ordinal = loc.row;
		memcpy(it->rec, &h, sizeof(h));
		if (loc.entry.part_id != it->part_id)
		{
			rehome(cd, it, &loc.entry, h.rec_bytes);
			moved++;
		}
		sift_f2_gbuf_unref(loc.gb);
	}
	sift_u64set_free(&gens);
	return moved;
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

static SiftColDeltaBatch
run_batch(const SiftColDeltaStage *cd, const int *ord, int lo, int hi, FullTransactionId next)
{
	SiftColDeltaBatch b = {0};
	SiftColDeltaRun run;
	uint8		out[SIFT_COL_DELTA_RUN_OUT_BYTES];

	sift_col_delta_run_init(&run);
	for (int k = lo; k <= hi; k++)
	{
		const uint8 *rec = k < hi ? cd->items[ord[k]].rec : NULL;
		uint32		n;

		if (k < hi && rec == NULL)
			continue;
		n = sift_col_delta_run_feed(&run, rec, out);
		for (uint32 at = 0; at < n; at += ((const SiftColDeltaEntryHdr *) (out + at))->rec_bytes)
			sift_col_delta_batch_add(&b, next, out + at);
	}
	return b;
}

/* The records of a run leave the stage once the chain holds them: a flush that fails leaves them for the one that follows or for the abort of their subtransaction */
static void
release_run(SiftColDeltaStage *cd, const int *ord, int lo, int hi)
{
	for (int k = lo; k < hi; k++)
		if (cd->items[ord[k]].rec != NULL)
			sift_col_delta_stage_release(cd, ord[k]);
}

#define SIFT_COL_DELTA_REGROUPS 8	/* merges of partitions that one flush meets before it gives up: each is a merge another session committed while it ran */

/* The records of ord[lo..hi), all of one partition, into its chain. Returns 0, or the records that a merge of the partitions since they were staged sent to the partition that holds their rows
 * now: nothing is written then, and the stage is grouped again. The lookups of the rows and the encoding of the run are made before the lock of the partition, on the parts the manifest has then;
 * under the lock the manifest generation of the partition must still be that of the parts, or a compaction, a split or a merge rewrote them and the work is made again */
static int
flush_part(Relation rel, SiftF2Store *store, SiftColDeltaStage *cd, const int *ord, int lo, int hi, FullTransactionId next)
{
	uint64		part_id = cd->items[ord[lo]].part_id;
	const uint8 *key_lo = cd->items[ord[lo]].key_lo;
	uint32		npages = 0;
	int			moved = 0;

	for (;;)
	{
		SiftPartitionEntry e,
					locked;
		SiftPartDesc *parts = NULL;
		int			nparts = 0;
		SiftColDeltaBatch b = {0};
		bool		stable;

		if (sift_manifest_find_entry(rel, part_id, key_lo, &e))
			parts = sift_manifest_read_parts(rel, &e, &nparts);
		moved = relocate_stale(rel, store, cd, ord, lo, hi, parts, nparts);
		if (moved != 0 || parts == NULL)
			break;
		b = run_batch(cd, ord, lo, hi, next);
		LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		stable = sift_manifest_find_entry(rel, part_id, key_lo, &locked) && locked.manifest_gen == e.manifest_gen;
		if (stable)
		{
			if (b.nentries > 0)
				npages = append_locked(rel, &locked, &b, next);
			release_run(cd, ord, lo, hi);
			SIFT_CRASH_POINT(f2_col_delta_post_append_pre_commit);
			SIFT_F2_GATE(SIFT_F2_GATE_COL_DELTA);
		}
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		sift_col_delta_batch_free(&b);
		pfree(parts);
		if (stable)
			break;
	}
	if (npages >= SIFT_COL_DELTA_FOLD_PAGES)
		(void) sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, part_id);
	return moved;
}

static void
flush_stage(Relation rel, SiftColDeltaStage *cd)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	FullTransactionId next = ReadNextFullTransactionId();
	int			nlive;
	int		   *ord = sift_col_delta_stage_by_part(cd, &nlive);
	int			regroups = 0;

	for (int lo = 0, hi; lo < nlive; lo = hi)
	{
		uint64		part_id = cd->items[ord[lo]].part_id;

		CHECK_FOR_INTERRUPTS();
		for (hi = lo; hi < nlive && cd->items[ord[hi]].part_id == part_id; hi++)
			;
		if (flush_part(rel, store, cd, ord, lo, hi, next) == 0)
			continue;
		if (++regroups > SIFT_COL_DELTA_REGROUPS)
			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("sift: the partitions of \"%s\" were merged %d times while a transaction flushed its column deltas",
																			  RelationGetRelationName(rel), regroups)));
		pfree(ord);
		ord = sift_col_delta_stage_by_part(cd, &nlive);
		hi = 0;
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


