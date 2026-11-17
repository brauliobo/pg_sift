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
#include "col/seg.h"

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
		ColBuild   *sb = colb_alloc(b->index, b->wt, b->budget, room, segs + (Size) s * room, false, 1, s, b->nscope);

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
		uint32		n = sift_scope_fill_expand(b->sfill, s, sift_scope_fill_members(b->sfill, s, src), fk->nulls, src->nrows, &b->srow, &b->skey, &b->scap);

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
sift_scope_refuse(const char *what, double limit, uint64 entries, uint64 rows, bool estimated, double before, double added)
{
	const char *about = estimated ? "about " : "";
	char	   *growth = before >= 1000000 ? psprintf(" and would grow the index from %.1f MB to %.1f MB (+%.0f%%)", before / 1e6, (before + added) / 1e6, 100.0 * added / before) : "";

	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("scope \"%s\": %s%s entries for %s%s rows (%.2f per row%s) is over the limit of %g entries a row%s", what, about,
					count_text(entries), about, count_text(rows), (double) entries / rows, estimated ? ", from a sample" : "", limit, growth),
					errhint("Above it the join answers the scope reads (the entity path); a count or a sum by key is cheaper from a table of them (docs/INDEX-RECIPES.md). "
							"max_multiplicity(n) in the scope option raises the limit, as SET sift.scope_max_multiplicity does for the next declaration.")));
}

/*
 * What a scope costs, said once per scope when the build is done: its entries for the rows of the table (the multiplicity of its scope key: how many keys the member of a row has, on
 * average) and the growth of the index. A multiplicity above the limit of the scope (max_multiplicity in its declaration: 3, the break-even of the units, docs/COL-SCOPE.md, unless raised) is refused, with the growth it would have been. The sizes are said from 1 MB of entities: below, they are a few kB that the
 * compressors change from one library version to the next.
 */
void
sift_scope_report(Relation index, const SiftColRel *r, int s, uint64 rows, uint64 entries, uint64 before, uint64 added)
{
	double		per_row = rows > 0 ? (double) entries / rows : 0;
	const char *name = r->schema->cols[r->scope_leaf[s]].path;
	char	   *what = pnstrdup(name, strlen(name) - strlen("#scope"));
	double		limit = sift_scope_specs(index)->s[s]->max_multiplicity;

	if (per_row > limit)
		sift_scope_refuse(what, limit, entries, rows, false, before, added);
	if (before >= 1000000)
		ereport(NOTICE, (errmsg("scope \"%s\": %s entries for %s rows (%.2f per row); the index grows from %.1f MB to %.1f MB (+%.0f%%)", what, count_text(entries), count_text(rows),
								per_row, before / 1e6, (before + added) / 1e6, 100.0 * added / before)));
	else
		ereport(NOTICE, (errmsg("scope \"%s\": %s entries for %s rows (%.2f per row)", what, count_text(entries), count_text(rows), per_row)));
}

static void
scope_report(const ColBuild *b, int s)
{
	sift_scope_report(b->index, b->r, s, segs_sum(b->segs, b->nsegs, false), segs_sum(b->scope[s]->segs, b->scope[s]->nsegs, false), segs_sum(b->segs, b->nsegs, true),
					  segs_sum(b->scope[s]->segs, b->scope[s]->nsegs, true));
}

/* The segments of the scopes follow those of the entities in b->segs, flagged */
void
colb_scope_gather(ColBuild *b)
{
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

#define ROLLUP_SAMPLE	64		/* the units of a segment whose rollups are measured */

/* The bytes of the rollups of the rows the immutable segments hold: the groups of a key all, the units a sample */
static double
rollup_bytes(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	double		bytes = 0;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);

			bytes += sift_cseg_rollup_bytes(seg, ROLLUP_SAMPLE);
			sift_cseg_close(seg);
		}
	return bytes;
}

/*
 * What the rollup of the rows costs a scope index (its key groups keep it, docs/COL-SCOPE.md), measured on the finished build: said like the growth of the scope, in MB from 1 MB of
 * index (below, a few kB that the compressors change from one library version to the next).
 */
void
colb_scope_say_rollup(Relation index)
{
	const char *opt = sift_col_options_rollup_columns(index);
	const char *cols = strncmp(opt, "auto:", 5) == 0 ? opt + 5 + strspn(opt + 5, " ") : opt;
	double		bytes,
				total = (double) RelationGetNumberOfBlocks(index) * BLCKSZ;

	if (*opt == '\0' || pg_strcasecmp(opt, "off") == 0 || pg_strcasecmp(opt, "auto") == 0 || (bytes = rollup_bytes(index)) == 0)
		return;
	if (total >= 1000000)
		ereport(NOTICE, (errmsg("sift_col index \"%s\": the rollup of the rows (%s) takes %.1f MB of the index (+%.1f%%)", sift_col_final_name(index), cols, bytes / 1e6,
								100.0 * bytes / (total - bytes))));
	else
		ereport(NOTICE, (errmsg("sift_col index \"%s\": the rollup of the rows (%s) is kept in the units and key groups", sift_col_final_name(index), cols)));
}
