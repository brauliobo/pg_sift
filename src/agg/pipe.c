/*
 * agg/pipe.c - the batches of the fact side through the dimensions of a star join to the groups (agg/pipe.h).
 */
/* check-memory: allow-allocs every array here follows the slots of one statement, not the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/datum.h"
#include "agg/gather.h"
#include "agg/expr/pg.h"
#include "agg/join/found.h"
#include "agg/pipe.h"
#include "agg/topn/rows.h"
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
	uint64	   *nan;			/* [nslots * SIFT_VEC_WORDS] the NaN rows of the numeric slots that have any */
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

	SIFT_INVARIANT(sift_v6_pgexpr_vars(e, SIFT_V6_PGX_RELID, 0, &vars), "a key of the join is an expression of at most %d columns", SIFT_V6_MAX_COLS);
	ke->nin = list_length(vars);
	for (int j = 0; j < ke->nin; j++)
	{
		const SiftV6Col *c = &x->rcol[((const Var *) list_nth(vars, j))->varattno - 1];

		ke->col[j] = (int) (c - x->rcol);
		pin[j] = (SiftV6PgIn) {.typid = c->typid,.scale = c->scale};
	}
	ke->x = sift_v6_pgexpr_open(p->cx, NULL, sift_v6_pgexpr_renumber(e, SIFT_V6_PGX_RELID, 0, vars), ke->nin, pin);
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
	p->pay_slot[SIFT_V6_JOINED_DIM] = at;
	p->njoined = x != NULL ? list_length(x->jexpr) : 0;
	p->nslots = at + p->njoined;
	p->type = MemoryContextAlloc(cx, p->nslots * sizeof(SiftVecType));
	p->typlen = MemoryContextAllocZero(cx, p->nslots * sizeof(int16));
	p->typbyval = MemoryContextAllocZero(cx, p->nslots * sizeof(bool));
	for (int i = 0; i < nfact; i++)
		p->type[i] = ftype[i];
	for (int d = 0; d < p->ndims; d++)
		for (int c = 0; c < x->dim[d].npay; c++)
		{
			const Oid	typid = x->dim[d].ptype[c];
			const SiftV6Class cls = sift_v6_class(typid);

			const int	s = p->pay_slot[d] + c;

			p->type[s] = !sift_v6_type_native(typid) ? SVEC_DATUM : cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
			if (p->type[s] == SVEC_DATUM)
				get_typlenbyval(typid, &p->typlen[s], &p->typbyval[s]);
		}
	p->joined = MemoryContextAlloc(cx, Max(p->njoined, 1) * sizeof(SiftV6KeyExpr *));
	for (int j = 0; j < p->njoined; j++)
	{
		const int	s = p->pay_slot[SIFT_V6_JOINED_DIM] + j;

		p->joined[j] = key_expr_new(p, list_nth(x->jexpr, j));
		const Oid	restype = p->joined[j]->x->restype;

		p->type[s] = sift_v6_type_native(restype) ? sift_v6_result_vec_type(restype) : SVEC_DATUM;
		if (p->type[s] == SVEC_DATUM)
			get_typlenbyval(restype, &p->typlen[s], &p->typbyval[s]);
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
	p->semi_slot = -1;
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
		return sift_v6_kind_reads_column(spec->agg[v].kind) && (!v6g_is_gen(&spec->agg[v]) || OidIsValid(spec->agg[v].arg.typid)) ? &spec->agg[v].arg : NULL;
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
	b->nan = MemoryContextAllocZero(p->cx, (size_t) p->nslots * SIFT_VEC_WORDS * sizeof(uint64));
	b->data = MemoryContextAllocZero(p->cx, p->nslots * sizeof(char *));
	for (int s = l == 0 ? p->nfact : 0; s < p->nslots; s++)
		b->data[s] = vec_buffer(p->cx, p->type[s]);
	b->bs = &b->own;
	return b;
}

SiftV6Pipe *
sift_v6_pipe_clone(MemoryContext cx, const SiftV6Pipe *p, SiftV6BatchFn sink, void *sink_arg)
{
	SiftV6Pipe *c = sift_v6_pipe_new(cx, p->spec, NULL, NULL, p->nfact, p->type);

	c->sink = sink;
	c->sink_arg = sink_arg;
	memcpy(c->dim, p->dim, sizeof(c->dim));
	memcpy(c->found, p->found, sizeof(c->found));
	memcpy(c->found_slot, p->found_slot, sizeof(c->found_slot));
	memcpy(c->nparts, p->nparts, sizeof(c->nparts));
	memcpy(c->part, p->part, sizeof(c->part));
	memcpy(c->fslot, p->fslot, sizeof(c->fslot));
	memcpy(c->key_slot, p->key_slot, Max(p->spec->nkeys, 1) * sizeof(int));
	memcpy(c->arg_slot, p->arg_slot, Max(sift_v6_spec_nvec(p->spec), 1) * sizeof(int));
	for (int l = 0; l <= c->ndims; l++)
		level_of(c, l);
	return c;
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
	if (p->sink != NULL)
		p->sink(p->sink_arg, b->bs, p->keyvec, p->argvec);
	else if (p->rows != NULL)
		sift_v6_rows_update(p->rows, b->bs, p->keyvec, (const uint64 *) b->vec[p->nfact - 1].data);
	else
		sift_v6_group_update(p->group, b->bs, p->keyvec, p->argvec);
	p->rows_out += b->bs->n_active;
}

void
sift_v6_pipe_semi(SiftV6Pipe *p, int slot)
{
	p->semi_slot = slot;
	p->semi_seen = MemoryContextAllocZero(p->cx, SIFT_VEC_WORDS * sizeof(uint64));
}

void
sift_v6_pipe_semi_reset(SiftV6Pipe *p)
{
	memset(p->semi_seen, 0, SIFT_VEC_WORDS * sizeof(uint64));
}

/* of the pairs of a fact row, only the first that went through everything goes on: the row counts once */
static void
semi_once(SiftV6Pipe *p, SiftV6PipeBatch *b)
{
	SiftBatchSel *bs = b->bs;
	const int64 *row = (const int64 *) b->vec[p->semi_slot].data;

	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 r = w * 64 + (uint32) __builtin_ctzll(bits);
			uint64	   *seen = &p->semi_seen[row[r] >> 6];
			const uint64 bit = UINT64CONST(1) << (row[r] & 63);

			if (*seen & bit)
				bs->mask[w] &= ~(UINT64CONST(1) << (r & 63));
			*seen |= bit;
		}
	count_active(bs);
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
			vec.typlen = p->typlen[s];
			vec.typbyval = p->typbyval[s];
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

/* the batch of the m pairs (idx[i], dim_row[i]) of dimension d: the columns of the fact side and the earlier dimensions follow their row of b, the payload of d is that of its row (0: NULL) */
static SiftV6PipeBatch *
pairs_batch(SiftV6Pipe *p, int d, const SiftV6PipeBatch *b, const uint32 *idx, const uint32 *dim_row, uint32 m)
{
	SiftV6PipeBatch *nb = level_of(p, d + 1);
	const int	nown = p->pay_slot[d];

	for (int s = 0; s < nown; s++)
		if (b->vec[s].data != NULL)
		{
			sift_vec_gather(&nb->vec[s], nb->valid + (size_t) s * SIFT_VEC_WORDS, nb->data[s], &b->vec[s], idx, m);
			sift_vec_gather_nan(&nb->vec[s], nb->nan + (size_t) s * SIFT_VEC_WORDS, &b->vec[s], idx, m);
		}
	gather_payload(p, d, nb, dim_row, m);
	memset(nb->own.mask, 0, sizeof(nb->own.mask));
	for (uint32 i = 0; i < m; i++)
		nb->own.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	nb->own.n_raw = nb->own.n_active = (uint16) m;
	nb->own.all_active = true;
	nb->own.has_sel = false;
	return nb;
}

/* a dimension with a key more than once: the pairs of (row, dimension row), a batch of them at a time, go on to the next dimension and then to the groups */
static void
expand(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr)
{
	SiftStarCursor cur = {0};
	uint32		idx[SIFT_VEC_LEN];

	while (!sift_star_cursor_done(&cur, pr->n))
	{
		const uint32 m = sift_star_expand(p->dim[d], pr, NULL, &cur, p->fact_idx, p->dim_row, SIFT_VEC_LEN);

		if (m == 0)
			break;
		if (p->dim[d]->matched != NULL)
			sift_v6_lookup_mark(p->dim[d], p->dim_row, m);
		for (uint32 i = 0; i < m; i++)
			idx[i] = p->fact_idx[i];
		stage(p, d + 1, pairs_batch(p, d, b, idx, p->dim_row, m));
	}
}

/* the dimension rows of the pairs a condition kept, which a RIGHT or FULL join counts as matched; the others are not */
static void
mark_kept(SiftV6Pipe *p, int d, const SiftV6PipeBatch *nb, const uint32 *dim_row, uint32 m)
{
	for (uint32 i = 0; i < m; i++)
		p->match[i] = (nb->own.mask[i >> 6] >> (i & 63)) & 1 ? dim_row[i] : 0;
	sift_v6_lookup_mark(p->dim[d], p->match, m);
}

/* the rows of b no pair of a condition kept, for a LEFT or FULL join: each is one pair with no dimension row */
static void
keep_unmatched(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr, const uint8 *hit)
{
	uint32	   *idx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	uint32	   *none = palloc0(SIFT_VEC_LEN * sizeof(uint32));
	uint32		m = 0;

	for (uint32 r = 0; r < pr->n; r++)
		if (((pr->active[r >> 6] >> (r & 63)) & 1) && !hit[r])
			idx[m++] = r;
	if (m > 0)
		stage(p, d + 1, pairs_batch(p, d, b, idx, none, m));
	pfree(idx);
	pfree(none);
}

/* a dimension with a condition on its pairs (the ON clause of an outer join that no key says): the pairs the condition keeps are the matches; a fact row of a LEFT or FULL join with none stays, with no dimension row */
static void
expand_on(SiftV6Pipe *p, int d, SiftV6PipeBatch *b, const SiftStarProbe *pr)
{
	const SiftV6JoinMode mode = p->dim[d]->shape.mode;
	SiftStarCursor cur = {0};
	uint32	   *idx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	uint8	   *hit = palloc0(pr->n);

	while (!sift_star_cursor_done(&cur, pr->n))
	{
		const uint32 m = sift_star_expand(p->dim[d], pr, NULL, &cur, p->fact_idx, p->dim_row, SIFT_VEC_LEN);
		const SiftVec *rv[SIFT_V6_RESID_COLS];
		SiftV6PipeBatch *nb;

		if (m == 0)
			break;
		for (uint32 i = 0; i < m; i++)
			idx[i] = p->fact_idx[i];
		nb = pairs_batch(p, d, b, idx, p->dim_row, m);
		for (int i = 0; i < p->spec->ext->nrcol; i++)
			rv[i] = &nb->vec[p->rslot[i]];
		sift_v6_resid_filter(p->resid, d, rv, &nb->own);
		count_active(&nb->own);
		for (uint32 i = 0; i < m; i++)
			hit[idx[i]] |= (nb->own.mask[i >> 6] >> (i & 63)) & 1;
		if (p->dim[d]->matched != NULL)
			mark_kept(p, d, nb, p->dim_row, m);
		stage(p, d + 1, nb);
	}
	if (sift_star_keeps_fact(mode))
		keep_unmatched(p, d, b, pr, hit);
	pfree(idx);
	pfree(hit);
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
		if (sift_v6_resid_has(p->resid, SIFT_V6_RESID_JOIN))
		{
			const SiftVec *rv[SIFT_V6_RESID_COLS];

			for (int i = 0; i < p->spec->ext->nrcol; i++)
				rv[i] = &b->vec[p->rslot[i]];
			sift_v6_resid_filter(p->resid, SIFT_V6_RESID_JOIN, rv, bs);
			count_active(bs);
			if (bs->n_active == 0)
				return;
		}
		if (p->semi_seen != NULL)
		{
			semi_once(p, b);
			if (bs->n_active == 0)
				return;
		}
		for (int j = 0; j < p->njoined; j++)
			b->vec[p->pay_slot[SIFT_V6_JOINED_DIM] + j] = *key_expr_vec(p, p->joined[j], b);
		update_groups(p, b);
		return;
	}
	if (p->found[d] != NULL)
	{
		sift_v6_found_apply(p->found[d], &b->vec[p->found_slot[d]], &b->vec[p->fslot[d][0]], bs);
		for (int c = 0; c < p->spec->ext->dim[d].npay; c++)
			b->vec[p->pay_slot[d] + c] = b->vec[p->fslot[d][0]];
		count_active(bs);
		stage(p, d + 1, b);
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
	if (sift_v6_resid_has(p->resid, d))
	{
		expand_on(p, d, b, &pr);
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
