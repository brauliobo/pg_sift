/*
 * col/f2/build/build.c - CREATE INDEX of format major 2: the heap is read once, the rows are collected in memory, sorted by the key and written as parts of the cold tier, one part for each
 * run that fits maintenance_work_mem and one manifest commit for each page of parts; compaction merges the runs.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "utils/timestamp.h"
#include "miscadmin.h"
#include "utils/snapmgr.h"

#include "col/f2/f2.h"
#include "col/f2/build/par.h"
#include "col/f2/connect.h"
#include "col/f2/compact/compact.h"
#include "col/f2/del/delete.h"
#include "col/f2/scope/scope.h"
#include "col/f2/uniq/unique.h"
#include "col/f2/l0/stage.h"
#include "col/scope/scope.h"
#include "core/f2/gate.h"
#include "core/f2/progress.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/visibility/visibility.h"

/* The rows of a build are cold, immutable data: each run is written as the part a compaction of the level below would write, so the cold codecs (zstd, part dictionaries) apply at once */
#define SIFT_BUILD_LEVEL	SIFT_MAX_PART_LEVEL
#define SIFT_BUILD_TIER		SIFT_TIER_COLD_L2

struct SiftF2Build
{
	Relation	heap;
	Relation	index;
	SiftF2Rel  *f;
	SiftRowBuf *rb;
	SiftCell   *cells;
	SiftCell  **elem;
	MemoryContext cx;
	MemoryContext runcx;		/* what writing one run allocates: the part, its sections and the commit; reset after each run */
	MemoryContext rowcxt;
	TupleTableSlot *slot;
	SiftPendDelRec *ended;		/* the rows of the run the heap has deleted but a snapshot may still see: deleted in the part once it is committed */
	int			nended;
	int			capended;
	SiftPartDesc written[SIFT_PARTLIST_CAP];	/* the parts of the runs not yet in the manifest: one commit takes a page of them */
	int			nwritten;
	uint64		budget;
	uint64		rows;
	SiftF2RowTicker tick;
	SiftF2UniqueBuild *unique;	/* the rows of a unique index by key, to find two of one key (NULL: the index is not unique) */
	SiftF2BuildHand *hand;		/* a participant of a parallel build: where the parts it ends with wait for the commit of the leader (NULL: it commits them) */
	SiftF2BuildGens *gens;		/* the generations the leader reserved for the runs of the participants */
	bool		tiny;			/* a part of the build is small enough for sift_compact_pack_tails */
};
typedef struct SiftF2Build BuildState;

/* STOR[STOR-COL-02]: a run ends at its share of memory or at the whole granule that would overshoot it (a run holds at least one granule; no 4 MiB floor) */
static bool
run_full(const BuildState *bs)
{
	const SiftRowBuf *rb = bs->rb;

	return rb->mem_bytes >= bs->budget || (rb->n % SIFT_GRANULE_ROWS == 0 && (double) rb->mem_bytes * (rb->n + SIFT_GRANULE_ROWS) / rb->n > (double) bs->budget);
}

static void
write_ended(BuildState *bs)
{
	uint8		lo[SIFT_BOUND_BYTES] = {0};

	if (bs->nended > 0)
		sift_del_route(bs->index, 1, lo, bs->ended, bs->nended, &bs->f->spec);
	bs->nended = 0;
}

static void
commit_parts(Relation index, const SiftPartDesc *parts, int n)
{
	uint8		lo[SIFT_BOUND_BYTES] = {0};
	SiftManifestEdit e = {.part_id = 1,.key_lo = lo,.add_parts = parts,.nadd = n};

	if (n > 0)
		sift_manifest_commit(index, &e);
}

static void
commit_written(BuildState *bs)
{
	commit_parts(bs->index, bs->written, bs->nwritten);
	bs->nwritten = 0;
}

static void
hand_over(BuildState *bs)
{
	memcpy(bs->hand->parts, bs->written, bs->nwritten * sizeof(SiftPartDesc));
	bs->hand->n = bs->nwritten;
	bs->hand->tiny = bs->tiny;
	bs->nwritten = 0;
}

static uint64
next_gen(BuildState *bs)
{
	if (bs->gens != NULL)
	{
		uint64		gen = pg_atomic_fetch_add_u64(&bs->gens->next, 1);

		if (gen < bs->gens->end)
			return gen;
	}
	return sift_manifest_new_gen(bs->index, 1);
}

static void
write_run(BuildState *bs)
{
	SiftColIn  *cols;
	SiftRows	rows;
	MemoryContext old;
	Size		freed;

	if (bs->rb->n == 0)
		return;
	sift_f2_progress_phase(SIFT_F2_PHASE_WRITE);
	SIFT_F2_GATE(SIFT_F2_GATE_BUILD_WRITE);
	old = MemoryContextSwitchTo(bs->runcx);
	cols = palloc(bs->f->ncols * sizeof(SiftColIn));
	sift_rowbuf_sort(bs->rb, &bs->f->spec);
	sift_rowbuf_cluster(bs->rb, &bs->f->spec, SIFT_BUILD_TIER);
	rows = sift_rowbuf_rows(bs->rb, cols, &bs->f->spec);
	rows.part_flags = SIFT_PART_FLAG_BUILT;
	/* STOR[STOR-WAL-02]: enable bulk_tail on F2 build (32-page smgr_bulk_write; F6 CIC WAL 26.45 -> 26.43 B/row measured) */
	rows.bulk_tail = true;
	SIFT_INVARIANT(bs->nwritten < SIFT_PARTLIST_CAP, "a build commits its parts before the list of them is full");
	bs->written[bs->nwritten] = sift_part_write_rows(bs->index, 1, next_gen(bs), SIFT_BUILD_LEVEL, SIFT_BUILD_TIER, &rows);
	bs->tiny |= bs->written[bs->nwritten++].nbytes <= SIFT_PACKED_SLOT_MAX_BYTES;
	if (bs->nwritten == SIFT_PARTLIST_CAP || bs->nended > 0)
		commit_written(bs);
	write_ended(bs);
	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->runcx);
	freed = bs->rb->mem_bytes;
	sift_rowbuf_free(bs->rb);
	sift_mem_release_os(freed);
	old = MemoryContextSwitchTo(bs->cx);
	bs->rb = sift_rowbuf_new(&bs->f->spec);
	MemoryContextSwitchTo(old);
}

/*
 * The heap hands the build a row that is not alive (tupleIsAlive false: deleted, but by a transaction a snapshot of the build's may not see, or by this one) so that an older snapshot
 * that uses the index finds it. It is no frozen row: the row keeps the transaction that inserted it, and the transaction that deleted it is written as a delete of the part, which
 * every snapshot that sees that transaction commit takes the row out by.
 */
static void
add_ended(BuildState *bs, ItemPointer tid, TransactionId *xmin, CommandId *cmin)
{
	bool		shouldfree;
	HeapTupleHeader hdr;
	SiftPendDelRec *del;

	if (!table_tuple_fetch_row_version(bs->heap, tid, SnapshotAny, bs->slot))
		elog(ERROR, "sift_col: the heap scan gave a row that the heap does not hold");	/* check-errors: allow */
	hdr = ExecFetchSlotHeapTuple(bs->slot, false, &shouldfree)->t_data;
	*xmin = HeapTupleHeaderGetXmin(hdr);
	*cmin = TransactionIdIsCurrentTransactionId(*xmin) ? HeapTupleHeaderGetCmin(hdr) : 0;
	if (bs->nended == bs->capended)
	{
		bs->capended = Max(256, bs->capended * 2);
		bs->ended = bs->ended != NULL ? repalloc(bs->ended, bs->capended * sizeof(SiftPendDelRec)) : MemoryContextAlloc(bs->cx, bs->capended * sizeof(SiftPendDelRec));
	}
	del = &bs->ended[bs->nended++];
	*del = (SiftPendDelRec) {.tid = sift_f2_tid(tid),.xmax = HeapTupleHeaderGetUpdateXid(hdr)};
	del->cmax = TransactionIdIsCurrentTransactionId(del->xmax) ? HeapTupleHeaderGetCmax(hdr) : 0;
	bs->rb->mem_bytes += sizeof(SiftPendDelRec);
}

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool tupleIsAlive, void *arg)
{
	BuildState *bs = arg;
	uint8		key[SIFT_BOUND_BYTES];
	TransactionId xmin = FrozenTransactionId;
	CommandId	cmin = 0;

	if (bs->unique != NULL && tupleIsAlive)
		sift_f2_unique_build_add(bs->unique, values, isnull);
	sift_f2_row(index, bs->f, bs->rowcxt, tid, values, isnull, bs->cells, bs->elem, key);
	if (!tupleIsAlive)
		add_ended(bs, tid, &xmin, &cmin);
	sift_rowbuf_add(bs->rb, sift_f2_tid(tid), xmin, cmin, 0, key, bs->cells, (const SiftCell *const *) bs->elem);
	MemoryContextReset(bs->rowcxt);
	bs->rows++;
	sift_f2_progress_row(&bs->tick);
	if (run_full(bs))
	{
		write_run(bs);
		sift_f2_progress_phase(SIFT_F2_PHASE_SCAN);
	}
}

uint64
sift_f2_build_budget(int nparticipants, int noperators)
{
	/* MEM[MEM-COL-11]: f2 build run budget, 3/4 of the share each of the noperators (the build, and the sort of a unique index) of each participant has of the maintenance budget */
	return sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, nparticipants, noperators) * 3 / 4;
}

SiftF2Build *
sift_f2_build_begin(Relation heap, Relation index, int nparticipants)
{
	SiftF2Build *bs = palloc0(sizeof(SiftF2Build));

	bs->heap = heap;
	bs->index = index;
	bs->cx = CurrentMemoryContext;
	bs->f = sift_f2_rel(index);
	bs->rowcxt = sift_col_rel_rowcxt(sift_col_rel(index));
	bs->cells = palloc0(bs->f->ncols * sizeof(SiftCell));
	bs->elem = palloc0(bs->f->ncols * sizeof(SiftCell *));
	bs->rb = sift_rowbuf_new(&bs->f->spec);
	bs->slot = table_slot_create(heap, NULL);
	bs->runcx = sift_mem_allocset_create(bs->cx, "sift f2 build run");
	bs->budget = sift_f2_build_budget(nparticipants, index->rd_index->indisunique ? 2 : 1);
	bs->unique = sift_f2_unique_build_begin(heap, index, nparticipants);
	return bs;
}

double
sift_f2_build_scan(SiftF2Build *bs, IndexInfo *info, TableScanDesc scan, bool progress)
{
	double		reltuples = table_index_build_scan(bs->heap, bs->index, info, true, progress, build_cb, bs, scan);

	if (bs->unique != NULL)
		sift_f2_unique_build_end(bs->unique);
	bs->unique = NULL;
	sift_f2_progress_rows_flush(&bs->tick);
	write_run(bs);
	if (bs->hand != NULL)
		hand_over(bs);
	else
		commit_written(bs);
	return reltuples;
}

void
sift_f2_build_share(SiftF2Build *bs, SiftF2BuildHand *hand, SiftF2BuildGens *gens)
{
	bs->hand = hand;
	bs->gens = gens;
}

void
sift_f2_build_commit_hands(SiftF2Build *leader, const SiftF2BuildHand *hands, int n)
{
	SiftPartDesc *all = palloc(n * sizeof(hands->parts));
	int			nall = 0;

	for (int i = 0; i < n; i++)
	{
		memcpy(all + nall, hands[i].parts, hands[i].n * sizeof(SiftPartDesc));
		nall += hands[i].n;
		leader->tiny |= hands[i].tiny;
	}
	commit_parts(leader->index, all, nall);
	pfree(all);
}

bool
sift_f2_build_tiny(const SiftF2Build *bs)
{
	return bs->tiny;
}

uint64
sift_f2_build_rows(const SiftF2Build *bs)
{
	return bs->rows;
}

void
sift_f2_build_end(SiftF2Build *bs)
{
	ExecDropSingleTupleTableSlot(bs->slot);
	sift_rowbuf_free(bs->rb);
	MemoryContextDelete(bs->runcx);
	sift_mem_release_os(bs->budget);
}

/*
 * The options a bare 'auto' stands for, chosen from the rows (written for the rebuilds that follow); AUTO_PENDING when the table had too few rows for the choice or no jsonb document to
 * read paths from. An automatic track_changes is off for a table no statement trigger hears (sift_col_track_deaf), as Format 1 decides it: only a column the user named is an error.
 */
static uint32
resolve_auto(Relation heap, Relation index)
{
	bool		deferred;

	if (!IsBinaryUpgrade && strcmp(sift_col_options_row_key(index), "auto") == 0 && sift_col_track_deaf(heap) != NULL)
		sift_col_set_option(index, "track_changes", "off");
	if (sift_col_paths_redecide(index))
		sift_col_auto_paths(heap, index);
	deferred = sift_f2_auto_key_order(heap, index);
	return deferred || sift_col_rel(index)->paths_pending ? SIFT_META_FLAG_AUTO_PENDING : 0;
}

static uint32
meta_flags(const SiftF2Rel *f, uint32 pending)
{
	return (f->nkey == 0 ? SIFT_META_FLAG_UNKEYED : 0) | pending;
}

IndexBuildResult *
sift_f2_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	Oid			outer = sift_col_building;
	SiftF2Build *volatile bs;
	TimestampTz start = GetCurrentTimestamp();
	SiftF2Choices choices;
	bool		tiny;

	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes require a heap table"),
						errhint("Index a heap table, or copy the rows into one with CREATE TABLE ... AS SELECT.")));
	if (info->ii_Unique)
		info->ii_ParallelWorkers = 0;	/* the rows of one key may be in the shares of two participants */
	sift_col_warn_f2_build_relopts(heap, index);
	sift_stage_discard(RelationGetRelid(index));
	sift_f2_connect_resolve(heap, index);
	sift_scope_resolve(heap, index, false, info->ii_Concurrent ? SIFT_JOURNAL_DEFER : SIFT_JOURNAL_PLAIN);
	choices = sift_f2_choices(index);
	if (!info->ii_Concurrent && sift_f2_scoped(index))
		sift_scope_start(heap, index, true);
	sift_col_building = RelationGetRelid(index);
	PG_TRY();
	{
		uint32		pending;

		if (choices.key_order || choices.paths)
			sift_f2_progress_phase(SIFT_F2_PHASE_SAMPLE);
		pending = resolve_auto(heap, index);
		bs = sift_f2_build_begin(heap, index, info->ii_ParallelWorkers + 1);
		sift_col_meta_init(index, SIFT_AM_COL, sift_col_track_on(index) ? SIFT_COMPAT_TRACK_DELETES : 0, sift_col_rel(index)->schema->hash, meta_flags(bs->f, pending));
		sift_f2_progress_heap(heap);
		sift_f2_progress_phase(SIFT_F2_PHASE_SCAN);
		SIFT_F2_GATE(SIFT_F2_GATE_BUILD_SCAN);
		if (!sift_f2_build_par(heap, index, info, bs, res))
			res->heap_tuples = sift_f2_build_scan(bs, info, NULL, true);
	}
	PG_FINALLY();
	{
		sift_col_building = outer;
	}
	PG_END_TRY();
	res->index_tuples += bs->rows;
	tiny = bs->tiny;
	sift_f2_build_end(bs);
	if (sift_f2_scoped(index) && !info->ii_Concurrent && !IsBinaryUpgrade)
	{
		sift_f2_progress_scopes(heap, index);
		sift_f2_scope_build(heap, index);
		sift_f2_progress_scopes_done(heap, index);
	}
	SIFT_F2_GATE(SIFT_COL_GATE_TAIL);
	if (info->ii_Concurrent && sift_f2_scoped(index))
		sift_scope_resolve(heap, index, true, SIFT_JOURNAL_EMPTY);
	SIFT_TEST_GATE(sift_test_build_gate, 8);
	if (sift_col_track_on(index) && !sift_col_track_awaits_update())
	{
		sift_f2_progress_phase(SIFT_F2_PHASE_TRACK);
		sift_f2_track_install(heap, index, info->ii_Concurrent);
		if (!info->ii_Concurrent)
			sift_f2_track_seal(heap, index);
	}
	sift_f2_connect_build(heap, index);
	sift_scope_start(heap, index, info->ii_Concurrent);
	sift_f2_say_build(heap, index, start, res->index_tuples, choices);
	if (tiny || sift_f2_scoped(index))
		(void) sift_compact_pack_tails(index);
	(void) sift_fsm_tail_shrink(index);
	return res;
}

void
sift_f2_ambuildempty(Relation index)
{
	SiftF2Rel  *f = sift_f2_rel(index);
	uint32		pending = sift_col_rel(index)->paths_pending || sift_col_key_order_auto_pending(index) ? SIFT_META_FLAG_AUTO_PENDING : 0;

	sift_col_meta_init_fork(index, SIFT_AM_COL, sift_col_track_on(index) ? SIFT_COMPAT_TRACK_DELETES : 0, sift_col_rel(index)->schema->hash, meta_flags(f, pending));
}
