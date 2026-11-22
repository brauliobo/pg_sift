/*
 * col/v1/maint/merge_window.c - the units of the inputs a merge holds at a time
 * The rows of one superblock of every input that has any are merged into one batch before the writer takes them. The units are decoded one at a time into batches
 * the window lends and folded into one as they fill the budget of the merge gives them (at least two units): a merge holds the units of a step, the one they are folded
 * into and the empty batches it lends again, whatever the number of its inputs, and gives each unit's arrays back as soon as its rows are in the superblock.
 */
#include "postgres.h"
#include "col/v1/maint/maint_int.h"
#include "core/limits.h"
#include "core/memtrack.h"

struct MergeWindow
{
	MemoryContext cx;
	const SiftColSchema *schema;
	Size		budget;			/* the bytes of the arrays of the units at which they are folded */
	SiftColBatch **use;			/* the units of the superblock in hand, the earlier input first: the one a fold made takes the first place */
	int			nuse;
	SiftColBatch **idle;		/* the batches given back, empty */
	int			nidle;
	Size		peak;			/* the most bytes the arrays of the units held at a fold */
};

MergeWindow *
maint_window_begin(MemoryContext cx, const SiftColSchema *schema, int inputs, Size budget)
{
	MergeWindow *w = MemoryContextAllocZero(cx, sizeof(MergeWindow));

	w->cx = AllocSetContextCreate(cx, "sift_col merge window", ALLOCSET_DEFAULT_SIZES);
	w->schema = schema;
	w->budget = budget;
	w->use = MemoryContextAlloc(w->cx, (inputs + 1) * sizeof(SiftColBatch *));
	w->idle = MemoryContextAlloc(w->cx, (2 * inputs + 2) * sizeof(SiftColBatch *));
	return w;
}

SiftColBatch *
maint_window_take(MergeWindow *w)
{
	return w->nidle > 0 ? w->idle[--w->nidle] : sift_cbatch_create(w->cx, w->schema);
}

static void
window_give(MergeWindow *w, SiftColBatch *b)
{
	sift_cbatch_release(b);
	w->idle[w->nidle++] = b;
}

static Size
window_bytes(const MergeWindow *w)
{
	Size		bytes = 0;

	for (int i = 0; i < w->nuse; i++)
		bytes += sift_cbatch_bytes(w->use[i]);
	return bytes;
}

static void
window_fold(MergeWindow *w, uint32 sb)
{
	SiftColBatch *done = maint_window_take(w);
	uint32		rows = 0;

	for (int i = 0; i < w->nuse; i++)
		rows += w->use[i]->nrows;
	sift_cbatch_reset(done, sb);
	sift_cbatch_reserve(done, rows);
	sift_cbatch_merge(done, w->use, w->nuse);
	w->peak = Max(w->peak, window_bytes(w) + sift_cbatch_bytes(done));
	for (int i = 0; i < w->nuse; i++)
		window_give(w, w->use[i]);
	w->use[0] = done;
	w->nuse = 1;
}

/* A decoded unit joins the superblock in hand, after the units that fill the budget are folded */
void
maint_window_add(MergeWindow *w, SiftColBatch *b, uint32 sb)
{
	if (w->nuse >= 2 && window_bytes(w) + sift_cbatch_bytes(b) > w->budget)
		window_fold(w, sb);
	w->use[w->nuse++] = b;
}

/* The rows of the superblock, merged: the batch of the one input that has any is the superblock as it was decoded */
SiftColBatch *
maint_window_rows(MergeWindow *w, uint32 sb)
{
	if (w->nuse > 1)
		window_fold(w, sb);
	return w->nuse == 1 ? w->use[0] : NULL;
}

/* The rows of the superblock are written: the arrays go back */
void
maint_window_written(MergeWindow *w)
{
	w->peak = Max(w->peak, window_bytes(w));
	for (int i = 0; i < w->nuse; i++)
		window_give(w, w->use[i]);
	w->nuse = 0;
}

void
maint_window_discard(MergeWindow *w, SiftColBatch *b)
{
	window_give(w, b);
}

Size
maint_window_peak(const MergeWindow *w)
{
	return w->peak;
}

MemoryContext
maint_window_context(const MergeWindow *w)
{
	return w->cx;
}
