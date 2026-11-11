/*
 * col/build/scope.c - the scope families of the build of a scope index (col/scope.h)
 *
 * The build of the entities scans the heap as for any sift_col index. Each batch of rows it completes is also expanded by the scope keys of the member key of each row (col/scope_fill.c) and
 * handed to a build of its own for each scope: a key-cut build whose units are cut by the derived key and then by time, that writes its segments flagged SIFT_SEG_SCOPE. A fact row
 * is in the segments of the entities once and in those of a scope once for each key of its member; a row with no pair is in none of them.
 *
 * A unit of a scope segment holds the rows of one scope key, or the whole runs of several small keys that share no fact (col/order.c): a fact whose member is in two keys has two
 * entries with one address, and a unit lists an address once.
 */
#include "postgres.h"

#include "col/build/build_int.h"
#include "col/scope.h"

#define SCOPE_ROWS_FACTOR	2		/* the rows a scope family is expected to hold for each row of the table, to fit its segments to the directory (a bound: the average is the keys of a member) */

/* The cut of the units of a scope */
void
colb_scope_cut(SiftColRel *r, int scope)
{
	r->ncut = sift_scope_cut(r, scope, r->cut);
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
			sift_cbatch_add_scoped(sb->batch, src, b->srow[i], sb->scope_leaf, b->skey[i], sb->r->scope_cols[s]);
		colb_flush_batch(sb);
	}
}

/* 2291000 as 2,291,000 */
static char *
count_text(uint64 n)
{
	char	   *plain = psprintf(UINT64_FORMAT, n),
			   *out = palloc(strlen(plain) * 4 / 3 + 2),
			   *o = out;
	int			len = strlen(plain);

	for (int i = 0; i < len; i++)
	{
		if (i > 0 && (len - i) % 3 == 0)
			*o++ = ',';
		*o++ = plain[i];
	}
	*o = '\0';
	return out;
}

static uint64
segs_sum(const SiftSegEntry *segs, int n, bool bytes)
{
	uint64		sum = 0;

	for (int i = 0; i < n; i++)
		sum += bytes ? segs[i].bytes : segs[i].ndocs;
	return sum;
}

/* The refusal of a scope above the limit, from the finished build (exact) or from the sample before it (estimated, the growth of the index not known) */
void
sift_scope_refuse(const char *what, uint64 entries, uint64 rows, bool estimated, double before, double added)
{
	const char *about = estimated ? "about " : "";
	char	   *growth = before >= 1000000 ? psprintf(" and would grow the index from %.1f MB to %.1f MB (+%.0f%%)", before / 1e6, (before + added) / 1e6, 100.0 * added / before) : "";

	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("scope \"%s\": %s%s entries for %s%s rows (%.2f per row%s) is over sift.scope_max_multiplicity (%g)%s", what, about,
					count_text(entries), about, count_text(rows), (double) entries / rows, estimated ? ", from a sample" : "", sift_scope_max_multiplicity, growth),
					errhint("Above it the join answers the scope reads (the entity path); a count or a sum by key is cheaper from a table of them (docs/INDEX-RECIPES.md). "
							"SET sift.scope_max_multiplicity raises the limit.")));
}

/*
 * What a scope costs, said once per scope when the build is done: its entries for the rows of the table (the multiplicity of its scope key: how many keys the member of a row has, on
 * average) and the growth of the index. A multiplicity above sift.scope_max_multiplicity (3: the break-even of the units, docs/COL-SCOPE.md) is refused, with the growth it would have been. The sizes are said from 1 MB of entities: below, they are a few kB that the
 * compressors change from one library version to the next.
 */
static void
scope_report(const ColBuild *b, int s)
{
	const ColBuild *sb = b->scope[s];
	uint64		rows = segs_sum(b->segs, b->nsegs, false),
				entries = segs_sum(sb->segs, sb->nsegs, false),
				before = segs_sum(b->segs, b->nsegs, true),
				added = segs_sum(sb->segs, sb->nsegs, true);
	double		per_row = rows > 0 ? (double) entries / rows : 0;
	const char *name = b->r->schema->cols[b->r->scope_leaf[s]].path;
	char	   *what = pnstrdup(name, strlen(name) - strlen("#scope"));

	if (per_row > sift_scope_max_multiplicity)
		sift_scope_refuse(what, entries, rows, false, before, added);
	if (before >= 1000000)
		ereport(NOTICE, (errmsg("scope \"%s\": %s entries for %s rows (%.2f per row); the index grows from %.1f MB to %.1f MB (+%.0f%%)", what, count_text(entries), count_text(rows),
								per_row, before / 1e6, (before + added) / 1e6, 100.0 * added / before)));
	else
		ereport(NOTICE, (errmsg("scope \"%s\": %s entries for %s rows (%.2f per row)", what, count_text(entries), count_text(rows), per_row)));
}

/* The segments of the scopes follow those of the entities in b->segs, flagged; the entities say that their rows have their entries */
void
colb_scope_gather(ColBuild *b)
{
	for (int i = 0; b->nscope > 0 && i < b->nsegs; i++)
		b->segs[i].flags |= SIFT_SEG_SCOPED;
	for (int s = 0; s < b->nscope; s++)
		scope_report(b, s);
	for (int s = 0; s < b->nscope; s++)
	{
		const ColBuild *sb = b->scope[s];

		memmove(b->segs + b->nsegs, sb->segs, sb->nsegs * sizeof(SiftSegEntry));
		for (int i = 0; i < sb->nsegs; i++)
		{
			b->segs[b->nsegs + i].flags |= SIFT_SEG_SCOPE;
			b->segs[b->nsegs + i].pad = s;
		}
		b->nsegs += sb->nsegs;
	}
}
