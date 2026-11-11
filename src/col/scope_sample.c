/*
 * col/scope_sample.c - the multiplicity of a scope before its build (col/scope.h)
 *
 * A scope above sift.scope_max_multiplicity entries a row is refused, and the exact figure is known only when the build has written the scope segments: a table of millions of rows
 * spends its build on them before the refusal. The build of a table of SCOPE_SAMPLE_MIN_BLOCKS blocks or more therefore estimates the figure first, from SCOPE_SAMPLE_RUNS runs of
 * SCOPE_SAMPLE_RUN_BLOCKS consecutive blocks at random places (a fixed seed, so a table gives one answer): the entries each run's rows have from the pairs of their member, as a ratio of the
 * entries to the rows of the runs. A run is the unit of the sample (the rows of a block are alike where the table is stored in the order of a key), so the error is that of a ratio of n
 * runs: the variance of the residuals of the runs about the ratio, over n (n - 1) and the mean rows of a run squared. The scope is refused before its build only when the estimate less
 * SCOPE_SAMPLE_SIGMAS of its standard errors is over the limit; every other scope is built, and the exact figure after the build decides as ever.
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/scope.h"

#define SCOPE_SAMPLE_MIN_BLOCKS		2048	/* 16 MB: below it the build is quick and the exact refusal comes soon enough */
#define SCOPE_SAMPLE_RUNS			32
#define SCOPE_SAMPLE_RUN_BLOCKS		8
#define SCOPE_SAMPLE_SIGMAS			3.5		/* a one-sided 99.9% bound of a ratio of 32 runs (Student, 31 degrees of freedom) */
#define SCOPE_SAMPLE_SEED			42

#ifdef SIFT_TESTING
int			sift_test_scope_sample_blocks = 0;	/* testing: the blocks a table needs to be sampled (0: SCOPE_SAMPLE_MIN_BLOCKS) */
#endif

typedef struct SampleRun
{
	uint32		rows;
	uint32		entries;
} SampleRun;

/* The member keys of the rows of one run of blocks, and their number: *fk and *nulls grow as the run needs */
static uint32
read_run(Relation heap, Snapshot snap, TupleTableSlot *slot, BlockNumber start, AttrNumber attno, Oid type, int64 **fk, uint64 **nulls, uint32 *cap)
{
	TableScanDesc scan = table_beginscan_strat(heap, snap, 0, NULL, true, false);
	uint32		n = 0;

	heap_setscanlimits(scan, start, SCOPE_SAMPLE_RUN_BLOCKS);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, attno, &isnull);

		if (n >= *cap)
		{
			*cap = Max(*cap * 2, 1024);
			*fk = *fk == NULL ? palloc(*cap * sizeof(int64)) : repalloc(*fk, *cap * sizeof(int64));
			*nulls = *nulls == NULL ? palloc((*cap + 63) / 64 * sizeof(uint64)) : repalloc(*nulls, (*cap + 63) / 64 * sizeof(uint64));
		}
		if (n % 64 == 0)
			(*nulls)[n / 64] = 0;
		if (isnull)
			(*nulls)[n / 64] |= UINT64CONST(1) << (n % 64);
		else
			(*fk)[n] = type == INT2OID ? DatumGetInt16(d) : type == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);
		n++;
	}
	table_endscan(scan);
	return n;
}

void
sift_scope_precheck(Relation heap, Relation index)
{
	SiftScopeSpecs *specs;
	BlockNumber nblocks,
				min_blocks = SCOPE_SAMPLE_MIN_BLOCKS;
	MemoryContext cx,
				old;
	struct SiftScopeFill *fill;
	TupleTableSlot *slot;
	Snapshot	snap;
	pg_prng_state prng;
	int64	   *fk = NULL;
	uint64	   *nulls = NULL;
	uint32	   *row = NULL;
	int64	   *key = NULL;
	uint32		fkcap = 0,
				rowcap = 0;

	if (!sift_col_is_index(index) || (specs = sift_scope_specs(index)) == NULL)
		return;
	/* the scope segments are key-cut, as the units cut by a key_cut are: a lowered sift.col_format_minor cannot write them (an older library refuses a scope), and a build that tried
	 * numbered the scope segments over the entities' */
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("sift_col index \"%s\" has a scope, which sift.col_format_minor %d cannot write", RelationGetRelationName(index), sift_col_format_minor),
						errdetail("A scope needs the units cut by key, written at sift.col_format_minor %d or more, and a release before 5.0 cannot read it.", SIFT_COL_FORMAT_PRESENCE),
						errhint("Build it at the normal setting, or give up the scope (ALTER INDEX .. RESET (scope)) to write an index that an older library reads.")));
#ifdef SIFT_TESTING
	if (sift_test_scope_sample_blocks > 0)
		min_blocks = sift_test_scope_sample_blocks;
#endif
	nblocks = RelationGetNumberOfBlocks(heap);
	if (nblocks < min_blocks)
		return;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift scope sample", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	/* the pairs are probed for the sampled members only (a few thousand rows), or read whole when the relation is that small */
	fill = sift_scope_fill_begin(index, cx, (uint64) SCOPE_SAMPLE_RUNS * SCOPE_SAMPLE_RUN_BLOCKS * 200);
	snap = RegisterSnapshot(GetTransactionSnapshot());
	slot = table_slot_create(heap, NULL);
	for (int s = 0; s < specs->n; s++)
	{
		SampleRun	runs[SCOPE_SAMPLE_RUNS];
		AttrNumber	attno = get_attnum(RelationGetRelid(heap), specs->s[s]->fk);
		Oid			type = get_atttype(RelationGetRelid(heap), attno);
		double		rows = 0,
					entries = 0,
					resid = 0,
					ratio,
					se;
		uint64		scaled_rows;
		int			n = 0;

		pg_prng_seed(&prng, SCOPE_SAMPLE_SEED);
		for (int c = 0; c < SCOPE_SAMPLE_RUNS; c++)
		{
			BlockNumber start = (BlockNumber) pg_prng_uint64_range(&prng, 0, nblocks - SCOPE_SAMPLE_RUN_BLOCKS);
			uint32		nrows = read_run(heap, snap, slot, start, attno, type, &fk, &nulls, &fkcap);

			CHECK_FOR_INTERRUPTS();
			if (nrows == 0)
				continue;
			runs[n].rows = nrows;
			runs[n].entries = sift_scope_fill_expand(fill, s, fk, nulls, nrows, &row, &key, &rowcap);
			rows += nrows;
			entries += runs[n].entries;
			n++;
		}
		if (n < SCOPE_SAMPLE_RUNS / 2)
			continue;
		ratio = entries / rows;
		for (int c = 0; c < n; c++)
			resid += pow(runs[c].entries - ratio * runs[c].rows, 2);
		se = sqrt(resid / ((double) n * (n - 1))) / (rows / n);
		if (ratio - SCOPE_SAMPLE_SIGMAS * se > sift_scope_max_multiplicity)
		{
			scaled_rows = (uint64) (rows / ((double) n * SCOPE_SAMPLE_RUN_BLOCKS) * nblocks);
			sift_scope_refuse(specs->s[s]->key_name, (uint64) (ratio * scaled_rows), scaled_rows, true, 0, 0);
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	sift_scope_fill_end(fill);
	UnregisterSnapshot(snap);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}
