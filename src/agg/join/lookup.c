/*
 * agg/join/lookup.c - the rows of a RIGHT or FULL join's dimension that no fact row matched, and the conditions of a join that run on the pairs of a batch (agg/join/lookup.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the tables */
#include "postgres.h"

#include "access/tupdesc.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/memutils.h"

#include "agg/datum.h"
#include "agg/join/lookup.h"
#include "core/compat.h"
#include "core/explain.h"
#include "core/memtrack.h"

void
sift_v6_lookup_mark(const SiftStarDim *d, const uint32 *dim_row, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		if (dim_row[i] != 0)
			d->matched[(dim_row[i] - 1) >> 6] |= UINT64CONST(1) << ((dim_row[i] - 1) & 63);
}

uint32
sift_v6_lookup_unmatched(const SiftStarDim *d, uint32 *next, uint32 *dim_row, uint32 cap)
{
	uint32		n = 0;

	for (; *next < d->nrows && n < cap; (*next)++)
		if (!((d->matched[*next >> 6] >> (*next & 63)) & 1))
			dim_row[n++] = *next + 1;
	return n;
}

struct SiftV6Resid
{
	int			ncols;
	const SiftV6Col *col;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *qual[1 + SIFT_STAR_MAX_DIMS];	/* [0] the conditions of the join, [1 + d] those of the ON clause of dimension d; NULL for none */
	Bitmapset  *used[1 + SIFT_STAR_MAX_DIMS];	/* the columns each reads (0-based) */
	TupleTableSlot *slot;
};

static Bitmapset *
columns_of(List *quals)
{
	Bitmapset  *vars = NULL,
			   *cols = NULL;
	int			a = -1;

	pull_varattnos((Node *) quals, 1, &vars);
	while ((a = bms_next_member(vars, a)) >= 0)
		cols = bms_add_member(cols, a + FirstLowInvalidHeapAttributeNumber - 1);
	return cols;
}

bool
sift_v6_resid_has(const SiftV6Resid *r, int dim)
{
	return r != NULL && r->qual[1 + dim] != NULL;
}

SiftV6Resid *
sift_v6_resid_new(MemoryContext cx, const SiftV6Ext *x)
{
	SiftV6Resid *r;
	MemoryContext old;
	TupleDesc	desc;
	bool		any = x != NULL && x->resid != NIL;

	for (int d = 0; x != NULL && d < x->ndims; d++)
		any |= x->on[d] != NIL;
	if (!any)
		return NULL;
	r = MemoryContextAllocZero(cx, sizeof(SiftV6Resid));
	r->ncols = x->nrcol;
	r->col = x->rcol;
	old = MemoryContextSwitchTo(cx);
	r->estate = CreateExecutorState();
	MemoryContextSwitchTo(r->estate->es_query_cxt);
	if (x->resid != NIL)
	{
		r->qual[0] = ExecPrepareQual(x->resid, r->estate);
		r->used[0] = columns_of(x->resid);
	}
	for (int d = 0; d < x->ndims; d++)
		if (x->on[d] != NIL)
		{
			r->qual[1 + d] = ExecPrepareQual(x->on[d], r->estate);
			r->used[1 + d] = columns_of(x->on[d]);
		}
	r->econtext = CreateExprContext(r->estate);
	desc = CreateTemplateTupleDesc(r->ncols);
	for (int i = 0; i < r->ncols; i++)
		TupleDescInitEntry(desc, i + 1, NULL, r->col[i].typid, -1, 0);
	sift_tupdesc_finalize(desc);
	r->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	r->econtext->ecxt_scantuple = r->slot;
	MemoryContextSwitchTo(old);
	return r;
}

void
sift_v6_resid_free(SiftV6Resid *r)
{
	if (r == NULL)
		return;
	ExecDropSingleTupleTableSlot(r->slot);
	FreeExecutorState(r->estate);
}

void
sift_v6_resid_filter(SiftV6Resid *r, int dim, const SiftVec *const *cols, SiftBatchSel *bs)
{
	ExprState  *qual = r->qual[1 + dim];
	const Bitmapset *used = r->used[1 + dim];

	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + (uint32) __builtin_ctzll(bits);
			MemoryContext old;
			bool		keep;
			int			i = -1;

			ResetExprContext(r->econtext);
			old = MemoryContextSwitchTo(r->econtext->ecxt_per_tuple_memory);
			ExecClearTuple(r->slot);
			memset(r->slot->tts_isnull, true, r->ncols * sizeof(bool));
			while ((i = bms_next_member(used, i)) >= 0)
			{
				r->slot->tts_values[i] = sift_v6_vec_datum(cols[i], (uint16) row, r->col[i].typid, r->col[i].scale, &r->slot->tts_isnull[i]);
			}
			ExecStoreVirtualTuple(r->slot);
			keep = ExecQual(qual, r->econtext);
			MemoryContextSwitchTo(old);
			if (!keep)
				bs->mask[w] &= ~(UINT64CONST(1) << (row & 63));
		}
}

/* ---- the tables of the dimensions of the statement ---- */

struct SiftV6SharedDim
{
	const EState *estate;
	Oid			relid;
	SiftStarShape shape;
	AttrNumber	kcol[SIFT_STAR_MAX_KEYS];
	AttrNumber	pcol[SIFT_STAR_MAX_PAY];
	List	   *tlist;			/* the columns and conditions of the scan with the relation numbered 1: what makes two scans the same */
	List	   *qual;
	bool		ready;
	SiftStarDim dim;
	MemoryContext cx;
	MemoryContextCallback forget;
	SiftV6SharedDim *next;
};

static SiftV6SharedDim *shared_dims;

static void
shared_dim_forget(void *arg)
{
	for (SiftV6SharedDim **at = &shared_dims; *at != NULL; at = &(*at)->next)
		if (*at == arg)
		{
			*at = ((SiftV6SharedDim *) arg)->next;
			return;
		}
}

typedef struct Renumber
{
	Index		scanrelid;
	bool		same;			/* nothing that differs from one node to the next: a Var of another relation, a parameter, a sub-plan */
} Renumber;

/* the Vars of the scan as the relation 1's, so that the same scan of two nodes (other range table entries) is equal */
static Node *
renumber(Node *n, Renumber *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = copyObject((Var *) n);

		c->same &= v->varno == c->scanrelid;
		v->varno = v->varnosyn = 1;
		return (Node *) v;
	}
	c->same &= !IsA(n, Param) && !IsA(n, SubPlan) && !IsA(n, AlternativeSubPlan);
	return expression_tree_mutator(n, renumber, c);
}

static bool
shape_equal(const SiftStarShape *a, const SiftStarShape *b)
{
	if (a->mode != b->mode || a->nkeys != b->nkeys || a->npay != b->npay || a->budget != b->budget)
		return false;
	for (int k = 0; k < a->nkeys; k++)
		if (a->kcls[k] != b->kcls[k])
			return false;
	for (int c = 0; c < a->npay; c++)
		if (a->pcls[c] != b->pcls[c] || a->pwidth[c] != b->pwidth[c])
			return false;
	return true;
}

SiftV6SharedDim *
sift_v6_shared_dim_open(const PlanState *ps, const SiftV6XDim *xd, const SiftStarShape *sh, const SiftStarDim **hit)
{
	const Scan *scan = (const Scan *) ps->plan;
	EState	   *estate = ps->state;
	Renumber	rn = {.scanrelid = scan->scanrelid,.same = true};
	MemoryContext cx,
				old;
	SiftV6SharedDim *h;
	List	   *tlist,
			   *qual;
	Oid			relid;

	*hit = NULL;
	if (!IsA(ps->plan, SeqScan) || sift_star_preserves_dim(sh->mode) || ps->plan->initPlan != NIL || contain_volatile_functions((Node *) ps->plan->qual))
		return NULL;
	tlist = (List *) renumber((Node *) ps->plan->targetlist, &rn);
	qual = (List *) renumber((Node *) ps->plan->qual, &rn);
	relid = exec_rt_fetch(scan->scanrelid, estate)->relid;
	if (!rn.same || !OidIsValid(relid))
		return NULL;
	for (SiftV6SharedDim *e = shared_dims; e != NULL; e = e->next)
		if (e->ready && e->estate == estate && e->relid == relid && shape_equal(&e->shape, sh) &&
			memcmp(e->kcol, xd->kcol, xd->nkeys * sizeof(AttrNumber)) == 0 && memcmp(e->pcol, xd->pcol, xd->npay * sizeof(AttrNumber)) == 0 && equal(e->tlist, tlist) && equal(e->qual, qual))
		{
			*hit = &e->dim;
			return NULL;
		}
	cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 shared dimension");
	h = MemoryContextAllocZero(cx, sizeof(SiftV6SharedDim));
	h->estate = estate;
	h->relid = relid;
	h->shape = *sh;
	memcpy(h->kcol, xd->kcol, xd->nkeys * sizeof(AttrNumber));
	memcpy(h->pcol, xd->pcol, xd->npay * sizeof(AttrNumber));
	h->cx = cx;
	old = MemoryContextSwitchTo(cx);
	h->tlist = copyObject(tlist);
	h->qual = copyObject(qual);
	MemoryContextSwitchTo(old);
	h->forget = (MemoryContextCallback) {.func = shared_dim_forget,.arg = h};
	MemoryContextRegisterResetCallback(cx, &h->forget);
	h->next = shared_dims;
	shared_dims = h;
	return h;
}

MemoryContext
sift_v6_shared_dim_cx(const SiftV6SharedDim *h)
{
	return h->cx;
}

void
sift_v6_shared_dim_publish(SiftV6SharedDim *h, const SiftStarDim *dim)
{
	h->dim = *dim;
	h->ready = true;
}

void
sift_v6_shared_dim_abort(SiftV6SharedDim *h)
{
	MemoryContextDelete(h->cx);
}

typedef struct LookupReads
{
	double		read;
	double		loaded;
	double		index_scans;
} LookupReads;

static bool
lookup_reads(PlanState *ps, LookupReads *r)
{
	const Plan *plan = ps->plan;
	SiftNodeInstr *in = ps->instrument;

	if (in != NULL)
	{
		InstrEndLoop(in);
		if (IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan))
		{
			r->read += in->ntuples + in->nfiltered1 + in->nfiltered2;
			r->loaded += in->ntuples;
		}
		if (IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapIndexScan))
			r->index_scans += in->nloops;
	}
	return planstate_tree_walker(ps, lookup_reads, r);
}

void
sift_v6_explain_lookup_reads(List *dims, ExplainState *es)
{
	LookupReads r = {0};
	SiftExplainLine ln;

	if (!es->analyze)
		return;
	foreach_ptr(PlanState, ps, dims)
		lookup_reads(ps, &r);
	sift_explain_begin(&ln, es, "Lookup");
	sift_explain_int(&ln, "Lookup Rows Read", NULL, "rows read", (int64) r.read, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Lookup Rows Loaded", NULL, "rows loaded", (int64) r.loaded, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Lookup Index Scans", NULL, "index scans", (int64) r.index_scans, SIFT_XI_OPT);
	sift_explain_end(&ln);
}
