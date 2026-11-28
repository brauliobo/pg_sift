/*
 * col/v1_engine/store_ops_unnest_arms.c - the arms of a cohort that unnest the same Format 1 index over one source (agg/fuse/fuse.h): each arm sets up its run as it would alone, the source
 * gives the union of the columns and of the element columns they read, and every batch goes to every arm as a batch of its own columns. An arm whose columns do not fit beside the others, or
 * that reads the elements of another array, reads its own source after the shared one.
 */
/* check-memory: allow-allocs every array here follows the arms of one cohort, not the rows of the table */
#include "postgres.h"

#include "agg/fuse/fuse.h"
#include "col/v1_engine/store_ops_unnest.h"

/* Where the columns of an arm are in the shared source */
typedef struct ArmMap
{
	int			col[SIFT_V6_MAX_COLS];
	int			child[SIFT_V1_MAX_ELEM_COLS];
	SiftVec		vcol[SIFT_V6_MAX_COLS];
	SiftV1ElemBatch eb;
} ArmMap;

static bool
same_col(const SiftV6InCol *a, const SiftV6InCol *b)
{
	return a->attno == b->attno && a->typid == b->typid && a->type == b->type && a->scale == b->scale && a->prec == b->prec && a->jsonb_cast == b->jsonb_cast && equal(a->path, b->path);
}

static int
union_col(SiftV6Input *in, const SiftV6InCol *c)
{
	for (int i = 0; i < in->ncols; i++)
		if (same_col(&in->col[i], c))
			return i;
	if (in->ncols == SIFT_V6_MAX_COLS)
		return -1;
	in->col[in->ncols] = *c;
	return in->ncols++;
}

static int
union_child(SiftV1ElemSpec *es, const SiftV1ElemSpec *a, int c)
{
	for (int i = 0; i < es->nchild; i++)
		if (es->child_leaf[i] == a->child_leaf[c] && es->child_type[i] == a->child_type[c] && es->child_kind[i] == a->child_kind[c])
			return i;
	if (es->nchild == SIFT_V1_MAX_ELEM_COLS)
		return -1;
	es->child_leaf[es->nchild] = a->child_leaf[c];
	es->child_type[es->nchild] = a->child_type[c];
	es->child_kind[es->nchild] = a->child_kind[c];
	return es->nchild++;
}

/* the columns of the run u added to the shared input and elements; false (both unchanged) when they do not fit or the elements are of another array */
static bool
union_arm(SiftV6Input *in, SiftV1ElemSpec *es, bool *elems, const UnRun *u, ArmMap *m)
{
	SiftV6Input next = *in;
	SiftV1ElemSpec enext = *es;

	for (int i = 0; i < u->in.ncols; i++)
		if ((m->col[i] = union_col(&next, &u->in.col[i])) < 0)
			return false;
	if (u->has_elems)
	{
		if (*elems && (enext.group_leaf != u->espec.group_leaf || enext.objects != u->espec.objects))
			return false;
		if (!*elems)
			enext = (SiftV1ElemSpec) {.objects = u->espec.objects,.group_leaf = u->espec.group_leaf};
		for (int c = 0; c < u->espec.nchild; c++)
			if ((m->child[c] = union_child(&enext, &u->espec, c)) < 0)
				return false;
		*elems = true;
	}
	*in = next;
	*es = enext;
	return true;
}

/* the batch of the shared source as the batch of the arm, its columns and element columns in its own order */
static void
arm_batch(UnRun *u, ArmMap *m, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	SiftV6Batch view = *b;

	for (int i = 0; i < u->in.ncols; i++)
		m->vcol[i] = b->col[m->col[i]];
	view.col = m->vcol;
	if (u->has_elems)
	{
		m->eb = *eb;
		for (int c = 0; c < u->espec.nchild; c++)
		{
			m->eb.child_data[c] = eb->child_data[m->child[c]];
			m->eb.child_valid[c] = eb->child_valid[m->child[c]];
		}
	}
	sift_v1_unnest_batch(u, &view, u->has_elems ? &m->eb : NULL);
}

static void
run_own_source(UnRun *u, Snapshot snap)
{
	SiftV1Scan	scan = {.elem = u->has_elems ? &u->espec : NULL};
	SiftV6Batch b;

	u->src = sift_v1_source_open(CurrentMemoryContext, snap, &u->in, NULL, &scan);
	while (u->src->next(u->src, &b))
		sift_v1_unnest_batch(u, &b, u->has_elems ? sift_v1_source_elems(u->src) : NULL);
	u->src->close(u->src);
}

void
sift_v1_unnest_run_arms(SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, Relation heap, Relation index, Snapshot snap, bool *alone)
{
	const int	n = cohort->narms;
	UnRun	   *u = palloc(n * sizeof(UnRun));
	ArmMap	   *m = palloc0(n * sizeof(ArmMap));
	SiftV6Input in;
	SiftV1ElemSpec es = {0};
	bool		elems = false;
	SiftV1Scan	scan = {0};
	SiftV6Source *src;
	SiftV6Batch b;

	for (int i = 0; i < n; i++)
	{
		sift_v1_unnest_open(&u[i], arms[i].spec, arms[i].group, heap, index, arms[i].stats, arms[i].star);
		if (i == 0)
		{
			in = u[0].in;
			in.ncols = 0;
		}
		alone[i] = !union_arm(&in, &es, &elems, &u[i], &m[i]);
	}
	scan.elem = elems ? &es : NULL;
	src = sift_v1_source_open(CurrentMemoryContext, snap, &in, NULL, &scan);
	for (int i = 0; i < n; i++)
		if (!alone[i])
			u[i].src = src;
	while (src->next(src, &b))
		for (int i = 0; i < n; i++)
			if (!alone[i])
				arm_batch(&u[i], &m[i], &b, elems ? sift_v1_source_elems(src) : NULL);
	src->close(src);
	for (int i = 0; i < n; i++)
	{
		if (alone[i])
			run_own_source(&u[i], snap);
		sift_v1_unnest_close(&u[i]);
	}
}
