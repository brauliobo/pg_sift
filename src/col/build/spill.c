/*
 * col/build/spill.c - the key ranges of a build with key_cut
 * The runs of a serial build overlap in the key (each holds the rows of a stretch of the table, sorted by the key), so a lookup of a key reads a unit in
 * every run until a rewrite cuts them. When the rows do not fit the memory of one run, the rows of the first run give the boundaries of ranges of the key,
 * and the rows go to a temporary file for each range as the table is scanned; each range is then written as a run of its own: runs that do not overlap.
 */
#include "postgres.h"
#include <math.h>
#include <sys/statvfs.h>
#include "commands/tablespace.h"
#include "storage/buffile.h"
#include "storage/fd.h"
#include "col/build/build_int.h"
#include "col/runs.h"
#include "core/cmp.h"

bool		sift_col_build_key_ranges = true;

#ifdef SIFT_TESTING
int			sift_test_col_temp_free_kb = -1;
int			sift_test_col_spill_run_kb = 0;
#endif

#define SPILL_RUN_BYTES	((Size) 768 << 20)	/* the rows of a run of the ranges are read into one batch, whose dictionary of a column of strings holds at most 1 GB */
#define SAMPLE_KEYS		100000
#define SAMPLE_SBS		512			/* superblocks the keys are taken from */
#define CHUNKS_PER_RUN	4			/* a run is the chunks of the key that fit its memory, cut at a chunk end: its size is within a quarter of the budget */
#define MAX_RANGES		512

struct ColSpill
{
	int			n;				/* ranges */
	int			leaf;			/* the first leaf of the key */
	const SiftColDef *def;
	int64	   *bound;			/* [n - 1]: range i holds the keys up to bound[i] (the last the rest and the NULLs) */
	BufFile   **file;
	uint64	   *rows;
};

/* the values of a leaf that are bytes (v.s, len), not a number */
static bool
is_bytes(const SiftColDef *def)
{
	return def->store == SIFT_COLS_STR || def->store == SIFT_COLS_VAR;
}

static void
put(BufFile *f, const void *p, size_t n)
{
	BufFileWrite(f, p, n);
}

static void
put_val(BufFile *f, const SiftColDef *def, const SiftColValue *v)
{
	uint8		head[2] = {(v->isnull ? 1 : 0) | (v->overflow ? 2 : 0) | (v->lo != 0 ? 4 : 0), v->tag};

	put(f, head, 2);
	if (!v->isnull)
	{
		if (is_bytes(def))
		{
			put(f, &v->len, sizeof(v->len));
			put(f, v->v.s, v->len);
		}
		else
			put(f, &v->v, sizeof(v->v));
	}
	if (head[0] & 4)
		put(f, &v->lo, sizeof(v->lo));
}

static void
get(BufFile *f, void *p, size_t n)
{
	BufFileReadExact(f, p, n);
}

static void
get_val(BufFile *f, const SiftColDef *def, SiftColValue *v)
{
	uint8		head[2];

	get(f, head, 2);
	memset(v, 0, sizeof(*v));
	v->isnull = head[0] & 1;
	v->overflow = (head[0] & 2) != 0;
	v->tag = head[1];
	if (!v->isnull)
	{
		if (is_bytes(def))
		{
			char	   *s;

			get(f, &v->len, sizeof(v->len));
			s = palloc(Max(v->len, 1));
			get(f, s, v->len);
			v->v.s = s;
		}
		else
			get(f, &v->v, sizeof(v->v));
	}
	if (head[0] & 4)
		get(f, &v->lo, sizeof(v->lo));
}

static void
put_row(BufFile *f, const SiftColSchema *sc, const SiftColRow *row)
{
	put(f, &row->tid, sizeof(row->tid));
	for (int c = 0; c < sc->ncols; c++)
		if (sc->cols[c].parent < 0)
			put_val(f, &sc->cols[c], &row->val[c]);
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		uint8		has;

		if (def->parent < 0)
			continue;
		has = row->elem[c] != NULL;
		put(f, &has, 1);
		if (has)
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			for (int64 i = 0; i < n; i++)
				put_val(f, def, &row->elem[c][i]);
		}
	}
}

/* Reads the next row of the file into row (arrays of ncols, allocated by the caller), its strings in the current context; false at the end */
static bool
get_row(BufFile *f, const SiftColSchema *sc, SiftColRow *row)
{
	if (!BufFileReadMaybeEOF(f, &row->tid, sizeof(row->tid), true))
		return false;
	row->ncols = sc->ncols;
	memset(row->val, 0, sc->ncols * sizeof(SiftColValue));
	memset(row->elem, 0, sc->ncols * sizeof(SiftColValue *));
	for (int c = 0; c < sc->ncols; c++)
		if (sc->cols[c].parent < 0)
			get_val(f, &sc->cols[c], &row->val[c]);
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		uint8		has;

		if (def->parent < 0)
			continue;
		get(f, &has, 1);
		if (has)
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			row->elem[c] = palloc(Max(n, 1) * sizeof(SiftColValue));
			for (int64 i = 0; i < n; i++)
				get_val(f, def, &row->elem[c][i]);
		}
	}
	return true;
}

/* The key a row sorts by (the chunk's key of col/order.c) */
static int64
key_of(const SiftColDef *def, const SiftColValue *v)
{
	return def->store == SIFT_COLS_INT ? v->v.i : sift_crun_key(def, sift_col_f8_key(v->v.f));
}

static int
range_of(const ColSpill *s, const SiftColValue *v)
{
	int64		key;

	int			lo = 0,
				hi = s->n - 1;

	if (v->isnull || v->overflow)
		return s->n - 1;
	key = key_of(s->def, v);
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

/* The first leaf of the key when it is a plain number (an integer, date, timestamp, numeric or float), else -1 */
static int
range_leaf(const ColBuild *b)
{
	const SiftColSchema *sc = b->r->schema;
	int			leaf;

	if (b->shared || b->r->ncut == 0)
		return -1;
	leaf = b->r->cut[0];
	if (sc->cols[leaf].parent >= 0 || (sc->cols[leaf].store != SIFT_COLS_INT && sc->cols[leaf].store != SIFT_COLS_F4 && sc->cols[leaf].store != SIFT_COLS_F8) ||
		sift_col_def_value_lanes(&sc->cols[leaf]) > 1)
		return -1;
	return leaf;
}

typedef struct Sample
{
	ColBuild   *b;
	int			leaf;
	uint32		stride,
				seen,
				n,
				cap;
	int64	   *keys;
	MemoryContext cx;
} Sample;

static void
sample_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	Sample	   *sm = state;
	SiftColRow	row;

	if (++sm->seen % sm->stride != 0 || sm->n == sm->cap)
		return;
	sift_col_make_row(sm->b->r, tid, values, isnull, &row, sm->cx);
	if (!row.val[sm->leaf].isnull && !row.val[sm->leaf].overflow)
		sm->keys[sm->n++] = key_of(&sm->b->r->schema->cols[sm->leaf], &row.val[sm->leaf]);
	MemoryContextReset(sm->cx);
}

/*
 * The runs of a build by ranges are cut at quantiles of the key of the whole table, not of its first rows (a table is often written in the order of
 * its key, and the first run holds the lowest keys): before the scan, the rows of superblocks spread over the table are read for their keys. Not for a table
 * that fits a run or two.
 */
void
colb_spill_sample(ColBuild *b, Relation heap, IndexInfo *info)
{
	int			leaf = range_leaf(b);
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	uint32		nsb = (nblocks + SIFT_SB_PAGES - 1) / SIFT_SB_PAGES,
				take;
	Sample		sm;

	if (!sift_col_build_key_ranges)
		return;
	if (leaf < 0 || b->total_rows == 0 || (uint64) nblocks * BLCKSZ < 3 * (uint64) b->budget)
		return;
	take = Min(nsb, SAMPLE_SBS);
	sm.b = b;
	sm.leaf = leaf;
	sm.stride = (uint32) Max(((double) b->total_rows / nsb * take) / SAMPLE_KEYS, 1);
	sm.seen = sm.n = 0;
	sm.cap = SAMPLE_KEYS * 2;
	sm.keys = MemoryContextAlloc(b->cxt, sm.cap * sizeof(int64));
	sm.cx = AllocSetContextCreate(b->cxt, "sift_col build sample", ALLOCSET_SMALL_SIZES);
	for (uint32 i = 0; i < take; i++)
	{
		BlockNumber start = (BlockNumber) ((uint64) i * nsb / take + nsb / take / 2) * SIFT_SB_PAGES;

		if (start >= nblocks)
			continue;
		(void) table_index_build_range_scan(heap, b->index, info, false, false, false, start, Min(SIFT_SB_PAGES, nblocks - start), sample_cb, &sm, NULL);
	}
	MemoryContextDelete(sm.cx);
	qsort(sm.keys, sm.n, sizeof(int64), sift_cmp_i64);
	b->ksample = sm.keys;
	b->nksample = sm.n;
}

/* Sends the rows of the batch to the files of their ranges */
void
colb_spill_batch(ColBuild *b, const SiftColBatch *batch)
{
	ColSpill   *s = b->spill;
	SiftColRow	row = {0};

	for (uint32 r = 0; r < batch->nrows; r++)
	{
		int			i;

		sift_cbatch_row(batch, r, &row);
		i = range_of(s, &row.val[s->leaf]);
		put_row(s->file[i], b->r->schema, &row);
		s->rows[i]++;
	}
	sift_cbatch_row_free(&row);
}

/*
 * Whether the temporary files of the rows fit the disk of the temporary tablespaces: need bytes are to be written, and the index (about as large) goes to the
 * disk of the data, which may be the same one, so the files may take up to half of what is free. A disk that cannot be asked, or no temporary tablespace
 * (the default one: base/), is the one of the data directory.
 */
static bool
temp_space_fits(double need)
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
	else
#endif
	if (room < 0)
		return true;
	return need <= room * Max(n, 1) / 2;
}

/* The rows of a run of the ranges: those the first run held, or the share of them that fits SPILL_RUN_BYTES when a large maintenance_work_mem made that run larger */
static uint64
spill_run_rows(const ColBuild *b, Size held_bytes)
{
	Size		cap = SPILL_RUN_BYTES;

#ifdef SIFT_TESTING
	if (sift_test_col_spill_run_kb > 0)
		cap = (Size) sift_test_col_spill_run_kb * 1024;
#endif
	return Max((uint64) ((double) b->held_rows * Min(1.0, (double) cap / held_bytes)), 1);
}

/*
 * The rows held when the first run is full give the ranges: as many as runs the estimate of the rows of the table makes, cut at the quantiles of the key
 * of those rows. False (the rows stay to be written as a run) for a table that fits about two runs, a key that is not a number, a parallel participant
 * or a key of a few values.
 */
bool
colb_spill_start(ColBuild *b)
{
	int			leaf = range_leaf(b);
	int			nr;
	Size		held_bytes;
	uint64		run_rows;
	double		need;
	ColSpill   *s;
	SiftColBatch **in;
	int			nin = 0;
	ListCell   *lc;

	if (!sift_col_build_key_ranges)
		return false;
	if (leaf < 0 || b->ksample == NULL || b->total_rows == 0 || b->held_rows == 0)
		return false;
	held_bytes = MemoryContextMemAllocated(b->heldcxt, true);
	run_rows = spill_run_rows(b, held_bytes);
	nr = (int) ceil((double) CHUNKS_PER_RUN * b->total_rows / (double) run_rows);
	if (nr < 3 || b->nksample < (uint32) nr * 8)
		return false;
	nr = Min(nr, MAX_RANGES);
	/* the rows go to temporary files, about the memory of the first run for each of its rows: neither temp_file_limit nor the disk may refuse them */
	need = (double) held_bytes / b->held_rows * (double) b->total_rows;
	if ((temp_file_limit >= 0 && need > (double) temp_file_limit * 1024.0) || !temp_space_fits(need))
		return false;

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
	in = palloc_array(SiftColBatch *, list_length(b->held));
	foreach(lc, b->held)
		in[nin++] = lfirst(lc);
	b->spill_run_rows = run_rows;
	b->spill_aim = sift_colk_run_rows(b->index, in, nin);
	s->file = MemoryContextAlloc(b->cxt, s->n * sizeof(BufFile *));
	s->rows = MemoryContextAllocZero(b->cxt, s->n * sizeof(uint64));
	for (int i = 0; i < s->n; i++)
		s->file[i] = BufFileCreateTemp(false);
	b->spill = s;
	foreach(lc, b->held)
		colb_spill_batch(b, lfirst(lc));
	list_free(b->held);
	b->held = NIL;
	b->held_rows = 0;
	b->trimmed = 0;
	MemoryContextReset(b->heldcxt);
	pfree(in);
	elog(DEBUG1, "sift_col build: %d chunks of the key, %llu rows to a run, %llu MB of files expected", s->n, (unsigned long long) b->spill_run_rows, (unsigned long long) (need / 1048576.0));
	return true;
}

/*
 * The scan is over. The chunks, in the order of the key, are packed into runs of as many rows as the first run held (about the memory of a run), and the rows
 * of a run are read back into one batch (the rows of any superblock, in any order: a keyed batch), which is sorted and written as a run is. A run ends at the end
 * of a chunk, so the runs do not overlap in the key.
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
		{
			BufFile    *f = s->file[k];

			SIFT_INVARIANT(s->rows[k] == 0 || BufFileSeek(f, 0, 0, SEEK_SET) == 0, "sift_col build: cannot rewind the file of a key range");
			for (uint64 n = 0; n < s->rows[k]; n++)
			{
				MemoryContext old = MemoryContextSwitchTo(rcx);
				bool		more = get_row(f, sc, &row);

				MemoryContextSwitchTo(old);
				SIFT_INVARIANT(more, "sift_col build: the file of a key range ends before its rows");
				sift_cbatch_add(b->batch, &row);
				MemoryContextReset(rcx);
			}
			BufFileClose(f);
		}
		elog(DEBUG1, "sift_col build: chunks %d..%d, %llu rows", i, j - 1, (unsigned long long) rows);
		if (b->batch->nrows > 0)
		{
			sift_cbatch_trim(b->batch);
			b->held = list_make1(b->batch);
			b->held_rows = b->batch->nrows;
			colb_write_run(b, false);
			b->batch = sift_cbatch_create(b->heldcxt, sc);	/* the batch lived in the memory the run released */
			b->batch->keyed = b->batch->unsorted = true;
		}
		i = j;
	}
	MemoryContextDelete(rcx);
}
