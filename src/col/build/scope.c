/*
 * col/build/scope.c - the scope families of the build of a scope index (col/scope.h)
 *
 * The build of the entities scans the heap as for any sift_col index. Each batch of rows it completes is also expanded by the scope keys of the member key of each row (col/scope_fill.c) and
 * handed to a build of its own for each scope: a key-cut build whose units are cut by the derived key and then by time, that writes its segments flagged SIFT_SEG_SCOPE. A fact row
 * is in the segments of the entities once and in those of a scope once for each key of its member; a row with no pair is in none of them.
 *
 * A unit of a scope segment holds a single scope key (col/order.c): a fact whose member is in two keys has two entries with one address, and a unit lists an address once.
 */
#include "postgres.h"

#include "col/build/build_int.h"

#define SCOPE_ROWS_FACTOR	2		/* the rows a scope family is expected to hold for each row of the table, to fit its segments to the directory (a bound: the average is the keys of a member) */

static bool
is_time(const SiftColDef *d)
{
	return d->parent < 0 && d->lane == 0 && !(d->flags & SIFT_COLD_DERIVED) &&
		(d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ);
}

/* The cut of the units of a scope: its derived key, then the first date or timestamp column of the index, else none */
void
colb_scope_cut(SiftColRel *r, int scope)
{
	int			time = -1;

	for (int id = 0; id < r->schema->ncols && time < 0; id++)
		if (is_time(&r->schema->cols[id]))
			time = id;
	r->cut[0] = r->scope_leaf[scope];
	r->ncut = 1;
	if (time >= 0)
		r->cut[r->ncut++] = time;
}

/* The builds of the scopes: segs has room for room segments of each. The fill of the entities (b->fill) becomes the one that also stamps the scopes */
void
colb_scope_begin(ColBuild *b, SiftSegEntry *segs, int room)
{
	b->nscope = b->r->nscope;
	b->metagen = true;
	b->sfill = sift_scope_fill_begin(b->index, b->cxt, b->share);
	b->fill = sift_scope_fill_hook(b->sfill, b->fill, b->r->nconn);
	for (int s = 0; s < b->nscope; s++)
	{
		ColBuild   *sb = colb_alloc(b->index, b->wt, b->budget, room, segs + (Size) s * room, false, 1, s);

		sb->fill = b->fill;
		/* the rows go to runs and merges only: the ranges of a spill are cut from a sample of the heap, which has no scope keys */
		sb->spill_tried = true;
		sb->share = b->total_rows * SCOPE_ROWS_FACTOR;
		b->scope[s] = sb;
	}
}

/* The batch of entities in hand is complete: its rows, replicated by the scope keys of their member, go to the build of each scope */
void
colb_scope_feed(ColBuild *b, const SiftColBatch *src)
{
	for (int s = 0; s < b->nscope; s++)
	{
		ColBuild   *sb = b->scope[s];
		const SiftCVals *fk = &src->col[sift_scope_fill_fk_leaf(b->sfill, s)];
		uint32		n = sift_scope_fill_expand(b->sfill, s, fk->key, fk->nulls, src->nrows, &b->srow, &b->skey, &b->scap);

		/* the entries of a fact are not in the order of its address, nor are those of one batch the rows of one superblock: the run sorts them */
		sb->batch->keyed = sb->batch->unsorted = true;
		for (uint32 i = 0; i < n; i++)
			sift_cbatch_add_scoped(sb->batch, src, b->srow[i], sb->scope_leaf, b->skey[i]);
		colb_flush_batch(sb);
	}
}

/* The segments of the scopes follow those of the entities in b->segs, flagged */
void
colb_scope_gather(ColBuild *b)
{
	for (int s = 0; s < b->nscope; s++)
	{
		const ColBuild *sb = b->scope[s];

		memmove(b->segs + b->nsegs, sb->segs, sb->nsegs * sizeof(SiftSegEntry));
		for (int i = 0; i < sb->nsegs; i++)
			b->segs[b->nsegs + i].flags |= SIFT_SEG_SCOPE;
		b->nsegs += sb->nsegs;
	}
}
