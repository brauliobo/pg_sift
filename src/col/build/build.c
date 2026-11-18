/*
 * col/build/build.c - the build of a sift_col index: the access method's ambuild and ambuildempty
 * the serial build and the phases CREATE INDEX reports
 * the runs, the publication and the NOTICEs are in the files next to it
 */
#include "postgres.h"
#include "access/xact.h"
#include "datatype/timestamp.h"
#include "commands/progress.h"
#include "pgstat.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"
#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/build/build_int.h"
#include "col/conn.h"
#include "col/scope.h"
#include "col/vstore/vstore.h"
#include "col/colspool.h"
#include "core/membudget.h"

#ifdef SIFT_TESTING
char	   *sift_test_vstore_columns = NULL;

int			sift_test_col_workers = 0;

int			sift_test_col_fail_sb = 0;

int			sift_test_col_gap_blocks = -1;

int			sift_test_col_merge_workers = -1;
bool		sift_test_col_parallel_always = true;
int			sift_test_col_merge_par_kb = 12288;

int			sift_test_col_merge_slices = 0;

int			sift_test_col_build_peak_kb = 0;

bool		sift_test_col_build_peak_log = false;

static Size peak_base;
static Size peak_max;
static Size peak_logged;

#define PEAK_TOP	8

typedef struct PeakTop
{
	const char *name;
	Size		self;
} PeakTop;

static void
peak_walk(MemoryContext c, PeakTop *top)
{
	Size		self = MemoryContextMemAllocated(c, false);

	for (int i = 0; i < PEAK_TOP; i++)
		if (self > top[i].self)
		{
			memmove(top + i + 1, top + i, (PEAK_TOP - i - 1) * sizeof(PeakTop));
			top[i] = (PeakTop) {c->name, self};
			break;
		}
	for (MemoryContext k = c->firstchild; k != NULL; k = k->nextchild)
		peak_walk(k, top);
}

static void
peak_say(Size now)
{
	PeakTop		top[PEAK_TOP] = {{0}};
	StringInfoData buf;

	peak_walk(TopMemoryContext, top);
	initStringInfo(&buf);
	for (int i = 0; i < PEAK_TOP && top[i].name != NULL; i++)
		appendStringInfo(&buf, "%s%s=%zu", i ? ", " : "", top[i].name, top[i].self / 1024);
	elog(LOG, "sift_col build peak %zu kB: %s", (now - peak_base) / 1024, buf.data);
	pfree(buf.data);
}

void
sift_colb_peak_begin(void)
{
	static TimestampTz statement;

	if (statement == GetCurrentStatementStartTimestamp())
		return;
	statement = GetCurrentStatementStartTimestamp();
	peak_base = peak_max = peak_logged = MemoryContextMemAllocated(TopMemoryContext, true);
	sift_test_col_build_peak_kb = 0;
	sift_test_col_spill_bytes = sift_test_col_spill_rows = 0;
	sift_test_col_spool_used_rows = sift_test_col_spool_est_bytes = 0;
}

void
sift_colb_peak_note(void)
{
	Size		now = MemoryContextMemAllocated(TopMemoryContext, true);

	SIFT_MEM_CHECKPOINT();
	if (now > peak_max)
	{
		peak_max = now;
		sift_test_col_build_peak_kb = (int) ((peak_max - peak_base) / 1024);
		if (sift_test_col_build_peak_log && now > peak_logged + 1024 * 1024)
		{
			peak_logged = now;
			peak_say(now);
		}
	}
}
#endif

char *
sift_col_ambuildphasename(int64 phase)
{
	switch (phase)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case SIFT_COL_PHASE_SCAN:
			return "scanning table";
		case SIFT_COL_PHASE_WRITE:
			return "writing segments";
		default:
			return NULL;
	}
}

static IndexBuildResult *
serial_build(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	SiftSegEntry *segs = palloc(SIFT_COL_BUILD_SEGMENTS * sizeof(SiftSegEntry));
	/* the segments of a scope index are numbered by the metapage, whether or not the units of its entities are cut by key: its scope segments are */
	bool		key_cut = sift_col_key_cut(sift_col_rel(index)) || sift_col_rel(index)->nscope > 0;
	SiftWriteTarget *wt;
	ColBuild   *b;
	int			nsegs;
	ColScanAcc *acc = palloc0(sizeof(ColScanAcc));

	if (key_cut)
		sift_colb_init_meta(index);
	wt = sift_wt_begin(index, key_cut ? SIFT_WRITE_SHARED : SIFT_WRITE_BULK);
	b = sift_colb_begin(index, wt, sift_colb_budget(1), SIFT_COL_BUILD_SEGMENTS, segs, false, 1, !info->ii_Concurrent);
	SIFT_MEM_SCOPE(col_build, b->cxt);
	SpinLockInit(&acc->mutex);
	PG_TRY();
	{
		sift_colb_scan_watch(acc);
		if (heap->rd_rel->reltuples > 0)
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_SCAN);
#ifdef SIFT_TESTING
		sift_test_phase_wait(SIFT_COL_PHASE_SCAN);
#endif
		res->heap_tuples = sift_col_journal_empty ? 0 : sift_colb_scan(b, heap, info);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_WRITE);
#ifdef SIFT_TESTING
		sift_test_phase_wait(SIFT_COL_PHASE_WRITE);
#endif
		nsegs = sift_colb_end(b);
		SIFT_MEM_CHECKPOINT();
	}
	PG_FINALLY();
	{
		sift_colb_scan_watch(NULL);
	}
	PG_END_TRY();
	sift_colb_scan_report(index, acc);
	res->index_tuples = sift_colb_rows(b);
	if (!key_cut)
		sift_store_init_meta_ex(wt, 0, 0, b->r->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
	sift_colb_publish(index, segs, nsegs, !info->ii_Concurrent && sift_col_track_on(index));
	sift_colb_free(b);
	return res;
}

static IndexBuildResult *
build_index(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = NULL;
	bool		chosen,
				scoped;

	/* the build reads the heap, which holds the rows this transaction has buffered for the index */
	sift_cwbuf_discard(RelationGetRelid(index));
	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes require a heap table"),
						errhint("Index a heap table, or copy the rows into one with CREATE TABLE ... AS SELECT.")));
	/* the bitmap scan compares bytes, which is equality only under a deterministic collation (a query matches the index collation exactly) */
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Oid			opc = index->rd_opcintype[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		SiftColDef	def[SIFT_COL_MAX_LANES];

		if ((opc == TEXTOID || opc == BPCHAROID) && OidIsValid(index->rd_indcollation[i]) && !get_collation_isdeterministic(index->rd_indcollation[i]))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_%s_ops needs a deterministic collation", opc == TEXTOID ? "text" : "bpchar"),
							errhint("Use the default operator class for a column under a nondeterministic collation.")));
		/* a char without a length holds values the leaf does not keep as its equality; a numeric is scanned only as the scaled integer of numeric(p <= 18, s) */
		if (opc == BPCHAROID && att->atttypmod < 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_bpchar_ops needs char(n)"),
							errhint("Use the default operator class for a column of type bpchar.")));
		if (opc == NUMERICOID && (sift_col_att_leaves(att, def, 0) != 1 || def[0].store != SIFT_COLS_INT))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_numeric_ops needs numeric(p, s) with p up to 18"),
							errhint("Use the default operator class for a numeric column of another precision.")));
	}
	sift_conn_auto(heap, index);
	sift_conn_resolve(heap, index);
	sift_scope_resolve(heap, index, false, info->ii_Concurrent ? SIFT_JOURNAL_DEFER : SIFT_JOURNAL_PLAIN);
	sift_scope_precheck(heap, index);
#ifdef SIFT_TESTING
	sift_test_col_workers = 0;
	sift_test_col_merge_slices = 0;
	sift_colb_peak_begin();
#endif
	sift_col_auto_arrlen(index);
	sift_col_auto_paths(heap, index);
	sift_col_auto_cube(heap, index);
	sift_col_auto_rows(heap, index);
	chosen = sift_col_auto_keys(heap, index);
	(void) sift_col_auto_cut(heap, index);
	sift_col_auto_gorder(heap, index, true);
	sift_col_row_key_prepare(heap, index);
	sift_col_track_check(heap, index);
	sift_col_auto_point(heap, index);
	sift_vstore_auto(heap, index);
	/* the first scope index of a relation makes its journal before it reads anything and stamps its segments with it; a concurrent build does not hold the writers out for its whole length: its journal begins at the end, and its validation fills the segments */
	scoped = !info->ii_Concurrent && sift_scope_specs(index) != NULL;
	if (scoped)
	{
		sift_scope_start(heap, index, true);
		PushActiveSnapshot(GetLatestSnapshot());
	}
	if (info->ii_ParallelWorkers > 0 && !sift_col_journal_empty)	/* with an order key every participant writes a key-sorted segment of the superblocks it claimed */
		res = sift_colp_build(heap, index, info);
	if (res == NULL)
		res = serial_build(heap, index, info);
	if (scoped)
		PopActiveSnapshot();
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_COL_GATE_TAIL);
#endif
	/* the journal of a relation of pairs that has none is made here, empty: it holds the writers of that relation out until this transaction ends, and the validation pass fills it */
	if (info->ii_Concurrent && sift_scope_specs(index) != NULL)
		sift_scope_resolve(heap, index, true, SIFT_JOURNAL_EMPTY);
	if (sift_col_track_on(index))
	{
		sift_col_log_create(index);
		sift_col_track_install(heap, index, info->ii_Concurrent);
		sift_conn_build(heap, index);
	}
	else
		sift_col_track_reconcile(index);
	sift_conn_start(heap, index);
	sift_scope_start(heap, index, info->ii_Concurrent);
	if (sift_scope_specs(index) != NULL)
		sift_scope_floor_after_build(index);
	if (chosen)
		sift_col_auto_keys_report(index);
	sift_col_vacuum_notice(heap, index);
	sift_col_choices_say(heap, index, SIFT_CHOICE_AUTO | SIFT_CHOICE_ADVICE | SIFT_CHOICE_CAP, true);
	return res;
}

IndexBuildResult *
sift_col_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	Oid			outer = sift_col_building;
	IndexBuildResult *res;

	sift_col_building = RelationGetRelid(index);
	PG_TRY();
	{
		res = build_index(heap, index, info);
	}
	PG_FINALLY();
	{
		sift_col_building = outer;
	}
	PG_END_TRY();
	return res;
}

void
sift_col_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);

	sift_store_init_meta_ex(wt, 0, 0, sift_col_rel(index)->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
}
