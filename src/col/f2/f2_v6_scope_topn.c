/*
 * col/f2/f2_v6_scope_topn.c - the newest rows of a scope key (ORDER BY the ring leaf DESC [, a tie leaf] LIMIT n over the facts of the key) from the summary: the entries of the ring of the key, the
 * rows the summary lacks (a slab, a part it does not name) that the snapshot sees and the key owns, and for pair edits the members the key lost and gained. The rows come out as the cells of the
 * leaves the tuples of the scan are made of.
 */
/* check-memory: allow-allocs the rows follow the limit of one statement and the rows of one key that a slab or a part the summary lacks holds, at most a read keeps */
#include "postgres.h"

#include "col/f2/f2_v6_scope_int.h"
#include "col/f2/f2_scope.h"
#include "core/cmp.h"

#define TOP_MEMBERS_BATCH 256

typedef struct TopRow
{
	int64		time;
	int64		tie;
	int			seq;
	bool		added;
	SiftScopeNewest ring;
	SiftF2ScopeAdded add;
} TopRow;

typedef struct TopOrder
{
	bool		tie_desc;
} TopOrder;

static int
top_cmp(const void *a, const void *b, void *arg)
{
	const TopRow *x = a,
			   *y = b;

	if (x->time != y->time)
		return x->time > y->time ? -1 : 1;
	if (x->tie != y->tie)
		return (x->tie < y->tie) != ((const TopOrder *) arg)->tie_desc ? -1 : 1;
	return x->seq - y->seq;
}

/* The entries of the ring that the key still owns: the members whose pairs changed may have left it */
static int
owned_rows(SiftF2Scope *sc, int64 key, SiftScopeNewest *rows, int n)
{
	SiftF2ScopeCell *m = palloc(Max(n, 1) * sizeof(SiftF2ScopeCell));
	int			w = 0;

	sift_f2_scope_cells(sc, rows, n, &sc->member_leaf, 1, m);
	for (int i = 0; i < n; i++)
		if (sift_f2_scope_owns_cell(sc, &m[i], key))
			rows[w++] = rows[i];
	pfree(m);
	return w;
}

/* Can a member the key gained since the summary has a row that ranks among the first limit (newer than cut)? The members the pairs say it gained, or may have gained when a node cannot tell */
static bool
gained_rank(SiftF2Scope *sc, const SiftScopeL2Root *root, int64 key, int64 cut)
{
	SiftScopeEdit ed;
	const int64 *gained;
	uint32		n;
	SiftScopeCubeAcc *cubes;
	bool	   *has;
	bool		rank = false;

	sift_scope_overlay_classify(&sc->meta, root, key, &sc->entity, sc->changed, NULL, sc->nchanged, &ed);
	gained = ed.exact ? ed.added : ed.own;
	n = ed.exact ? ed.nadded : ed.nown;
	cubes = palloc(TOP_MEMBERS_BATCH * sizeof(SiftScopeCubeAcc));
	has = palloc0(TOP_MEMBERS_BATCH * sizeof(bool));
	for (uint32 from = 0; from < n && !rank; from += TOP_MEMBERS_BATCH)
	{
		const uint32 nb = Min(TOP_MEMBERS_BATCH, n - from);

		sc->entity.members_cubes(sc->entity.arg, gained + from, nb, SIFT_SPAN_ALL, cubes, has);
		for (uint32 i = 0; i < nb; i++)
			if (has[i])
			{
				rank |= cubes[i].has_time && cubes[i].time_max >= cut;
				sift_scube_free(&cubes[i]);
			}
	}
	pfree(cubes);
	pfree(has);
	sift_scope_edit_free(&ed);
	return rank;
}

/* The ties of the value the ring was cut at, in place of the rows with it: the rows to order by the tie leaf; the rows kept, or -1 when the ring holds fewer than it should */
static SiftScopeNewest *
ring_with_ties(SiftF2Scope *sc, const SiftScopeL2Root *root, int ring, SiftScopeNewest *rows, int n, int *total)
{
	SiftScopeNewest *ties,
			   *flat;
	int			keep = 0;
	const int	nties = (int) sift_scope_ring_ties(&sc->meta, root, ring, rows[n - 1].time, &sc->entity, &ties);

	while (rows[keep].time != rows[n - 1].time)
		keep++;
	*total = keep + nties;
	if (*total < n)
		return NULL;
	flat = palloc(*total * sizeof(SiftScopeNewest));
	memcpy(flat, rows, keep * sizeof(SiftScopeNewest));
	memcpy(flat + keep, ties, nties * sizeof(SiftScopeNewest));
	return flat;
}

/* The cells of the leaves of the first have rows, in order: the rows of the ring and those of the slabs and parts the summary lacks are read from their own places */
static bool
topn_cells(SiftF2Scope *sc, const TopRow *c, int have, const int *leaves, int nleaves, SiftF2ScopeCell **out, int *nout)
{
	SiftScopeNewest *rr = palloc(Max(have, 1) * sizeof(SiftScopeNewest));
	SiftF2ScopeAdded *aa = palloc(Max(have, 1) * sizeof(SiftF2ScopeAdded));
	SiftF2ScopeCell *cr = palloc(Max(have, 1) * nleaves * sizeof(SiftF2ScopeCell)),
			   *ca = palloc(Max(have, 1) * nleaves * sizeof(SiftF2ScopeCell));
	int			ir = 0,
				ia = 0;

	for (int i = 0; i < have; i++)
		if (c[i].added)
			aa[ia++] = c[i].add;
		else
			rr[ir++] = c[i].ring;
	sift_f2_scope_cells(sc, rr, ir, leaves, nleaves, cr);
	sift_f2_scope_added_cells(sc, aa, ia, leaves, nleaves, ca);
	*out = palloc(Max(have, 1) * nleaves * sizeof(SiftF2ScopeCell));
	ir = ia = 0;
	for (int i = 0; i < have; i++)
		memcpy(*out + i * nleaves, c[i].added ? ca + (ia++) * nleaves : cr + (ir++) * nleaves, nleaves * sizeof(SiftF2ScopeCell));
	*nout = have;
	return true;
}

bool
sift_v6_scope_topn(SiftF2Scope *sc, const ScopeSpec *sp, int64 key, bool no_key, int tie_leaf, const int *leaves, int nleaves, SiftF2ScopeCell **out, int *nout)
{
	const int	ring = sift_f2_scope_ring_of(sc, sp->ring_col);
	const SiftScopeL2Root *root = no_key ? NULL : sift_scope_l2_find(&sc->meta, key);
	const bool	edited = sc->nchanged > 0;
	const int	want = edited ? sp->limit * 2 : sp->limit;
	SiftScopeNewest *rows = palloc(want * sizeof(SiftScopeNewest));
	SiftF2ScopeAdded *added = NULL;
	TopRow	   *c;
	TopOrder	order = {sp->tie_desc};
	uint32		from_ring,
				nadded = 0;
	int			n = 0,
				total,
				have,
				nr = 0;
	bool		tie_at_cut = false;
	SiftScopeNewest *flat = rows;
	SiftF2ScopeCell *tr = NULL,
			   *ta = NULL;

	if (!sift_f2_scope_rings(sc))
		SIFT_CORRUPT(sc->rel, "The summary of scope %d is damaged.", sp->ord);
	if (ring < 0 || !sift_scope_ring_serves(&sc->meta, ring, sp->nulls_first, sp->leaf_not_null))
		return false;
	if (ring != 0 && sift_f2_scope_added_rank(sc, key, ring, sp->limit, NULL))
		return false;
	if (root != NULL)
		n = (int) sift_scope_latest_ring(&sc->meta, root, ring, want, false, &sc->entity, rows, &from_ring, sp->tie_col > 0 ? &tie_at_cut : NULL);
	total = n;
	if (sp->tie_col > 0 && n > 0 && tie_at_cut && (flat = ring_with_ties(sc, root, ring, rows, n, &total)) == NULL)
		return false;
	if (edited && total > 0)
		total = owned_rows(sc, key, flat, total);
	if (edited && n == want && total < sp->limit)
		return false;
	if (ring == 0 && (nadded = sift_f2_scope_added_top(sc, key, sp->limit, &added)) == UINT32_MAX)
		return false;
	c = palloc(Max(total + nadded, 1) * sizeof(TopRow));
	if (sp->tie_col > 0)
	{
		tr = palloc(Max(total, 1) * sizeof(SiftF2ScopeCell));
		ta = palloc(Max(nadded, 1) * sizeof(SiftF2ScopeCell));
		sift_f2_scope_cells(sc, flat, total, &tie_leaf, 1, tr);
		sift_f2_scope_added_cells(sc, added, (int) nadded, &tie_leaf, 1, ta);
	}
	for (int i = 0; i < total; i++)
	{
		if (tr != NULL && tr[i].isnull)
			return false;
		c[nr] = (TopRow) {.time = flat[i].time,.tie = tr != NULL ? tr[i].i : 0,.seq = nr,.ring = flat[i]};
		nr++;
	}
	for (uint32 i = 0; i < nadded; i++)
	{
		if (ta != NULL && ta[i].isnull)
			return false;
		c[nr] = (TopRow) {.time = added[i].time,.tie = ta != NULL ? ta[i].i : 0,.seq = nr,.added = true,.add = added[i]};
		nr++;
	}
	qsort_arg(c, nr, sizeof(TopRow), top_cmp, &order);
	have = Min(nr, sp->limit);
	if (root != NULL && have != sp->limit && !sp->leaf_not_null)
		return false;
	if (edited && gained_rank(sc, root, key, have == sp->limit ? c[have - 1].time : PG_INT64_MIN))
		return false;
	return topn_cells(sc, c, have, leaves, nleaves, out, nout);
}
