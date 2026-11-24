/*
 * agg/v6/v6_pipe.c - the batches of the fact side through the dimensions of a star join to the groups (agg/v6/v6_pipe.h).
 */
/* check-memory: allow-allocs every array here follows the slots of one statement, not the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_rows_topn.h"
#include "core/popcount.h"

/* A key the probing side computes: the expression over the columns rcol[col[0 ..]], whose vector comes out of PostgreSQL's evaluator before the dimension is probed */
struct SiftV6KeyExpr
{
	SiftV6PgExpr *x;
	int			nin;
	int			col[SIFT_V6_MAX_COLS];
	SiftVxOut	out;
};

struct SiftV6PipeBatch
{
	SiftVec    *vec;			/* [nslots] */
	uint64	   *valid;			/* [nslots * SIFT_VEC_WORDS] */
	char	  **data;			/* [nslots] the values of the slots this batch owns */
	SiftBatchSel own;
	SiftBatchSel *bs;
};

/* the virtual attribute of a Var of the expression is the 1-based place of its column in x->rcol: the inputs of the evaluator are those the expression reads */
static SiftV6KeyExpr *
key_expr_new(SiftV6Pipe *p, Expr *e)
{
	SiftV6KeyExpr *ke = MemoryContextAllocZero(p->cx, sizeof(SiftV6KeyExpr));
	const SiftV6Ext *x = p->spec->ext;
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];
	List	   *vars;

	SIFT_INVARIANT(sift_v6_pgexpr_vars(e, SIFT_V6_PGX_RELID, &vars), "a key of the join is an expression of at most %d columns", SIFT_V6_MAX_COLS);
	ke->nin = list_length(vars);
	for (int j = 0; j < ke->nin; j++)
	{
		const SiftV6Col *c = &x->rcol[((const Var *) list_nth(vars, j))->varattno - 1];

		ke->col[j] = (int) (c - x->rcol);
		pin[j] = (SiftV6PgIn) {.typid = c->typid,.scale = c->scale};
	}
	ke->x = sift_v6_pgexpr_open(p->cx, NULL, sift_v6_pgexpr_renumber(e, SIFT_V6_PGX_RELID, vars), ke->nin, pin);
	ke->x->restype = getBaseType(exprType((Node *) e));
	sift_vx_out_init(&ke->out, p->cx);
	return ke;
}

SiftV6Pipe *
sift_v6_pipe_new(MemoryContext cx, const SiftV6Spec *spec, SiftV6Group *group, SiftV6Rows *rows, int nfact, const SiftVecType *ftype)
{
	SiftV6Pipe *p = MemoryContextAllocZero(cx, sizeof(SiftV6Pipe));
	const SiftV6Ext *x = spec->ext;
	int			at = nfact;

	p->cx = cx;
	p->spec = spec;
	p->group = group;
	p->rows = rows;
	p->nfact = nfact;
	p->ndims = x != NULL ? x->ndims : 0;
	for (int d = 0; d < p->ndims; d++)
	{
		p->pay_slot[d] = at;
		at += x->dim[d].npay;
		p->nparts[d] = 1;
	}
	p->nslots = at;
	p->type = MemoryContextAlloc(cx, p->nslots * sizeof(SiftVecType));
	for (int i = 0; i < nfact; i++)
		p->type[i] = ftype[i];
	for (int d = 0; d < p->ndims; d++)
		for (int c = 0; c < x->dim[d].npay; c++)
		{
			const Oid	typid = x->dim[d].ptype[c];
			const SiftV6Class cls = sift_v6_class(typid);

			p->type[p->pay_slot[d] + c] = !sift_v6_type_native(typid) ? SVEC_DATUM : cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
		}
	p->key_slot = MemoryContextAlloc(cx, Max(spec->nkeys, 1) * sizeof(int));
	p->arg_slot = MemoryContextAlloc(cx, Max(sift_v6_spec_nvec(spec), 1) * sizeof(int));
	p->keyvec = MemoryContextAlloc(cx, Max(spec->nkeys, 1) * sizeof(SiftVec *));
	p->argvec = MemoryContextAlloc(cx, Max(sift_v6_spec_nvec(spec), 1) * sizeof(SiftVec *));
	p->resid = sift_v6_resid_new(cx, x);
	for (int d = 0; d < p->ndims; d++)
		for (int k = 0; k < x->dim[d].nkeys; k++)
			if (x->dim[d].kx[k] > 0)
				p->kexpr[d][k] = key_expr_new(p, list_nth(x->kexpr, x->dim[d].kx[k] - 1));
	p->match = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint32));
	p->fact_idx = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint16));
	p->dim_row = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint32));
	return p;
}

/* the column the vector v of a batch is: argvec[j] is the argument of aggregate j, argvec[naggs + j] its second, then the columns of the COUNT(DISTINCT ..) rows and those of the FILTER clauses */
const SiftV6Col *
sift_v6_vec_col(const SiftV6Spec *spec, int v)
{
	if (v < spec->naggs)
		return sift_v6_kind_reads_column(spec->agg[v].kind) ? &spec->agg[v].arg : NULL;
	if (v < 2 * spec->naggs)
		return spec->agg[v - spec->naggs].kind == SV6_ARG_MAX || spec->agg[v - spec->naggs].kind == SV6_ARG_MIN || (v6g_is_gen(&spec->agg[v - spec->naggs]) && OidIsValid(spec->agg[v - spec->naggs].arg2.typid)) ? &spec->agg[v - spec->naggs].arg2 : NULL;
	if (v >= sift_v6_spec_fvec(spec))
		return &spec->fcol[v - sift_v6_spec_fvec(spec)];
	for (int j = 0; j < spec->naggs; j++)
		if (v >= sift_v6_agg_xvec(spec, j) && v < sift_v6_agg_xvec(spec, j) + spec->agg[j].nxcols)
			return &spec->agg[j].xcol[v - sift_v6_agg_xvec(spec, j)];
	return NULL;
}

int
sift_v6_pipe_dim_slot(const SiftV6Pipe *p, int dim, int pay)
{
	return p->pay_slot[dim] + pay;
}

static void *
vec_buffer(MemoryContext cx, SiftVecType t)
{
	return MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sift_gather_width(t) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
}

/* the batch of level l: level 0 borrows the fact vectors, the others own every slot they can hold */
static SiftV6PipeBatch *
level_of(SiftV6Pipe *p, int l)
{
	SiftV6PipeBatch *b = p->lvl[l];

	if (b != NULL)
		return b;
	b = p->lvl[l] = MemoryContextAllocAligned(p->cx, sizeof(SiftV6PipeBatch), alignof(SiftV6PipeBatch), MCXT_ALLOC_ZERO);
	b->vec = MemoryContextAllocZero(p->cx, p->nslots * sizeof(SiftVec));
	b->valid = MemoryContextAllocZero(p->cx, (size_t) p->nslots * SIFT_VEC_WORDS * sizeof(uint64));
	b->data = MemoryContextAllocZero(p->cx, p->nslots * sizeof(char *));
	for (int s = l == 0 ? p->nfact : 0; s < p->nslots; s++)
		b->data[s] = vec_buffer(p->cx, p->type[s]);
	b->bs = &b->own;
	return b;
}

static void
count_active(SiftBatchSel *bs)
{
	uint32		n = 0;

	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
		n += (uint32) sift_pc64(bs->mask[w]);
	bs->n_active = (uint16) n;
	bs->all_active = n == bs->n_raw;
	bs->has_sel = false;
}

static void
update_groups(SiftV6Pipe *p, SiftV6PipeBatch *b)
{
	const SiftV6Spec *spec = p->spec;

	for (int i = 0; i < spec->nkeys; i++)
		p->keyvec[i] = &b->vec[p->key_slot[i]];
	for (int j = 0; j < sift_v6_spec_nvec(spec); j++)
		p->argvec[j] = p->arg_slot[j] < 0 ? NULL : &b->vec[p->arg_slot[j]];
	if (p->rows != NULL)
		sift_v6_rows_update(p->rows, b->bs, p->keyvec, (const uint64 *) b->vec[p->nfact - 1].data);
	else
		sift_v6_group_update(p->group, b->bs, p->keyvec, p->argvec);
	p->rows_out += b->bs->n_active;
}

/* the payload columns of dimension d for the rows b holds, from the rows of the dimension dim_row names (0: none, a NULL) */
static void
gather_payload(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const uint32 *dim_row, uint32 n)
{
	const SiftStarDim *dim = p->dim[d];

	for (int c = 0; c < p->spec->ext->dim[d].npay; c++)
	{
		const int	s = p->pay_slot[d] + c;
		uint64	   *valid = b->valid + (size_t) s * SIFT_VEC_WORDS;

		SiftVec		vec = {.data = b->data[s],.valid = valid,.len = (uint16) n,.all_valid = false,.dense = true,.type = p->type[s]};

		if (p->type[s] == SVEC_STRVIEW)
			sift_star_gather_str(dim, c, dim_row, (uint16) n, (SiftStrView *) b->data[s], valid);
		else if (p->type[s] == SVEC_DATUM && dim->shape.pcls[c] == SSC_STR)
			sift_star_gather_datum(dim, c, dim_row, (uint16) n, (uint64 *) b->data[s], valid);
		else
			sift_star_gather_bits(dim, c, dim_row, (uint16) n, (uint64 *) b->data[s], valid);
		if (p->type[s] == SVEC_DATUM)
		{
			vec.typid = p->spec->ext->dim[d].ptype[c];
			get_typlenbyval(vec.typid, &vec.typlen, &vec.typbyval);
		}
		b->vec[s] = vec;
	}
}

static void
restrict_part(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr)
{
	SiftBatchSel *bs = b->bs;

	for (uint32 r = 0; r < bs->n_raw; r++)
		if (((bs->mask[r >> 6] >> (r & 63)) & 1) && sift_star_probe_part(&p->dim[d]->shape, pr, r, p->nparts[d]) != p->part[d])
			bs->mask[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
	count_active(bs);
}

static void stage(SiftV6Pipe *p, int d, SiftV6PipeBatch *b);

/* the vector of the key for the rows of the batch, from the columns the batch holds before the dimension */
static const SiftVec *
key_expr_vec(SiftV6Pipe *p, SiftV6KeyExpr *ke, SiftV6PipeBatch *b)
{
	const SiftVec *in[SIFT_V6_MAX_COLS];

	for (int j = 0; j < ke->nin; j++)
		in[j] = &b->vec[p->rslot[ke->col[j]]];
	sift_v6_pgexpr_derive(ke->x, in, b->bs, &ke->out);
	return &ke->out.vec;
}

/* a dimension with a key more than once: the pairs of (row, dimension row), a batch of them at a time, go on to the next dimension */
static void
expand(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr)
{
	SiftStarCursor cur = {0};
	uint32	   *idx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	const int	nown = p->pay_slot[d];

	while (!sift_star_cursor_done(&cur, pr->n))
	{
		const uint32 m = sift_star_expand(p->dim[d], pr, NULL, &cur, p->fact_idx, p->dim_row, SIFT_VEC_LEN);
		SiftV6PipeBatch *nb;

		if (m == 0)
			break;
		if (p->dim[d]->matched != NULL)
			sift_v6_lookup_mark(p->dim[d], p->dim_row, m);
		nb = level_of(p, d + 1);
		for (uint32 i = 0; i < m; i++)
			idx[i] = p->fact_idx[i];
		for (int s = 0; s < nown; s++)
			if (b->vec[s].data != NULL)
				sift_vec_gather(&nb->vec[s], nb->valid + (size_t) s * SIFT_VEC_WORDS, nb->data[s], &b->vec[s], idx, m);
		gather_payload(p, d, nb, p->dim_row, m);
		memset(nb->own.mask, 0, sizeof(nb->own.mask));
		for (uint32 i = 0; i < m; i++)
			nb->own.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		nb->own.n_raw = nb->own.n_active = (uint16) m;
		nb->own.all_active = true;
		nb->own.has_sel = false;
		stage(p, d + 1, nb);
	}
	pfree(idx);
}

static void
stage(SiftV6Pipe *p, int d, SiftV6PipeBatch *b)
{
	SiftBatchSel *bs = b->bs;
	const SiftStarDim *dim;
	SiftStarProbe pr;

	if (bs->n_active == 0)
		return;
	if (d == p->ndims)
	{
		if (p->resid != NULL)
		{
			const SiftVec *rv[SIFT_V6_RESID_COLS];

			for (int i = 0; i < p->spec->ext->nrcol; i++)
				rv[i] = &b->vec[p->rslot[i]];
			sift_v6_resid_filter(p->resid, rv, bs);
			count_active(bs);
			if (bs->n_active == 0)
				return;
		}
		update_groups(p, b);
		return;
	}
	dim = p->dim[d];
	pr = (SiftStarProbe) {.n = bs->n_raw,.active = bs->mask};
	for (int k = 0; k < dim->shape.nkeys; k++)
		pr.key[k] = p->kexpr[d][k] != NULL ? key_expr_vec(p, p->kexpr[d][k], b) : &b->vec[p->fslot[d][k]];
	if (p->nparts[d] > 1)
	{
		restrict_part(p, d, b, &pr);
		if (bs->n_active == 0)
			return;
	}
	if (!dim->unique_keys && dim->shape.mode != SV6J_SEMI_EQUI && dim->shape.mode != SV6J_ANTI_EQUI)
	{
		expand(p, d, b, &pr);
		return;
	}
	sift_star_probe(dim, &pr, p->match);
	sift_star_apply(dim, p->match, NULL, bs->n_raw, bs->mask);
	if (dim->matched != NULL)
		sift_v6_lookup_mark(dim, p->match, bs->n_raw);
	count_active(bs);
	if (bs->n_active == 0)
		return;
	gather_payload(p, d, b, p->match, bs->n_raw);
	stage(p, d + 1, b);
}

void
sift_v6_pipe_run(SiftV6Pipe *p, SiftVec *const *fact, SiftBatchSel *bs)
{
	SiftV6PipeBatch *b = level_of(p, 0);

	for (int i = 0; i < p->nfact; i++)
		b->vec[i] = *fact[i];
	b->bs = bs;
	stage(p, 0, b);
}

void
sift_v6_pipe_finish(SiftV6Pipe *p)
{
	for (int d = 0; d < p->ndims; d++)
	{
		const SiftStarDim *dim = p->dim[d];
		uint32		next = 0,
					m;

		if (dim->matched == NULL)
			continue;
		while ((m = sift_v6_lookup_unmatched(dim, &next, p->dim_row, SIFT_VEC_LEN)) > 0)
		{
			SiftV6PipeBatch *nb = level_of(p, d + 1);

			for (int s = 0; s < p->pay_slot[d]; s++)
			{
				uint64	   *valid = nb->valid + (size_t) s * SIFT_VEC_WORDS;

				memset(valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
				memset(nb->data[s], 0, SIFT_VEC_LEN * sift_gather_width(p->type[s]));
				nb->vec[s] = (SiftVec) {.data = nb->data[s],.valid = valid,.len = (uint16) m,.all_valid = false,.dense = true,.type = p->type[s]};
			}
			gather_payload(p, d, nb, p->dim_row, m);
			memset(nb->own.mask, 0, sizeof(nb->own.mask));
			for (uint32 i = 0; i < m; i++)
				nb->own.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
			nb->own.n_raw = nb->own.n_active = (uint16) m;
			nb->own.all_active = true;
			nb->own.has_sel = false;
			stage(p, d + 1, nb);
		}
	}
	sift_v6_resid_free(p->resid);
	p->resid = NULL;
}
