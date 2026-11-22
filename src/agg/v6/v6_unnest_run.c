/*
 * agg/v6/v6_unnest_run.c - the aggregate scan of a LATERAL unnest of an array column (agg/v6/v6_unnest_run.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "access/tableam.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_unnest.h"
#include "agg/v6/v6_unnest_run.h"
#include "agg/v6/v6_vet.h"
#include "agg/vexpr_str.h"
#include "col/am.h"
#include "col/f2/f2_sink_ext.h"
#include "core/errctx.h"
#include "core/memtrack.h"

typedef struct UnCol
{
	int			attno;
	int			leaf;
	SiftV6Class cls;
} UnCol;

typedef struct UnRun
{
	const SiftV6Spec *spec;
	const SiftV6Ext *x;
	SiftV6ScanStats *stats;
	SiftV6Vet	vet;
	SiftV6Pipe *pipe;
	SiftBatchSel bs;
	SiftBatchSel obs;			/* the pairs of a call of emit_pairs */
	int			ncols;
	UnCol		cols[SIFT_F2_PULL_MAX_COLS];
	SiftVecType etype;			/* of the element: SVEC_STRVIEW or SVEC_I64 */
	AttrNumber	heap_att;		/* the array column of the table */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftClause	where;
	bool		has_where;
	SiftVec   **wvecs;
	int		   *wcol;			/* the column of each vector the WHERE clause reads */
	SiftVec	   *out[SIFT_F2_PULL_MAX_COLS + 1];
	uint64	   *out_valid[SIFT_F2_PULL_MAX_COLS + 1];
	void	   *out_data[SIFT_F2_PULL_MAX_COLS + 1];
	SiftVec		outv[SIFT_F2_PULL_MAX_COLS + 1];
	uint32	   *pidx;
	uint32	   *eidx;
	MemoryContext bcx;			/* the arrays read from the heap for a batch */
} UnRun;

static SiftVecType
vec_type(SiftV6Class cls)
{
	return cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
}

static int
col_index(UnRun *u, const SiftColRel *r, const SiftV6Col *c)
{
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].attno == c->attno)
			return i;
	if (u->ncols == SIFT_F2_PULL_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_F2_PULL_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	u->cols[u->ncols] = (UnCol) {.attno = c->attno,.leaf = r->first[c->attno],.cls = sift_v6_class(c->typid)};
	return u->ncols++;
}

static void
setup_cols(UnRun *u, const SiftColRel *r)
{
	const SiftV6Spec *spec = u->spec;

	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].src != SV6SRC_ELEM)
			col_index(u, r, &spec->key[i]);
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		if (sift_v6_vec_col(spec, v) != NULL && sift_v6_vec_col(spec, v)->src != SV6SRC_ELEM)
			col_index(u, r, sift_v6_vec_col(spec, v));
}

static SiftF2PullSpec
pull_spec(const UnRun *u)
{
	SiftF2PullSpec ps = {.ncols = u->ncols,.nmasks = 0,.group_leaf = u->x->elem.group_leaf,.nchild = 1};

	for (int i = 0; i < u->ncols; i++)
		ps.col[i] = (SiftF2PullCol) {.leaf = u->cols[i].leaf,.type = vec_type(u->cols[i].cls)};
	ps.child[0] = (SiftF2PullCol) {.leaf = u->x->elem.elem_leaf,.type = u->etype};
	return ps;
}

static void
setup_pipe(UnRun *u, const SiftColRel *r, SiftV6Group *g)
{
	const SiftV6Spec *spec = u->spec;
	SiftVecType type[SIFT_F2_PULL_MAX_COLS + 1];

	for (int i = 0; i < u->ncols; i++)
		type[i] = vec_type(u->cols[i].cls);
	type[u->ncols] = u->etype;
	u->pipe = sift_v6_pipe_new(CurrentMemoryContext, spec, g, u->ncols + 1, type);
	for (int i = 0; i < spec->nkeys; i++)
		u->pipe->key_slot[i] = spec->key[i].src == SV6SRC_ELEM ? u->ncols : col_index(u, r, &spec->key[i]);
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
	{
		const SiftV6Col *c = sift_v6_vec_col(spec, v);

		u->pipe->arg_slot[v] = c == NULL ? -1 : c->src == SV6SRC_ELEM ? u->ncols : col_index(u, r, c);
	}
	for (int s = 0; s <= u->ncols; s++)
	{
		u->out_data[s] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sift_gather_width(type[s]) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		u->out_valid[s] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		u->out[s] = &u->outv[s];
	}
}

/* the pairs of (row, element) into the vectors of the pairs, and on to the groups: pidx[i] names the row of fact and eidx[i] the element of elems of pair i (SIFT_GATHER_NONE: a NULL one) */
static void
emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m)
{
	SiftBatchSel *bs = &u->obs;

	for (int i = 0; i < u->ncols; i++)
		sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &fact[i], u->pidx, m);
	sift_vec_gather(u->out[u->ncols], u->out_valid[u->ncols], u->out_data[u->ncols], elems, u->eidx, m);
	memset(bs->mask, 0, sizeof(bs->mask));
	for (uint32 i = 0; i < m; i++)
		bs->mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	bs->n_raw = bs->n_active = (uint16) m;
	bs->all_active = true;
	bs->has_sel = false;
	sift_v6_pipe_run(u->pipe, u->out, bs);
}

/* a row whose array the index cannot hold: the array of its heap row taken apart as unnest does, NULL and empty arrays included */
static void
emit_heap_array(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	Datum	   *elems = NULL;
	bool	   *nulls = NULL;
	int			nel = 0;
	bool		isnull,
				found;
	Datum		d;
	SiftVec		ev = {.type = u->etype};
	const size_t w = sift_gather_width(u->etype);

	found = sift_v6_vet_fetch(&u->vet, tid);
	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its array is read");
	d = slot_getattr(u->vet.slot, u->heap_att, &isnull);
	if (!isnull)
		deconstruct_array(DatumGetArrayTypeP(d), u->x->elem.type, u->typlen, u->typbyval, u->typalign, &elems, &nulls, &nel);
	ev.data = MemoryContextAllocAligned(u->bcx, Max((size_t) nel, 1) * w + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	ev.valid = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
	for (int k = 0; k < nel; k++)
	{
		if (nulls[k])
			continue;
		ev.valid[k >> 6] |= UINT64CONST(1) << (k & 63);
		if (u->etype == SVEC_STRVIEW)
		{
			struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(elems[k]));

			((SiftStrView *) ev.data)[k] = sift_vstr_make(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
		}
		else
			((int64 *) ev.data)[k] = u->x->elem.type == INT2OID ? DatumGetInt16(elems[k]) : u->x->elem.type == INT4OID ? DatumGetInt32(elems[k]) : DatumGetInt64(elems[k]);
	}
	for (int at = 0; at < Max(nel, 1) && (nel > 0 || u->x->elem.left); at += SIFT_VEC_LEN)
	{
		const uint32 m = nel == 0 ? 1 : (uint32) Min(SIFT_VEC_LEN, nel - at);

		for (uint32 i = 0; i < m; i++)
		{
			u->pidx[i] = row;
			u->eidx[i] = nel == 0 ? SIFT_GATHER_NONE : (uint32) at + i;
		}
		emit_pairs(u, fact, &ev, m);
	}
	MemoryContextSwitchTo(old);
}

static void
restrict_where(UnRun *u, const SiftF2ExtBatch *b)
{
	SiftBatchSel *bs = &u->bs;
	uint64		keep[SIFT_VEC_WORDS];

	for (int i = 0; i < u->spec->nfcols; i++)
		u->wvecs[i] = (SiftVec *) &b->col[u->wcol[i]];
	sift_clause_eval(&u->where, u->wvecs, b->n, keep);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		bs->mask[w] &= keep[w];
}

static void
run_batch(UnRun *u, const SiftF2ExtBatch *b)
{
	SiftBatchSel *bs = &u->bs;
	const int	n = b->n;
	const int	words = (n + 63) / 64;
	const SiftF2ElemStream *es = b->elem;
	uint64		fallback[SIFT_VEC_WORDS] = {0};
	SiftVec		ev = {.type = u->etype,.all_valid = false};

	u->stats->batches++;
	memset(bs->mask, 0, sizeof(bs->mask));
	memcpy(bs->mask, b->live, words * sizeof(uint64));
	if (n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (n & 63)) - 1;
	if (u->has_where)
		restrict_where(u, b);
	if (sift_v6_vet_mask(&u->vet, b->tid, 0, false, n, bs) == 0)
		return;
	u->stats->rows += bs->n_active;
	MemoryContextReset(u->bcx);
	ev.data = (void *) es->child_data[0];
	ev.valid = (uint64 *) es->child_valid[0];
	if (es->layout.n_parents < (uint32) n)
		memcpy(fallback, bs->mask, words * sizeof(uint64));
	else
	{
		const SiftUnnestIn in = {.lay = &es->layout,.parent_mask = bs->mask,.poison = es->poison,.doc_valid = es->doc_valid,.left = u->x->elem.left};
		SiftUnnestCursor cur = {0};

		while (!sift_unnest_done(&in, &cur))
		{
			const uint32 m = sift_unnest_expand(&in, &cur, u->pidx, u->eidx, SIFT_VEC_LEN, fallback);

			if (m > 0)
				emit_pairs(u, b->col, &ev, m);
		}
	}
	for (int w = 0; w < words; w++)
		for (uint64 bits = fallback[w] & bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + __builtin_ctzll(bits);

			emit_heap_array(u, b->col, b->tid[row], row);
		}
}

void
sift_v6_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats)
{
	UnRun		run = {0};
	UnRun	   *u = &run;
	SiftF2PullSpec ps;
	SiftF2Pull *pull;
	const SiftF2ExtBatch *b;

	u->spec = spec;
	u->x = spec->ext;
	u->stats = stats;
	u->etype = sift_v6_class(u->x->elem.type) == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
	u->heap_att = index->rd_index->indkey.values[u->x->elem.attno - 1];
	get_typlenbyvalalign(u->x->elem.type, &u->typlen, &u->typbyval, &u->typalign);
	u->bcx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 unnest heap rows");
	u->pidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->eidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	setup_cols(u, sift_col_rel(index));
	setup_pipe(u, sift_col_rel(index), g);
	if (u->x->where != NIL)
	{
		bool		bound;

		u->has_where = true;
		bound = sift_v6_clause_bind(CurrentMemoryContext, u->x->where, spec, &u->where);
		SIFT_INVARIANT(bound, "the WHERE clause of the aggregate scan of an unnest compiled when it was planned");
		u->wvecs = palloc(Max(spec->nfcols, 1) * sizeof(SiftVec *));
		u->wcol = palloc(Max(spec->nfcols, 1) * sizeof(int));
		for (int i = 0; i < spec->nfcols; i++)
			u->wcol[i] = col_index(u, sift_col_rel(index), &spec->fcol[i]);
	}
	sift_v6_vet_begin(&u->vet, heap, snap, stats);
	ps = pull_spec(u);
	pull = sift_f2_pull_begin(index, snap, &ps);
	while ((b = sift_f2_pull_next(pull)) != NULL)
		run_batch(u, b);
	sift_f2_pull_end(pull);
	sift_v6_vet_end(&u->vet);
	MemoryContextDelete(u->bcx);
}
