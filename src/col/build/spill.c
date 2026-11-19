/*
 * col/build/spill.c - the key ranges of a build with key_cut
 * The runs of a serial build overlap in the key (each holds the rows of a stretch of the table, sorted by the key), so a lookup of a key reads a unit in
 * every run, and nothing cuts them by key but sift.col_maintain. When the rows do not fit the memory of one run, the rows of the first run give the boundaries of ranges of the key,
 * and the rows go to a temporary file for each range as the table is scanned; each range is then written as a run of its own: runs that do not overlap.
 */
#include "postgres.h"
#include <math.h>
#include <sys/statvfs.h>
#include "commands/tablespace.h"
#include "storage/buffile.h"
#include "storage/fd.h"
#include "col/build/build_int.h"
#include "col/colspool.h"
#include "col/rowfile.h"
#include "col/runs.h"
#include "core/cmp.h"
#include "core/hash.h"
#include "core/membudget.h"

#ifdef SIFT_TESTING
bool		sift_test_col_build_runs_overlap = false;
int			sift_test_col_temp_free_kb = -1;
int			sift_test_col_spill_run_kb = 0;
int			sift_test_col_spill_bytes = 0;
int			sift_test_col_spill_rows = 0;
#endif

#define SPILL_RUN_BYTES	((Size) 768 << 20)	/* the rows of a run of the ranges are read into one batch, whose dictionary of a column of strings holds at most 1 GB */
#define SAMPLE_KEYS		100000
#define SAMPLE_SBS		512			/* superblocks the keys are taken from */
#define CHUNKS_PER_RUN	4			/* a run is the chunks of the key that fit its memory, cut at a chunk end: its size is within a quarter of the budget */
#define MAX_RANGES		512
#define PRICE_SBS		16			/* superblocks the rows are taken from that price the files before a build */
#define PRICE_ROWS		16384
#define PRICE_BYTES		((Size) 8 << 20)	/* what the rows priced may take, whatever their width */
#define BYTES_SAMPLE_ROWS	4096		/* rows of the first run that are written to a file that is only counted, to know what a row takes in the files */

struct ColSpill
{
	int			n;				/* ranges */
	int			leaf;			/* the first leaf of the key */
	const SiftColDef *def;
	int64	   *bound;			/* [n - 1]: range i holds the keys up to bound[i] (the last the rest and the NULLs) */
	MemoryContext cx;			/* the chunks of the ranges: the memory the build gives the spool, which holds the chunks of every range */
	SiftChunkPlan *plan;		/* NULL: the rows of a range are packed in blocks, else they are columnar chunks */
	SiftChunkSpool **chunk;
	SiftRowFile **file;
	uint64	   *rows;
	StringInfoData scratch;		/* the blocks of the files are compressed into it, and read back through it */
};

/* The key a row sorts by (the chunk's key of col/order.c) */
int64
colb_key_of(const SiftColDef *def, const SiftColValue *v)
{
	return def->store == SIFT_COLS_INT ? v->v.i : sift_crun_key(def, sift_col_f8_key(v->v.f));
}

static int
range_of_key(const ColSpill *s, int64 key)
{
	int			lo = 0,
				hi = s->n - 1;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (key <= s->bound[mid])
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

static int
range_of(const ColSpill *s, const SiftColValue *v)
{
	if (v->isnull || v->overflow)
		return s->n - 1;
	return range_of_key(s, colb_key_of(s->def, v));
}

static bool
ranges_wanted(void)
{
#ifdef SIFT_TESTING
	return !sift_test_col_build_runs_overlap;
#else
	return true;
#endif
}

/* The first leaf of the key when it is a plain number (an integer, date, timestamp, numeric or float), else -1 */
int
colb_key_leaf(const SiftColRel *r)
{
	const SiftColSchema *sc = r->schema;
	int			leaf;

	if (r->ncut == 0)
		return -1;
	leaf = r->cut[0];
	if (sc->cols[leaf].parent >= 0 || (sc->cols[leaf].store != SIFT_COLS_INT && sc->cols[leaf].store != SIFT_COLS_F4 && sc->cols[leaf].store != SIFT_COLS_F8) ||
		sift_col_def_value_lanes(&sc->cols[leaf]) > 1)
		return -1;
	return leaf;
}

/* The leaf of the key of a build by key ranges: a table of more than half a run (budget), whose key is a plain number; else -1 */
int
colb_spill_leaf_of(const SiftColRel *r, uint64 nblocks, uint64 total_rows, Size budget)
{
	if (!ranges_wanted() || total_rows == 0 || nblocks * BLCKSZ < (uint64) budget / 2)
		return -1;
	return colb_key_leaf(r);
}

/* The keys the ranges of one build are cut at: those of the entities' key_cut, or the scope keys the members of the sampled rows have (a scope's units are cut by its key) */
typedef struct SampleKeys
{
	ColBuild   *b;
	int			leaf;
	int			scope;			/* -1: the cut of the entities */
	AttrNumber	att;			/* a scope: the column of the index that holds the member key */
	Oid			type;
	int64	   *keys;
	uint32		n,
				cap;
	uint64		rows,			/* the sampled rows that have a member, and the entries they have */
				entries;
} SampleKeys;

typedef struct Sample
{
	ColBuild   *b;
	uint32		stride;
	uint64		seen,
				next;			/* the row the sample takes next */
	SampleKeys	of[1 + SIFT_SCOPE_MAX];
	int			nof;
	uint32	   *row;			/* the expansion of one member (sift_scope_fill_expand) */
	int64	   *key;
	uint32		rowcap;
	MemoryContext cx;
} Sample;

static void
sample_entity(Sample *sm, SampleKeys *k, ItemPointer tid, Datum *values, bool *isnull)
{
	SiftColRow	row;

	sift_col_make_row(sm->b->r, tid, values, isnull, &row, sm->cx);
	if (!row.val[k->leaf].isnull && !row.val[k->leaf].overflow && k->n < k->cap)
		k->keys[k->n++] = colb_key_of(&sm->b->r->schema->cols[k->leaf], &row.val[k->leaf]);
}

static void
sample_scope(Sample *sm, SampleKeys *k, Datum *values, bool *isnull)
{
	MemoryContext old;
	int64		member;
	uint32		n;

	if (isnull[k->att - 1])
		return;
	old = MemoryContextSwitchTo(sm->b->cxt);
	member = sift_scope_fill_member(sm->b->sfill, k->scope, k->type, values[k->att - 1]);
	n = sift_scope_fill_expand(sm->b->sfill, k->scope, &member, NULL, 1, &sm->row, &sm->key, &sm->rowcap);
	MemoryContextSwitchTo(old);
	k->rows++;
	k->entries += n;
	for (uint32 i = 0; i < n && k->n < k->cap; i++)
		k->keys[k->n++] = sm->key[i];
}

static void
sample_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	Sample	   *sm = state;

	if (sm->seen++ != sm->next)
		return;
	sm->next = sift_sample_next(sm->next, sm->stride);
	for (int i = 0; i < sm->nof; i++)
		if (sm->of[i].scope < 0)
			sample_entity(sm, &sm->of[i], tid, values, isnull);
		else
			sample_scope(sm, &sm->of[i], values, isnull);
	MemoryContextReset(sm->cx);
}

/* Does the build cut its rows by ranges of a sample of its key: a table of a run or more, a key that is a plain number */
static bool
wants_ranges(const ColBuild *b, BlockNumber nblocks, uint64 expansion)
{
	return !b->shared && colb_spill_leaf_of(b->r, (uint64) nblocks * expansion, b->total_rows, b->budget) >= 0;
}

static void
sample_add(Sample *sm, ColBuild *b, int scope)
{
	SampleKeys *k = &sm->of[sm->nof++];

	k->b = b;
	k->leaf = colb_key_leaf(b->r);
	k->scope = scope;
	k->cap = SAMPLE_KEYS * 2;
	if (scope >= 0)
	{
		const SiftColRel *r = sm->b->r;

		k->att = r->schema->cols[r->scope_leaf[scope]].attno;
		k->type = TupleDescAttr(RelationGetDescr(sm->b->index), k->att - 1)->atttypid;
		k->cap = SAMPLE_KEYS * SCOPE_ROWS_FACTOR;
	}
	k->keys = MemoryContextAlloc(b->cxt, k->cap * sizeof(int64));
}

static void
sample_finish(SampleKeys *k)
{
	qsort(k->keys, k->n, sizeof(int64), sift_cmp_i64);
	k->b->ksample = k->keys;
	k->b->nksample = k->n;
	if (k->scope >= 0 && k->rows > 0)
		k->b->entries = (uint64) ((double) k->b->total_rows * k->entries / k->rows);
}

/*
 * The runs of a build by ranges are cut at quantiles of the key of the whole table, not of its first rows (a table is often written in the order of
 * its key, and the first run holds the lowest keys): before the scan, the rows of superblocks spread over the table are read for their keys: the key_cut of the entities,
 * and for a scope index the scope keys of the members of those rows, which are the keys the units of each scope are cut by. Not for a table that fits a run.
 */
void
colb_spill_sample(ColBuild *b, Relation heap, IndexInfo *info)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	uint32		nsb = (nblocks + SIFT_SB_PAGES - 1) / SIFT_SB_PAGES,
				take;
	Sample		sm = {.b = b};

	if (sift_col_key_cut(b->r) && wants_ranges(b, nblocks, 1))
		sample_add(&sm, b, -1);
	for (int s = 0; s < b->nscope; s++)
		if (wants_ranges(b->scope[s], nblocks, SCOPE_ROWS_FACTOR))
			sample_add(&sm, b->scope[s], s);
	if (sm.nof == 0)
		return;
	take = Min(nsb, SAMPLE_SBS);
	sm.stride = (uint32) Max(((double) b->total_rows / nsb * take) / SAMPLE_KEYS, 1);
	sm.cx = AllocSetContextCreate(b->cxt, "sift_col build sample", ALLOCSET_SMALL_SIZES);
	for (uint32 i = 0; i < take; i++)
	{
		BlockNumber start = (BlockNumber) ((uint64) i * nsb / take + nsb / take / 2) * SIFT_SB_PAGES;

		if (start >= nblocks)
			continue;
		(void) table_index_build_range_scan(heap, b->index, info, false, false, false, start, Min(SIFT_SB_PAGES, nblocks - start), sample_cb, &sm, NULL);
	}
	MemoryContextDelete(sm.cx);
	for (int i = 0; i < sm.nof; i++)
		sample_finish(&sm.of[i]);
}

static int
batch_range(const ColSpill *s, const SiftColBatch *batch, uint32 r)
{
	const SiftCVals *kv = &batch->col[s->leaf];

	return sift_col_bit_test(kv->nulls, r) || sift_col_bit_test(kv->ovf, r) ? s->n - 1
		: range_of_key(s, s->def->store == SIFT_COLS_INT ? kv->key[r] : sift_crun_key(s->def, kv->key[r]));
}

/* The rows of the batch go to the chunks of their ranges, a leaf at a time: the range of each row from the key leaf, then the rows of a range together */
static void
spill_batch_chunks(ColSpill *s, const SiftColBatch *batch)
{
	uint32		nr = batch->nrows;
	uint16	   *rng = palloc(nr * sizeof(uint16));
	uint32	   *start = palloc0((s->n + 1) * sizeof(uint32));
	uint32	   *fill = palloc(s->n * sizeof(uint32));
	SiftColRowRef *ref = palloc(nr * sizeof(SiftColRowRef));

	for (uint32 r = 0; r < nr; r++)
	{
		int			i = batch_range(s, batch, r);

		rng[r] = (uint16) i;
		start[i + 1]++;
	}
	for (int i = 0; i < s->n; i++)
		start[i + 1] += start[i];
	memcpy(fill, start, s->n * sizeof(uint32));
	for (uint32 r = 0; r < nr; r++)
		ref[fill[rng[r]]++] = (SiftColRowRef) {batch, r};
	for (int i = 0; i < s->n; i++)
		if (start[i + 1] > start[i])
		{
			if (s->chunk[i] == NULL)
				s->chunk[i] = sift_chunkspool_create(s->cx, s->plan);
			sift_chunkspool_add_rows(s->chunk[i], ref + start[i], start[i + 1] - start[i]);
			s->rows[i] += start[i + 1] - start[i];
		}
	pfree(rng);
	pfree(start);
	pfree(fill);
	pfree(ref);
}

/* Sends the rows of the batch to the files of their ranges */
void
colb_spill_batch(ColBuild *b, const SiftColBatch *batch)
{
	ColSpill   *s = b->spill;
	SiftColRow	row = {0};

	SIFT_MEM_SCOPE(col_build_spool, s->cx);
	if (s->plan != NULL)
	{
		spill_batch_chunks(s, batch);
		SIFT_MEM_CHECKPOINT();
		return;
	}
	for (uint32 r = 0; r < batch->nrows; r++)
	{
		int			i;

		sift_cbatch_row(batch, r, &row);
		i = range_of(s, &row.val[s->leaf]);
		sift_rowfile_put(s->file[i], &row, 0, 0);
		s->rows[i]++;
	}
	sift_cbatch_row_free(&row);
}

/*
 * The bytes the temporary files of the rows may take: the index (about as large) goes to the disk of the data, which may be the same one, so the files may take
 * up to half of what is free of each temporary tablespace. A disk that cannot be asked, or no temporary tablespace (the default one: base/), is the one of
 * the data directory. -1: no limit is known.
 */
static double
temp_room(void)
{
	Oid			ts[8];
	int			n;
	double		room = -1;

	PrepareTempTablespaces();
	n = GetTempTablespaces(ts, lengthof(ts));
	for (int i = 0; i < Max(n, 1); i++)
	{
		char		path[MAXPGPATH];
		struct statvfs sv;
		char	   *slash;

		TempTablespacePath(path, n > 0 ? ts[i] : InvalidOid);
		/* the directory is made with the first file: its parent is there */
		if (statvfs(path, &sv) != 0 && ((slash = strrchr(path, '/')) == NULL || (*slash = '\0', statvfs(path, &sv) != 0)))
			continue;
		room = room < 0 ? (double) sv.f_bavail * sv.f_frsize : Min(room, (double) sv.f_bavail * sv.f_frsize);
	}
#ifdef SIFT_TESTING
	if (sift_test_col_temp_free_kb >= 0)
		room = sift_test_col_temp_free_kb * 1024.0;
#endif
	return room < 0 ? -1 : room * Max(n, 1) / 2;
}

/* Whether temporary files of need bytes are allowed (temp_file_limit, the room of the temporary tablespaces), and the most they may take (-1: no limit is known) */
static bool
spill_fits(double need, double *limit, double *room)
{
	*room = temp_room();
	*limit = temp_file_limit >= 0 ? (double) temp_file_limit * 1024.0 : -1;
	return (*limit < 0 || need <= *limit) && (*room < 0 || need <= *room);
}

bool
colb_spill_fits(double need, double *allowed)
{
	double		limit,
				room;
	bool		fits = spill_fits(need, &limit, &room);

	*allowed = limit < 0 ? room : room < 0 ? limit : Min(limit, room);
	return fits;
}

/* A build by key ranges whose rows the temporary files cannot take is refused before they are written: runs that overlap in the key are not what the index is for */
static void
spill_refuse_unless_fits(const ColBuild *b, double need)
{
	double		limit,
				room;

	if (spill_fits(need, &limit, &room))
		return;
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("sift_col index \"%s\": the rows of the build need about %.0f MB of temporary files", sift_col_final_name(b->index), need / 1048576.0),
			 errdetail("temp_file_limit allows %s, and the temporary tablespaces have room for %s (half of what they have free).",
					   limit < 0 ? "any size" : psprintf("%.0f MB", limit / 1048576.0), room < 0 ? "any size" : psprintf("%.0f MB", room / 1048576.0)),
			 errhint("Raise temp_file_limit, set temp_tablespaces to a disk with more room, or free space on it.")));
}

static Size
spill_run_cap(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_spill_run_kb > 0)
		return (Size) sift_test_col_spill_run_kb * 1024;
#endif
	return SPILL_RUN_BYTES;
}

/* The rows of a run of the ranges: those the first run held, or the share of them that fits SPILL_RUN_BYTES when a large maintenance_work_mem made that run larger */
static uint64
spill_run_rows(const ColBuild *b, Size held_bytes)
{
	return Max((uint64) ((double) b->held_rows * Min(1.0, (double) spill_run_cap() / held_bytes)), 1);
}

/* The bytes a row takes in the files, as the rows of the first run say: a sample of them goes through a file that is only counted */
static double
spill_row_bytes(const SiftColSchema *sc, const List *held, uint64 held_rows, Size block)
{
	StringInfoData scratch;
	SiftRowFile *f;
	SiftColRow	row = {0};
	uint64		stride = Max(held_rows / BYTES_SAMPLE_ROWS, 1),
				seen = 0,
				taken = 0;
	double		per_row;
	ListCell   *lc;

	initStringInfo(&scratch);
	f = sift_rowfile_create(sc, &scratch, false, block, false);
	foreach(lc, held)
	{
		const SiftColBatch *batch = lfirst(lc);

		for (uint32 r = 0; r < batch->nrows; r++)
			if (seen++ % stride == 0)
			{
				sift_cbatch_row(batch, r, &row);
				sift_rowfile_put(f, &row, 0, 0);
				taken++;
			}
	}
	sift_cbatch_row_free(&row);
	sift_rowfile_seal(f);
	per_row = (double) sift_rowfile_bytes(f) / taken;
	sift_rowfile_close(f);
	pfree(scratch.data);
	return per_row;
}

/* The plan of the chunks, from the rows of the first run that fall in the range that has most of them: NULL when the budget does not hold the chunks of the ranges, and for the build of a scope, whose entries repeat the address of a fact where the rows of a chunk have ascending ones */
static SiftChunkPlan *
spill_plan(const ColBuild *b, const ColSpill *s)
{
	SiftColRowRef *ref = palloc(SIFT_CHUNKPLAN_ROWS * sizeof(SiftColRowRef));
	uint64	   *count = palloc0(s->n * sizeof(uint64));
	uint32		n = 0;
	int			most = 0;
	ListCell   *lc;
	SiftChunkPlan *plan;

	if (b->scope_leaf >= 0)
		return NULL;
	foreach(lc, b->held)
		for (uint32 r = 0; r < ((const SiftColBatch *) lfirst(lc))->nrows; r++)
			count[batch_range(s, lfirst(lc), r)]++;
	for (int i = 1; i < s->n; i++)
		if (count[i] > count[most])
			most = i;
	foreach(lc, b->held)
	{
		const SiftColBatch *batch = lfirst(lc);

		for (uint32 r = 0; r < batch->nrows && n < SIFT_CHUNKPLAN_ROWS; r++)
			if (batch_range(s, batch, r) == most)
				ref[n++] = (SiftColRowRef) {batch, r};
	}
	plan = sift_chunkplan_make(b->r->schema, ref, n, s->n, b->budget);
	pfree(count);
	pfree(ref);
	return plan;
}

typedef struct Price
{
	SiftColRel *r;
	SiftColBatch *batch;
	MemoryContext cx,
				tmpcx;
	ItemPointerData last;		/* the rows of a chunk plan ascend in address: a row of a HOT chain that comes after a later one is left out of the sample */
	uint32		stride,
				seen,
				n;
} Price;

static void
price_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	Price	   *p = state;
	SiftColRow	row;

	if (++p->seen % p->stride != 0 || p->n == PRICE_ROWS || MemoryContextMemAllocated(p->cx, false) > PRICE_BYTES || ItemPointerCompare(tid, &p->last) <= 0)
		return;
	p->last = *tid;
	sift_col_make_row(p->r, tid, values, isnull, &row, p->tmpcx);
	sift_cbatch_add(p->batch, &row);
	p->n++;
	MemoryContextReset(p->tmpcx);
}

/*
 * What a row takes in the files of the key ranges, from rows of superblocks spread over the table, priced as the first run's rows will be (the plan of the chunks, or
 * the packed blocks when the budget does not hold the chunks of the ranges): the build knows the bytes of its files before it has read a run, which a choice of the
 * path of the build and a refusal for want of room need.
 */
double
colb_spill_price(Relation heap, Relation index, IndexInfo *info, uint64 total_rows)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	uint32		nsb = (nblocks + SIFT_SB_PAGES - 1) / SIFT_SB_PAGES,
				take = Min(nsb, PRICE_SBS);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col build price", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	Price		p = {.r = sift_col_rel_private(index, cx), .cx = cx, .tmpcx = AllocSetContextCreate(cx, "sift_col build price row", ALLOCSET_SMALL_SIZES)};
	List	   *held = NIL;
	SiftColRowRef *ref = palloc(PRICE_ROWS * sizeof(SiftColRowRef));
	SiftChunkPlan *plan;
	Size		share = sift_colb_budget(1);
	uint32		n = 0;
	int			nr;
	double		per_row;
	ListCell   *lc;

#ifdef SIFT_TESTING
	if (sift_test_col_segment_kb > 0)
		share = (Size) sift_test_col_segment_kb * 1024;
#endif
	p.stride = (uint32) Max(((double) total_rows / nsb * take) / PRICE_ROWS, 1);
	for (uint32 i = 0; i < take; i++)
	{
		BlockNumber start = (BlockNumber) ((uint64) i * nsb / take + nsb / take / 2) * SIFT_SB_PAGES;

		if (start >= nblocks)
			continue;
		p.batch = sift_cbatch_create(cx, p.r->schema);
		sift_cbatch_reset(p.batch, sift_block_sb(start));
		ItemPointerSet(&p.last, 0, 0);
		(void) table_index_build_range_scan(heap, index, info, false, false, false, start, Min(SIFT_SB_PAGES, nblocks - start), price_cb, &p, NULL);
		held = lappend(held, p.batch);
	}
	foreach(lc, held)
		for (uint32 r = 0; r < ((const SiftColBatch *) lfirst(lc))->nrows; r++)
			ref[n++] = (SiftColRowRef) {lfirst(lc), r};
	nr = (int) Min(Max(ceil((double) CHUNKS_PER_RUN * total_rows / Max((double) Min(share, spill_run_cap()) * n / Max(MemoryContextMemAllocated(cx, true), 1), 1)), 3), MAX_RANGES);
	plan = n > 0 ? sift_chunkplan_make(p.r->schema, ref, n, nr, share) : NULL;
	per_row = n == 0 ? 0 : plan != NULL ? sift_chunkplan_row_bytes(plan) :
		spill_row_bytes(p.r->schema, held, n, Min(Max(share / ((Size) nr * 2), SIFT_ROWFILE_BLOCK_MIN), SIFT_ROWFILE_BLOCK_MAX));
	SIFT_MEM_CHECKPOINT();
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return per_row;
}

/*
 * The rows held when the first run is full give the ranges: as many as runs the estimate of the rows of the table makes, cut at the quantiles of the key
 * of those rows. False (the rows stay to be written as a run, in the order of the table) for a key that is not a number, a parallel participant
 * or a key of two values or fewer. The rows are spooled as columnar chunks when the memory the build gives the spool (its budget) holds a chunk of the plan for each range,
 * else packed in blocks, which take up to the budget between the ranges (at most 32 kB each).
 */
bool
colb_spill_start(ColBuild *b)
{
	int			leaf = b->shared ? -1 : colb_key_leaf(b->r);
	int			nr;
	Size		held_bytes;
	uint64		run_rows;
	double		need;
	Size		block;
	ColSpill   *s;
	SiftColBatch **in;
	int			nin = 0;
	ListCell   *lc;

	if (!ranges_wanted())
		return false;
	if (leaf < 0 || b->ksample == NULL || b->entries == 0 || b->held_rows == 0)
		return false;
	held_bytes = MemoryContextMemAllocated(b->heldcxt, true);
	run_rows = spill_run_rows(b, held_bytes);
	nr = (int) ceil((double) CHUNKS_PER_RUN * b->entries / (double) run_rows);
	if (nr < 3 || b->nksample < (uint32) nr * 8)
		return false;
	nr = Min(nr, MAX_RANGES);

	s = MemoryContextAllocZero(b->cxt, sizeof(ColSpill));
	s->leaf = leaf;
	s->def = &b->r->schema->cols[leaf];
	s->bound = MemoryContextAlloc(b->cxt, nr * sizeof(int64));
	for (int i = 0; i < nr - 1; i++)
	{
		int64		q = b->ksample[(uint64) (i + 1) * b->nksample / nr - 1];

		if (s->n == 0 || q > s->bound[s->n - 1])
			s->bound[s->n++] = q;
	}
	s->n++;						/* the last range holds the rest */
	if (s->n < 3)
		return false;
	/* the rows go to temporary files, as many bytes for each as those of one range of the first run take */
	s->plan = spill_plan(b, s);
	block = Min(Max(b->budget / ((Size) s->n * 2), SIFT_ROWFILE_BLOCK_MIN), SIFT_ROWFILE_BLOCK_MAX);
	need = (s->plan != NULL ? sift_chunkplan_row_bytes(s->plan) : spill_row_bytes(b->r->schema, b->held, b->held_rows, block)) * (double) b->entries;
	spill_refuse_unless_fits(b, need);

	in = palloc_array(SiftColBatch *, list_length(b->held));
	foreach(lc, b->held)
		in[nin++] = lfirst(lc);
	b->spill_run_rows = run_rows;
	b->spill_aim = sift_colk_run_rows(b->index, in, nin);
	s->cx = AllocSetContextCreate(b->cxt, "sift_col build spool", ALLOCSET_SMALL_SIZES);
	s->rows = MemoryContextAllocZero(b->cxt, s->n * sizeof(uint64));
	if (s->plan != NULL)
		s->chunk = MemoryContextAllocZero(b->cxt, s->n * sizeof(SiftChunkSpool *));
	else
	{
		MemoryContext old = MemoryContextSwitchTo(b->cxt);

		s->file = palloc(s->n * sizeof(SiftRowFile *));
		initStringInfo(&s->scratch);
		for (int i = 0; i < s->n; i++)
			s->file[i] = sift_rowfile_create(b->r->schema, &s->scratch, true, block, false);
		MemoryContextSwitchTo(old);
	}
	b->spill = s;
	foreach(lc, b->held)
		colb_spill_batch(b, lfirst(lc));
	list_free(b->held);
	b->held = NIL;
	b->held_rows = 0;
	b->trimmed = 0;
	MemoryContextReset(b->heldcxt);
	pfree(in);
#ifdef SIFT_TESTING
	sift_test_col_spool_est_bytes = (int) Min(need, (double) INT_MAX);
	sift_test_col_spool_ranges = s->n;
#endif
	if (s->plan != NULL)
		ereport(NOTICE, (errmsg("sift_col index \"%s\": %d key ranges, about %.1f MB of temporary files, in chunks of %u rows (%s)",
								sift_col_final_name(b->index), s->n, need / 1048576.0, sift_chunkplan_rows(s->plan), sift_chunkplan_layer(s->plan))));
	else
		ereport(NOTICE, (errmsg("sift_col index \"%s\": %d key ranges, about %.1f MB of temporary files, packed in blocks",
								sift_col_final_name(b->index), s->n, need / 1048576.0)));
	return true;
}

/* The rows read back are sorted and written as a run, and the next run starts from a batch that has none of them */
static void
spill_run(ColBuild *b)
{
	if (b->batch->nrows == 0)
		return;
	sift_cbatch_trim(b->batch);
	b->held = list_make1(b->batch);
	b->held_rows = b->batch->nrows;
	colb_write_run(b, false);
	b->batch = sift_cbatch_create(b->heldcxt, b->r->schema);	/* the batch lived in the memory the run released */
	b->batch->keyed = b->batch->unsorted = true;
}

static void
range_seal(ColSpill *s, int k)
{
	if (s->plan != NULL)
	{
		if (s->chunk[k] != NULL)
			sift_chunkspool_seal(s->chunk[k]);
	}
	else
		sift_rowfile_seal(s->file[k]);
}

static uint64
range_bytes(const ColSpill *s, int k)
{
	if (s->plan == NULL)
		return sift_rowfile_bytes(s->file[k]);
	return s->chunk[k] != NULL ? sift_chunkspool_bytes(s->chunk[k]) : 0;
}

/* The rows of a sealed range go to the batch of the run, which is written as a run each time it holds target rows; the range's file is closed */
static void
range_read(ColBuild *b, ColSpill *s, int k, SiftColRow *row, MemoryContext rcx, uint64 target)
{
	uint32		xmin,
				cmin;

	if (s->plan != NULL)
	{
		SiftChunkSpool *c = s->chunk[k];

		while (c != NULL && sift_chunkspool_read_into(c, b->batch))
			if (b->batch->nrows >= target)
				spill_run(b);
		if (c != NULL)
			sift_chunkspool_close(c);
		return;
	}
	for (uint64 n = 0; n < s->rows[k]; n++)
	{
		MemoryContext old = MemoryContextSwitchTo(rcx);
		bool		more = sift_rowfile_get(s->file[k], row, &xmin, &cmin);

		MemoryContextSwitchTo(old);
		SIFT_INVARIANT(more, "sift_col build: the file of a key range ends before its rows");
		sift_cbatch_add(b->batch, row);
		MemoryContextReset(rcx);
		if (b->batch->nrows >= target)
			spill_run(b);
	}
	sift_rowfile_close(s->file[k]);
}

/*
 * The scan is over. The chunks, in the order of the key, are packed into runs of as many rows as the first run held (about the memory of a run), and the rows
 * of a run are read back into one batch (the rows of any superblock, in any order: a keyed batch), which is sorted and written as a run is. A run ends at the end
 * of a chunk, so the runs do not overlap in the key. A chunk of more rows than a run (one value of the key holds them, or a few) is read back in pieces of a run,
 * in the order of the scan, each sorted and written as a run: they overlap, as the runs of a build without ranges do, and the rewrite orders them.
 */
void
colb_spill_finish(ColBuild *b)
{
	ColSpill   *s = b->spill;
	const SiftColSchema *sc = b->r->schema;
	MemoryContext rcx = AllocSetContextCreate(b->cxt, "sift_col build range row", ALLOCSET_DEFAULT_SIZES);
	SiftColRow	row;
	uint64		target = b->spill_run_rows;
	int			i = 0;

	for (int k = 0; k < s->n; k++)
		range_seal(s, k);
#ifdef SIFT_TESTING
	for (int k = 0; k < s->n; k++)
	{
		sift_test_col_spill_bytes += (int) range_bytes(s, k);
		sift_test_col_spill_rows += (int) s->rows[k];
	}
#endif
	b->spill = NULL;			/* the batches are held again */
	b->spill_final = true;
	row.val = MemoryContextAlloc(b->cxt, sc->ncols * sizeof(SiftColValue));
	row.elem = MemoryContextAllocZero(b->cxt, (sc->ncols + 1) * sizeof(SiftColValue *));
	/* the batch of the scan has the memory of the largest superblock it met: the runs start from one that has none */
	MemoryContextReset(b->heldcxt);
	b->held = NIL;
	b->held_rows = 0;
	b->trimmed = 0;
	b->batch = sift_cbatch_create(b->heldcxt, sc);
	b->batch->keyed = b->batch->unsorted = true;
	while (i < s->n)
	{
		uint64		rows = 0;
		int			j = i;

		while (j < s->n && (j == i || rows + s->rows[j] <= target))
			rows += s->rows[j++];
		for (int k = i; k < j; k++)
			range_read(b, s, k, &row, rcx, target);
		elog(DEBUG1, "sift_col build: chunks %d..%d, %llu rows", i, j - 1, (unsigned long long) rows);
		spill_run(b);
		i = j;
	}
	if (s->plan == NULL)
		pfree(s->scratch.data);
	MemoryContextDelete(s->cx);
	MemoryContextDelete(rcx);
}
