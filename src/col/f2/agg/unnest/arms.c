/*
 * col/f2/agg/unnest/arms.c - the arms of a cohort that unnest the same index over one pull (agg/fuse/fuse.h): each arm sets up its run as it would alone, the pull reads the union of the
 * leaves they read, and every batch goes to every arm as a batch of its own columns. An arm whose leaves do not fit the pull beside the others reads its own pull after the shared one.
 */
/* check-memory: allow-allocs every array here follows the arms of one cohort, not the rows of the table */
#include "postgres.h"

#include "agg/fuse/fuse.h"
#include "col/f2/agg/unnest/int.h"
#include "col/f2/agg/unnest/run.h"

/* Where the leaves of an arm are in the shared pull */
typedef struct ArmMap
{
	int			ngroups;
	int			group[SIFT_F2_PULL_MAX_GROUPS];
	int			child[SIFT_F2_PULL_MAX_GROUPS][SIFT_F2_PULL_MAX_CHILD];
	SiftVec		col[SIFT_F2_PULL_MAX_COLS];
	SiftF2ElemStream elem[SIFT_F2_PULL_MAX_GROUPS];
} ArmMap;

static bool
same_col(const SiftF2PullCol *a, const SiftF2PullCol *b)
{
	return a->leaf == b->leaf && a->type == b->type && a->scale == b->scale && a->kind == b->kind;
}

/* the place of c among cols, added when new; -1 when there is no room */
static int
union_col(SiftF2PullCol *cols, int *n, int cap, const SiftF2PullCol *c)
{
	for (int i = 0; i < *n; i++)
		if (same_col(&cols[i], c))
			return i;
	if (*n == cap)
		return -1;
	cols[*n] = *c;
	return (*n)++;
}

/* the group g of the arm in the pull, with its element columns; false when they do not fit */
static bool
union_group(SiftF2PullSpec *ps, const SiftF2PullGroup *g, int *at, int *child)
{
	int			k;

	for (k = 0; k < ps->ngroups && ps->group[k].leaf != g->leaf; k++)
		;
	if (k == ps->ngroups)
	{
		if (k == SIFT_F2_PULL_MAX_GROUPS)
			return false;
		ps->group[ps->ngroups++] = (SiftF2PullGroup) {.leaf = g->leaf};
	}
	*at = k;
	for (int c = 0; c < g->nchild; c++)
		if ((child[c] = union_col(ps->group[k].child, &ps->group[k].nchild, SIFT_F2_PULL_MAX_CHILD, &g->child[c])) < 0)
			return false;
	return true;
}

/* the leaves of the run u added to the shared pull ps; false (ps unchanged) when they do not fit */
static bool
union_arm(SiftF2PullSpec *ps, UnRun *u, ArmMap *m)
{
	SiftF2PullSpec next = *ps;
	int			pcol[SIFT_F2_PULL_MAX_COLS];

	for (int i = 0; i < u->ps.ncols; i++)
		if ((pcol[i] = union_col(next.col, &next.ncols, SIFT_F2_PULL_MAX_COLS, &u->ps.col[i])) < 0)
			return false;
	m->ngroups = u->ps.ngroups;
	for (int g = 0; g < u->ps.ngroups; g++)
		if (!union_group(&next, &u->ps.group[g], &m->group[g], m->child[g]))
			return false;
	if (next.npred != u->ps.npred || (next.npred > 0 && memcmp(next.pred, u->ps.pred, next.npred * sizeof(SiftColPred)) != 0))
		next.npred = 0;
	*ps = next;
	memcpy(u->pcol, pcol, sizeof(pcol));
	return true;
}

/* the batch b of the shared pull as the batch of the arm: its columns and element streams in its own order */
static const SiftF2ExtBatch *
arm_batch(const UnRun *u, ArmMap *m, const SiftF2ExtBatch *b, SiftF2ExtBatch *out)
{
	*out = *b;
	for (int i = 0; i < u->ps.ncols; i++)
		m->col[i] = b->col[u->pcol[i]];
	for (int g = 0; g < m->ngroups; g++)
	{
		const SiftF2ElemStream *src = &b->elem[m->group[g]];

		m->elem[g] = *src;
		for (int c = 0; c < u->ps.group[g].nchild; c++)
		{
			m->elem[g].child_data[c] = src->child_data[m->child[g][c]];
			m->elem[g].child_valid[c] = src->child_valid[m->child[g][c]];
			m->elem[g].child_type[c] = src->child_type[m->child[g][c]];
			m->elem[g].child_dict[c] = src->child_dict[m->child[g][c]];
		}
	}
	out->col = m->col;
	out->elem = m->ngroups > 0 ? m->elem : NULL;
	return out;
}

static void
run_own_pull(UnRun *u)
{
	const SiftF2ExtBatch *b;

	u->pull = sift_f2_pull_begin(u->host, u->snap, &u->ps);
	while ((b = sift_f2_pull_next(u->pull)) != NULL)
		sift_f2_unnest_batch(u, b);
	sift_f2_pull_end(u->pull);
}

void
sift_v6_unnest_run_arms(SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, Relation heap, Relation index, Snapshot snap, bool *alone)
{
	const int	n = cohort->narms;
	UnRun	   *u = palloc(n * sizeof(UnRun));
	ArmMap	   *m = palloc0(n * sizeof(ArmMap));
	SiftF2PullSpec ps = {0};
	SiftF2Pull *pull;
	const SiftF2ExtBatch *b;
	SiftF2ExtBatch view;

	for (int i = 0; i < n; i++)
	{
		sift_f2_unnest_open(&u[i], arms[i].spec, arms[i].group, heap, index, snap, arms[i].stats, arms[i].star);
		if (i == 0)
		{
			ps.npred = u[0].ps.npred;
			ps.pred = u[0].ps.pred;
		}
		alone[i] = !union_arm(&ps, &u[i], &m[i]);
	}
	pull = sift_f2_pull_begin(index, snap, &ps);
	for (int i = 0; i < n; i++)
		u[i].pull = pull;
	while ((b = sift_f2_pull_next(pull)) != NULL)
		for (int i = 0; i < n; i++)
			if (!alone[i])
				sift_f2_unnest_batch(&u[i], arm_batch(&u[i], &m[i], b, &view));
	sift_f2_pull_end(pull);
	for (int i = 0; i < n; i++)
	{
		if (alone[i])
			run_own_pull(&u[i]);
		sift_f2_unnest_close(&u[i]);
	}
}
