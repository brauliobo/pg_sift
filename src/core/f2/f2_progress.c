/*
 * core/f2/f2_progress.c - see f2_progress.h
 */
#include "postgres.h"

#include "access/parallel.h"
#include "commands/progress.h"
#include "datatype/timestamp.h"
#include "utils/timestamp.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/f2_scope.h"
#include "col/granule/granule_int.h"
#include "core/f2/f2_progress.h"
#include "core/f2/manifest.h"
#include "core/memtrack.h"
#include "core/ops.h"
#include "core/progress.h"

#define CENSUS_SAMPLE_PARTS	16

char *
sift_f2_ambuildphasename(int64 phase)
{
	switch (phase)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case SIFT_F2_PHASE_SAMPLE:
			return "sampling schema and key order";
		case SIFT_F2_PHASE_SCAN:
			return "scanning table";
		case SIFT_F2_PHASE_WRITE:
			return "writing parts";
		case SIFT_F2_PHASE_SCOPE:
			return "building scope summaries";
		case SIFT_F2_PHASE_TRACK:
			return "installing change tracking";
	}
	return NULL;
}

/* ---- CREATE INDEX and REINDEX ---- */

SiftF2Choices
sift_f2_choices(Relation index)
{
	return (SiftF2Choices) {.key_order = sift_col_options_key_order_auto(index),.paths = sift_col_paths_redecide(index),.scope = sift_f2_scoped(index)};
}

void
sift_f2_progress_phase(int phase)
{
	if (!IsParallelWorker())
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, phase);
}

void
sift_f2_progress_heap(Relation heap)
{
	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
}

void
sift_f2_progress_rows_flush(SiftF2RowTicker *t)
{
	pgstat_progress_parallel_incr_param(PROGRESS_CREATEIDX_TUPLES_DONE, t->pending);
	t->pending = 0;
}

/* The counters of the partitions are those of the CREATE INDEX of the partitioned table when the heap is a partition of one: they stay its */
static bool
counts_scopes(Relation heap)
{
	return !heap->rd_rel->relispartition;
}

void
sift_f2_progress_scopes(Relation heap, Relation index)
{
	sift_f2_progress_phase(SIFT_F2_PHASE_SCOPE);
	if (counts_scopes(heap))
		pgstat_progress_update_multi_param(2, (const int[]) {PROGRESS_CREATEIDX_PARTITIONS_TOTAL, PROGRESS_CREATEIDX_PARTITIONS_DONE},
										   (const int64[]) {sift_col_rel(index)->nscope, 0});
}

void
sift_f2_progress_scopes_done(Relation heap, Relation index)
{
	if (counts_scopes(heap))
		pgstat_progress_update_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, sift_col_rel(index)->nscope);
}

static char *
qualified_name(Relation rel)
{
	return quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));
}

static uint64
file_bytes(Relation rel)
{
	return (uint64) RelationGetNumberOfBlocks(rel) * BLCKSZ;
}

void
sift_f2_say_build(Relation heap, Relation index, TimestampTz start, uint64 rows, SiftF2Choices choices)
{
	int64		us = GetCurrentTimestamp() - start;
	bool		client = sift_ux_should_emit_build(us, rows, sift_f2_ux_chose(&choices), false);
	bool		server = sift_f2_ux_log_due(us, sift_log_min_duration);
	SiftF2Census census;
	SiftUxOp	op;
	char		extra[SIFT_UX_LINE], line[SIFT_UX_LINE], chosen[SIFT_UX_QUANTITY];

	if (!client && !server)
		return;
	census = sift_f2_census(index, true);
	op = (SiftUxOp) {.am = "sift_col",.name = RelationGetRelationName(index),.extra = sift_f2_ux_build_extra(extra, sizeof(extra), &census),
		.elapsed_us = us,.rows = rows,.bytes = file_bytes(index),.heap_bytes = file_bytes(heap)};
	(void) sift_ux_build_line(line, sizeof(line), &op);
	if (server)
		ereport(LOG_SERVER_ONLY, (errmsg("%s", line)));
	if (client)
	{
		(void) sift_f2_ux_choices(chosen, sizeof(chosen), &choices);
		ereport(NOTICE, (errmsg("%s", line), sift_f2_ux_chose(&choices) ? errdetail("Chosen from the data: %s.", chosen) : 0,
						 errhint("Inspect layout with SELECT * FROM sift.status('%s');", qualified_name(index))));
	}
}

/* ---- the file of an index ---- */

SiftF2CodecClass
sift_f2_codec_class(const SiftChunkRef *ref)
{
	switch (ref->enc & SIFT_ENC_MASK)
	{
		case SIFT_CENC_INT_CONST:
		case SIFT_CENC_ALL_NULL:
			return SIFT_F2_CODEC_CONST;
		case SIFT_CENC_INT_FOR:
		case SIFT_CENC_INT_DELTA:
			return SIFT_F2_CODEC_DELTA;
		case SIFT_CENC_BOOL_BITS:
		case SIFT_CENC_RLE:
			return SIFT_F2_CODEC_PACKED;
		case SIFT_CENC_STR_INLINE:
		case SIFT_CENC_STR_VIEW:
			return SIFT_F2_CODEC_INLINE;
		default:
			return SIFT_F2_CODEC_CODED;
	}
}

static void
tally_codecs(Relation rel, const SiftPart *part, SiftF2Codecs *c)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, part, 0, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);

	for (int i = 0; i < (int) v.hdr->nchunks; i++)
	{
		SiftChunk	ch;
		uint64	   *slot[] = {&c->constant, &c->delta, &c->packed, &c->inlined, &c->coded};

		sift_chunk_open(&v, i, &ch);
		(*slot[sift_f2_codec_class(ch.ref)])++;
	}
}

SiftF2Census
sift_f2_census(Relation rel, bool deep)
{
	SiftF2Census c = {0};
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 census");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np && deep; p++)
		{
			SiftPart   *part = sift_part_open(rel, pd[p].header_blk, pd[p].part_gen, (uint32) mr.entries[i].part_id);

			c.granules += part->hdr.ngranules;
			if (c.parts + p < CENSUS_SAMPLE_PARTS && part->hdr.ngranules > 0)
				tally_codecs(rel, part, &c.codecs);
		}
		c.parts += np;
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return c;
}

/* ---- VACUUM and maintenance of an index ---- */

void
sift_f2_trace_begin(SiftF2Trace *t, Relation index, SiftJob job, bool census)
{
	*t = (SiftF2Trace) {.index = index,.start = GetCurrentTimestamp(),.pages_before = RelationGetNumberOfBlocks(index),.census = census};
	if (census)
		t->before = sift_f2_census(index, true);
	t->began = job != SIFT_JOB_NONE && sift_progress_begin(index, job);
}

void
sift_f2_trace_phase(const SiftF2Trace *t, const char *phase)
{
	sift_progress_phase(t->index, phase);
}

void
sift_f2_trace_step(int64 done, uint64 bytes)
{
	sift_progress_step(done, bytes);
}

void
sift_f2_trace_end(SiftF2Trace *t)
{
	sift_progress_end(t->began);
	t->began = false;
}

static SiftF2IndexFacts
index_facts(const SiftF2Trace *t, uint64 dead_rows, bool shrink_deferred)
{
	BlockNumber pages = RelationGetNumberOfBlocks(t->index);
	BlockNumber high = sift_col_meta_read(t->index).high_water;

	return (SiftF2IndexFacts) {.name = RelationGetRelationName(t->index),.elapsed_us = GetCurrentTimestamp() - t->start,.dead_rows = dead_rows,
		.block_size = BLCKSZ,.pages_before = t->pages_before,.pages_after = pages,.before = t->before,.after = sift_f2_census(t->index, true),
		.shrink_deferred = shrink_deferred,.free_blocks = pages > high ? pages - high : 0};
}

void
sift_f2_say_vacuum(const SiftF2Trace *t, int elevel, double tuples_removed, bool shrink_deferred)
{
	SiftF2IndexFacts v;
	SiftUxOp	op;
	char		line[SIFT_UX_LINE];

	if (elevel != INFO)
		return;
	v = index_facts(t, (uint64) tuples_removed, shrink_deferred);
	sift_f2_ux_vacuum_op(&op, &v);
	ereport(INFO, (errmsg("%s", sift_ux_vacuum_line(line, sizeof(line), &op))));
}

void
sift_f2_say_maintain(const SiftF2Trace *t)
{
	SiftF2IndexFacts v = index_facts(t, 0, false);
	SiftUxOp	op;
	char		line[SIFT_UX_LINE];

	if (!sift_ux_should_emit_timing(v.elapsed_us))
		return;
	sift_f2_ux_maintain_op(&op, &v);
	ereport(NOTICE, (errmsg("%s", sift_ux_maintain_line(line, sizeof(line), &op))));
}

/* ---- VACUUM of a table ---- */

void
sift_f2_vacuum_progress_begin(Relation rel)
{
	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM, RelationGetRelid(rel));
	pgstat_progress_update_multi_param(2, (const int[]) {PROGRESS_VACUUM_PHASE, PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES},
									   (const int64[]) {PROGRESS_VACUUM_PHASE_SCAN_HEAP, (int64) sift_mem_maint_kb() * 1024});
}

static BlockNumber vacuum_total_blocks;

/* The file grows while parts are rewritten: the blocks scanned count against the length the report started from */
void
sift_f2_vacuum_progress_total(Relation rel)
{
	vacuum_total_blocks = RelationGetNumberOfBlocks(rel);
	pgstat_progress_update_param(PROGRESS_VACUUM_TOTAL_HEAP_BLKS, vacuum_total_blocks);
}

void
sift_f2_vacuum_progress_scanned(int done, int nunits)
{
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, (int64) vacuum_total_blocks * done / Max(nunits, 1));
}

void
sift_f2_vacuum_progress_phase(int phase)
{
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE, phase);
}

void
sift_f2_vacuum_progress_dead(uint64 ids, uint64 bytes, uint64 max_bytes, uint32 cycles)
{
	pgstat_progress_update_multi_param(4, (const int[]) {PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS, PROGRESS_VACUUM_DEAD_TUPLE_BYTES, PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES,
														 PROGRESS_VACUUM_NUM_INDEX_VACUUMS},
									   (const int64[]) {(int64) ids, (int64) bytes, (int64) max_bytes, cycles});
}

void
sift_f2_vacuum_progress_indexes(int nindexes, int processed)
{
	pgstat_progress_update_multi_param(2, (const int[]) {PROGRESS_VACUUM_INDEXES_TOTAL, PROGRESS_VACUUM_INDEXES_PROCESSED}, (const int64[]) {nindexes, processed});
}

void
sift_f2_vacuum_progress_end(void)
{
	pgstat_progress_end_command();
}

void
sift_f2_table_trace_begin(SiftF2TableTrace *t, Relation rel)
{
	*t = (SiftF2TableTrace) {.start = GetCurrentTimestamp(),.parts_before = sift_f2_census(rel, false).parts,.bytes_before = file_bytes(rel)};
}

void
sift_f2_say_table_vacuum(const SiftF2TableTrace *t, Relation rel, int elevel, uint64 removed, uint32 batches, double live, double dead)
{
	SiftF2TableFacts f;
	char		line[SIFT_UX_LINE];

	if (elevel != INFO)
		return;
	f = (SiftF2TableFacts) {.name = RelationGetRelationName(rel),.elapsed_us = GetCurrentTimestamp() - t->start,.removed = removed,.batches = batches,
		.live = (uint64) live,.dead_not_removable = (uint64) dead,.parts_before = t->parts_before,.parts_after = sift_f2_census(rel, false).parts,
		.bytes_before = t->bytes_before,.bytes_after = file_bytes(rel)};
	ereport(INFO, (errmsg("%s", sift_f2_ux_table_vacuum_line(line, sizeof(line), &f))));
}
