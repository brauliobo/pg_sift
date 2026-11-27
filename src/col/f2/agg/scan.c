/*
 * agg/scan.c - the rows of the scan of an index of format major 2 or of a sift_table as batches of up to 2048 rows: the columns of the keys and the aggregates as vectors, and every batch folded
 * into the groups. The rows of an index the heap does not show to the snapshot are left out (a page the visibility map has all-visible needs no look); a table's rows are visible by its own versions.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "executor/tuptable.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/ht/ht.h"
#include "agg/engine.h"
#include "agg/group/int.h"
#include "agg/filter/eval.h"
#include "agg/group/fast.h"
#include "agg/fuse/fuse.h"
#include "agg/win/datebin_pg.h"
#include "col/f2/agg/colstat.h"
#include "agg/dictseen.h"
#include "agg/ext.h"
#include "agg/pipe.h"
#include "col/f2/agg/preagg.h"
#include "agg/star/run.h"
#include "agg/vet.h"
#include "agg/qual/bqual.h"
#include "agg/expr/num.h"
#include "agg/expr/str.h"
#include "agg/topn/rows.h"
#include "agg/par/pinrun.h"
#include "agg/par/run.h"
#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/sink/sink.h"
#include "col/f2/scope/route.h"
#include "col/f2/store/scan.h"
#include "col/f2/store/schema.h"
#include "core/memtrack.h"
#include "col/f2/agg/scan_int.h"
#include "col/f2/agg/unnest/run.h"

/* The JSON type a row holds when the leaf holds its value: a boolean leaf a boolean, a text leaf a string, any other a number */
static uint8
type_of_leaf_value(const V6Scan *s, int leaf)
{
	const SiftColSchema *leaves = s->is_table ? sift_f2_store_open(s->host)->schema : sift_col_rel(s->host)->schema;
	const SiftColDef *d = &leaves->cols[leaf];

	return d->type == SIFT_COLT_BOOL ? SIFT_COLTAG_BOOLEAN : d->type == SIFT_COLT_TEXT ? SIFT_COLTAG_STRING : SIFT_COLTAG_NUMBER;
}

/* the vector of a column of a leaf: 16 bytes an element for a text or a JSON key, 8 for the others (a datum too) */
static void
col_init(V6Scan *s, int leaf, const SiftV6Col *c, bool key, bool takes_nan)
{
	V6Col	   *n = &s->cols[s->ncols];

	*n = (V6Col) {.leaf = leaf,.attno = c->attno,.scale = c->scale,.json = c->jleaf > 0,.jtype = c->jtype,.jsonb_cast = c->jsonb_cast,.jbase = c->jtype ? type_of_leaf_value(s, leaf) : 0,
		.typid = c->typid,.cls = sift_v6_col_class(c),.nan_ok = takes_nan,.key_only = key,.is_table = s->is_table && c->jleaf == 0};
	n->bare = n->is_table && sift_f2_store_open(s->host)->headerless && sift_f2_type_bare_bytes(c->typid);
	/* MEM[MEM-AGG-05]: SiftV6Scan batch column buffer (SIFT_VEC_LEN * elem + SIFT_VEC_PAD: 8B an element, 16KB a column; 16B, 32KB, only for a text or JSON column; was 32KB for every column) */
	n->vec.data = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * (n->cls == SV6C_TEXT || c->jleaf > 0 ? sizeof(SiftStrView) : sizeof(int64)) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	s->ncols++;
}

/* the leaves after the head of a datum column, which its value is made of */
static void
lane_cols(V6Scan *s, int head)
{
	const int	lanes = s->is_table ? 1 : sift_col_def_value_lanes(&s->defs[head]);

	for (int k = 1; k < lanes; k++)
	{
		col_init(s, head + k, &(SiftV6Col) {.attno = s->cols[s->ncols - 1].attno,.typid = INT8OID}, false, false);
		s->cols[s->ncols - 1].lane = true;
	}
}

/* a derived column of an expression with no column in it: the scan has nothing to decode for it */
static bool
sourceless(const V6Scan *s, const SiftV6Col *c)
{
	return c->derive > 0 && sift_v6_pgexpr_sourceless(s->spec, &s->spec->derive[c->derive - 1]);
}

static V6Col *
col_for(V6Scan *s, const int *first, SiftV6Col c, bool key, bool takes_nan)
{
	V6Col	   *n;
	int			leaf;

	if (c.derive > 0)
		c = s->spec->derive[c.derive - 1].src;
	if (sift_v6_vatt_is_json(c.attno))
		c = sift_v6_vatt_col(c.attno, c.attno, c.typid, c.scale);
	leaf = c.attno == SIFT_V6_SCORE_ATTNO ? -1 : c.jleaf > 0 ? c.jleaf - 1 : first[c.attno];

	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].leaf == leaf && !s->cols[i].lane && s->cols[i].jtype == c.jtype && s->cols[i].jsonb_cast == c.jsonb_cast && s->cols[i].cls == sift_v6_col_class(&c))
		{
			s->cols[i].key_only &= key;
			s->cols[i].nan_ok &= takes_nan || s->cols[i].cls == SV6C_DATUM;
			return &s->cols[i];
		}
	col_init(s, leaf, &c, key, takes_nan);
	n = &s->cols[s->ncols - 1];
	if (n->cls == SV6C_DATUM)
	{
		n->nan_ok = true;
		n->def = &s->defs[leaf];
		get_typlenbyval(c.typid, &n->typlen, &n->typbyval);
		lane_cols(s, leaf);
	}
	return n;
}

/* a column of the relation the scan reads: not one a dimension of a star join gives, nor the element of an unnested array */
static bool
reads_fact(const SiftV6Col *c)
{
	return c->src != SV6SRC_DIM && c->src != SV6SRC_ELEM;
}

/* the slot of a column of the pipe: what the scan derived from columns, a payload column of a dimension, or the column of the fact side that col_for finds */
int
sift_f2_v6_slot_of(V6Scan *s, const int *first, const SiftV6Col *c, bool takes_nan)
{
	if (c->derive > 0)
		return s->ncols + c->derive - 1;
	return c->src == SV6SRC_DIM ? sift_v6_pipe_dim_slot(s->pipe, c->dim, c->slot) : (int) (col_for(s, first, *c, false, takes_nan) - s->cols);
}

/* whether a star join reads the column of the fact side: a key of a dimension or a column of a condition of the join */
static bool
join_reads(const SiftV6Ext *x, int attno)
{
	for (int d = 0; x != NULL && d < x->ndims; d++)
		for (int k = 0; k < x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&x->dim[d], k) && x->dim[d].fsrc[k] == SV6SRC_ATTR && sift_v6_xdim_key_col(&x->dim[d], k).attno == attno)
				return true;
	for (int i = 0; x != NULL && i < x->nrcol; i++)
		if (x->rcol[i].src == SV6SRC_ATTR && x->rcol[i].attno == attno)
			return true;
	return false;
}

/*
 * Whether the column is read by nothing but a COUNT(DISTINCT) of it (and so can be counted by the codes of the dictionaries of the parts): not a key, a key of a join, another aggregate's argument,
 * a FILTER or a row of several columns
 */
static bool
read_by_code_only(const SiftV6Spec *spec, int attno)
{
	bool		distinct = false;

	if (sift_v6_spec_pg_reads(spec) || join_reads(spec->ext, attno))
		return false;
	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].attno == attno)
			return false;
	for (int j = 0; j < spec->naggs; j++)
	{
		const SiftV6Agg *a = &spec->agg[j];

		for (int c = 0; c < a->nxcols; c++)
			if (a->xcol[c].attno == attno)
				return false;
		if ((a->kind == SV6_ARG_MAX || a->kind == SV6_ARG_MIN || v6g_is_gen(a)) && a->arg2.attno == attno)
			return false;
		if (sift_v6_kind_reads_column(a->kind) && a->arg.attno == attno)
		{
			if (a->kind != SV6_COUNT_DISTINCT || a->nxcols > 0)
				return false;
			distinct = true;
		}
	}
	for (int i = 0; i < spec->nfcols; i++)
		if (spec->fcol[i].attno == attno)
			return false;
	return distinct;
}

/* the slot of a lane after the head of a datum: the head's vector carries the value, so the join gathers nothing for it */
static SiftVec no_vec;

static void
setup_pipe(V6Scan *s, const int *first, SiftV6StarRun *star)
{
	const SiftV6Spec *spec = s->spec;
	const SiftV6Ext *x = spec->ext;
	const int	nbin = s->ncols + s->nderived;
	const int	nfact = nbin + s->nbins + (s->rows != NULL);
	SiftVecType *type = palloc(nfact * sizeof(SiftVecType));

	for (int i = 0; i < s->ncols; i++)
		type[i] = s->cols[i].vec.type;
	for (int k = 0; k < s->nderived; k++)
		type[s->ncols + k] = sift_v6_derive_vec_type(spec, s->derived[k].d);
	for (int b = 0; b < s->nbins; b++)
		type[nbin + b] = SVEC_I64;
	if (s->rows != NULL)
	{
		type[nfact - 1] = SVEC_I64;
		s->tidvec.data = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(uint64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	}
	s->pipe = sift_v6_pipe_new(CurrentMemoryContext, spec, s->group, s->rows, nfact, type);
	sift_v6_star_attach(star, s->pipe);
	for (int d = 0; d < x->ndims; d++)
		for (int k = 0; k < x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&x->dim[d], k))
			{
				const SiftV6Col key = sift_v6_xdim_key_col(&x->dim[d], k);

			s->pipe->fslot[d][k] = sift_f2_v6_slot_of(s, first, &key, false);
			}
	for (int i = 0; i < x->nrcol; i++)
		s->pipe->rslot[i] = sift_f2_v6_slot_of(s, first, &x->rcol[i], false);
	for (int i = 0; i < spec->nkeys; i++)
		s->pipe->key_slot[i] = sift_f2_v6_slot_of(s, first, &spec->key[i], spec->rows);
	for (int b = 0; b < s->nbins; b++)
		s->pipe->key_slot[s->bins[b].key] = nbin + b;
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		s->pipe->arg_slot[v] = sift_v6_vec_col(spec, v) == NULL ? -1 : sift_f2_v6_slot_of(s, first, sift_v6_vec_col(spec, v), sift_v6_vec_takes_nan(spec, v));
	s->fact = palloc(nfact * sizeof(SiftVec *));
	for (int i = 0; i < s->ncols; i++)
		s->fact[i] = s->cols[i].lane ? &no_vec : &s->cols[i].vec;
	for (int k = 0; k < s->nderived; k++)
		s->fact[s->ncols + k] = &s->derived[k].out.vec;
	for (int b = 0; b < s->nbins; b++)
		s->fact[nbin + b] = &s->bins[b].vec;
	if (s->rows != NULL)
		s->fact[nfact - 1] = &s->tidvec;
}

/* the heap addresses of the n rows of the batch, as the slot of the pipe that the pairs of a join carry to the heap of the best rows */
static void
fill_tids(V6Scan *s, const uint64 *tid, int n)
{
	memcpy(s->tidvec.data, tid, n * sizeof(uint64));
	s->tidvec.len = (uint16) n;
	s->tidvec.all_valid = true;
	s->tidvec.dense = true;
	s->tidvec.type = SVEC_I64;
}

/* The keys and arguments the spec computes from columns: their sources are read as values, their vectors replace the ones of the columns */
static void
setup_derived(V6Scan *s, const int *first)
{
	const SiftV6Spec *spec = s->spec;

	s->derived = palloc0(Max(spec->nderive, 1) * sizeof(V6Derived));
	for (int k = 0; k < spec->nderive; k++)
	{
		V6Derived  *dv = &s->derived[s->nderived++];

		dv->d = &spec->derive[k];
		for (int j = 0; j < k && dv->prefix == NULL; j++)
			if (sift_vn_prefix_of(&spec->derive[j], dv->d))
				dv->prefix = &s->derived[j];
		if (!sift_v6_pgexpr_sourceless(spec, dv->d))
			dv->src = &col_for(s, first, dv->d->src, false, dv->d->kind == SVD_PG_EXPR || sift_vn_carries_nan(dv->d))->vec;
		for (int i = 0; i < dv->d->ncond; i++)
			dv->cond[i] = &col_for(s, first, dv->d->cond[i], false, sift_vn_carries_nan(dv->d))->vec;
		sift_vx_out_init(&dv->out, CurrentMemoryContext);
	}
	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].derive > 0)
			s->keyvec[i] = &s->derived[spec->key[i].derive - 1].out.vec;
	for (int j = 0; j < spec->naggs; j++)
		if (spec->agg[j].arg.derive > 0)
			s->argvec[j] = &s->derived[spec->agg[j].arg.derive - 1].out.vec;
}

typedef struct BqCols
{
	V6Scan	   *s;
	const int  *first;
} BqCols;

static const SiftVec *
input_col(void *arg, const SiftV6InCol *c, bool takes_nan)
{
	const BqCols *b = arg;

	return &col_for(b->s, b->first, (SiftV6Col) {.attno = c->attno,.typid = c->typid,.scale = c->scale}, false, takes_nan)->vec;
}

static const SiftVec *
bq_col(void *arg, const SiftV6InCol *c)
{
	return input_col(arg, c, false);
}

/* the inputs of an expression PostgreSQL evaluates: a NaN is the value it is for PostgreSQL (sift_v6_vec_datum) */
static const SiftVec *
pg_col(void *arg, const SiftV6InCol *c)
{
	return input_col(arg, c, true);
}

static bool
reads_raising(Node *n, void *context)
{
	return n != NULL && ((IsA(n, Var) && sift_v6_vatt_raises(((const Var *) n)->varattno)) || expression_tree_walker(n, reads_raising, context));
}

/* the quals no scan key says: their input is the relation's, their columns are decoded with the others */
static void
setup_bq(V6Scan *s, const int *first)
{
	BqCols		cols = {s, first};
	SiftV6Input in;
	List	   *uncast = NIL;

	if (s->spec->vqual == NIL)
		return;
	if (s->is_table)
		sift_v6_input_init_table(&in, s->host);
	else
		sift_v6_input_init_rel(&in, s->host);
	s->bq = sift_v6_bq_open(CurrentMemoryContext, s->spec->vqual, &in, bq_col, &cols, s->spec->exec);
	foreach_ptr(Node, q, s->spec->vqual)
		if (!reads_raising((Node *) q, NULL))
			uncast = lappend(uncast, q);
	if (uncast != NIL && list_length(uncast) < list_length(s->spec->vqual))
		s->bq_uncast = sift_v6_bq_open(CurrentMemoryContext, uncast, &in, bq_col, &cols, s->spec->exec);
}

/* the keys, arguments, derivations, quals and bins of the spec over the columns of the relation */
static void
setup_spec(V6Scan *s, const int *first)
{
	const SiftV6Spec *spec = s->spec;
	const SiftV6Ext *x = spec->ext;
	const int	nvec = sift_v6_spec_nvec(spec);

	for (int i = 0; i < spec->nkeys; i++)
		if (reads_fact(&spec->key[i]))
			s->keyvec[i] = &col_for(s, first, spec->key[i], x == NULL && !spec->rows, spec->rows)->vec;
	for (int v = 0; v < nvec; v++)
		if (sift_v6_vec_col(spec, v) != NULL && reads_fact(sift_v6_vec_col(spec, v)) && !sourceless(s, sift_v6_vec_col(spec, v)))
			s->argvec[v] = &col_for(s, first, *sift_v6_vec_col(spec, v), false, sift_v6_vec_takes_nan(spec, v))->vec;
	for (int d = 0; x != NULL && d < x->ndims; d++)
		for (int k = 0; k < x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&x->dim[d], k) && x->dim[d].fsrc[k] == SV6SRC_ATTR)
				col_for(s, first, sift_v6_xdim_key_col(&x->dim[d], k), false, false);
	for (int i = 0; x != NULL && i < x->nrcol; i++)
		if (x->rcol[i].src == SV6SRC_ATTR)
			col_for(s, first, x->rcol[i], false, false);
	setup_derived(s, first);
	for (int i = 0; i < spec->nfcols; i++)
		if (spec->fcol[i].derive > 0)
			s->argvec[sift_v6_spec_fvec(spec) + i] = &s->derived[spec->fcol[i].derive - 1].out.vec;
	setup_bq(s, first);
	for (int k = 0; k < s->nderived; k++)
		if (s->derived[k].d->kind == SVD_PG_EXPR)
			s->derived[k].pg = sift_v6_pgexpr_bind(CurrentMemoryContext, spec, s->derived[k].d, pg_col, &(BqCols) {s, first}, s->derived[k].pgin);
	for (int i = 0; i < s->ncols; i++)
		s->cols[i].by_code = s->cols[i].cls == SV6C_TEXT && !s->cols[i].json && read_by_code_only(spec, s->cols[i].attno);
	s->bins = palloc0(Max(spec->nbins, 1) * sizeof(V6Bin));
	for (int b = 0; b < spec->nbins; b++)
	{
		V6Bin	   *bn = &s->bins[s->nbins++];

		bn->key = spec->bin[b].key;
		bn->month = spec->bin[b].stride_us == SIFT_DATEBIN_MONTH;
		bn->bin = (SiftDateBin) {.stride_us = spec->bin[b].stride_us,.origin = spec->bin[b].origin,.stride_status = spec->bin[b].status};
		bn->src = s->keyvec[bn->key];
		bn->out = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		s->keyvec[bn->key] = &bn->vec;
	}
}

static void
pair_col(void *arg, const SiftV6Col *c, bool month)
{
	const BqCols *b = arg;

	col_for(b->s, b->first, *c, false, false);
}

void
sift_f2_v6_scan_setup(V6Scan *s, const SiftColDef *defs, const int *first, SiftV6StarRun *star)
{
	const SiftV6Spec *spec = s->spec;
	const SiftV6Ext *x = spec->ext;
	const int	nvec = sift_v6_spec_nvec(spec);

	s->defs = defs;
	s->first = first;
	s->cols = palloc0((spec->nkeys + nvec + spec->nderive * (1 + SIFT_V6_MAX_CASE) + sift_v6_spec_pg_inputs(spec) + (x != NULL ? x->ndims * SIFT_STAR_MAX_KEYS + x->nrcol : 0) + (spec->vqual != NIL ? SIFT_V6_MAX_COLS : 0)) * SIFT_COL_MAX_LANES * sizeof(V6Col));
	s->keyvec = palloc(Max(spec->nkeys, 1) * sizeof(SiftVec *));
	s->argvec = palloc0(Max(nvec, 1) * sizeof(SiftVec *));
	if (x != NULL && x->unnest)
		sift_v6_unnest_attr_cols(spec, pair_col, &(BqCols) {s, first});
	else
		setup_spec(s, first);
	for (int i = 0; i < s->ncols; i++)
		if ((s->cols[i].json || s->cols[i].cls == SV6C_DATUM) && s->batch_cx == NULL)
			/* MEM[MEM-AGG-05]: SiftV6Scan batch_cx (the json exceptions of a batch, made only when a column reads a JSON key; was made for every scan) */
			s->batch_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 json exceptions");
	for (int i = 0; i < s->ncols; i++)
	{
		V6Col	   *c = &s->cols[i];

		c->vec.type = c->cls == SV6C_F64 ? SVEC_F64 : c->cls == SV6C_TEXT ? SVEC_STRVIEW : c->cls == SV6C_DATUM ? SVEC_DATUM : SVEC_I64;
		c->vec.typid = c->cls == SV6C_DATUM ? c->typid : InvalidOid;
		c->vec.typlen = c->typlen;
		c->vec.typbyval = c->typbyval;
	}
	if (x != NULL && x->ndims > 0)
		setup_pipe(s, first, star);
}

static void
derive_rows(void *arg, int k, const SiftBatchSel *bs)
{
	V6Scan	   *s = arg;
	const V6Derived *dv = &s->derived[k];

	if (dv->d->kind == SVD_PG_EXPR)
		sift_v6_pgexpr_derive(dv->pg, dv->pgin, bs, &s->derived[k].out);
	else if (dv->d->kind == SVD_NUM_EXPR)
		sift_vn_derive(dv->d, dv->cond, dv->prefix ? &dv->prefix->out.vec : NULL, dv->prefix ? dv->prefix->d->ncond : 0, bs, &s->derived[k].out);
	else
		sift_vx_derive(dv->d, dv->src, dv->cond, bs, &s->derived[k].out);
}

void
sift_f2_v6_derive_columns(V6Scan *s)
{
	const bool	to_group = s->emit == NULL && s->pipe == NULL;

	sift_v6_derive_batch(to_group ? s->group : NULL, s->nderived, &s->bs, s->argvec, derive_rows, s);
	if (s->spec->affine)
		sift_vx_affine_check(s->spec, s->keyvec[0], &s->bs);
}

/* The keys that are date_bin() of a column, for the rows of the batch: the error of PostgreSQL for a row that overflows, raised only for a row the scan keeps */
void
sift_f2_v6_bin_keys(V6Scan *s)
{
	for (int i = 0; i < s->nbins; i++)
	{
		V6Bin	   *b = &s->bins[i];
		uint32		err_row;
		SiftDateBinStatus st = SDB_OK;

		if (b->month)
			sift_datemonth_batch(&s->bs, b->src, b->out);
		else
			st = sift_datebin_batch(&b->bin, &s->bs, b->src, b->out, &err_row);
		if (st != SDB_OK)
			sift_datebin_raise(st);
		b->vec = *b->src;
		b->vec.data = b->out;
	}
}

uint16
sift_f2_v6_vet_slice(V6Scan *s, const uint64 *live, const uint64 *tid, bool visible, int off, int n)
{
	SiftBatchSel *bs = &s->bs;
	const int	words = (n + 63) / 64;

	memset(bs->mask, 0, sizeof(bs->mask));
	memcpy(bs->mask, live + off / 64, words * sizeof(uint64));
	if (n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (n & 63)) - 1;
	if (s->spec->tid_range && sift_v6_tid_mask(&s->vet, s->spec->tid_lo, s->spec->tid_hi, tid, off, n, bs) == 0)
		return 0;
	return sift_v6_vet_mask(&s->vet, tid, off, visible, n, bs);
}

bool
sift_f2_v6_batch_visible(const V6Scan *s, const SiftF2Batch *b)
{
	return s->vet.heap == NULL || s->all_visible || (b->part != NULL && b->part == s->visible_part);
}

/* The rows the stored values could not decide are the heap's to decide: they join the batch when the recheck keeps them */
static uint16
vet_maybe(V6Scan *s, const uint64 *maybe, const uint64 *tid, int off, int n)
{
	SiftBatchSel *bs = &s->bs;

	for (int w = 0; w < (n + 63) / 64; w++)
		for (uint64 bits = maybe[off / 64 + w] & (n - w * 64 >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (n - w * 64)) - 1); bits != 0; bits &= bits - 1)
		{
			const int	r = w * 64 + __builtin_ctzll(bits);

			if (s->recheck(s->recheck_arg, tid[off + r]))
			{
				bs->mask[w] |= UINT64CONST(1) << (r & 63);
				bs->n_active++;
			}
		}
	bs->all_active = bs->n_active == n;
	return bs->n_active;
}

static bool
part_offer(void *arg, const SiftPartDesc *d, bool countable, bool *tids_unneeded)
{
	V6Scan	   *s = arg;

	s->visible_part = sift_v6_vis_part_settled(&s->vet, d) || sift_v6_vis_part_all_visible(&s->vis, s->vet.heap, d) ? d : NULL;
	if (s->visible_part == NULL || s->spec->tid_range)
		return false;
	if (countable && s->count_only)
	{
		sift_v6_group_add_rows(s->group, d->nrows);
		s->stats->rows += d->nrows;
		return true;
	}
	if (s->stat != NULL && sift_v6_colstat_offer(s->stat, d, countable))
	{
		s->stats->rows += d->nrows;
		return true;
	}
	*tids_unneeded = s->rows == NULL;
	return false;
}

/* The least and the greatest of the part, as the two rows of a batch of the columns the aggregates read */
static bool
zone_offer(void *arg, const SiftPartDesc *d, const SiftF2PartZone *zone)
{
	V6Scan	   *s = arg;
	SiftBatchSel *bs = &s->bs;

	if (!s->minmax_only || s->visible_part != d || s->spec->tid_range)
		return false;
	for (int i = 0; i < s->ncols; i++)
	{
		SiftVec    *v = &s->cols[i].vec;

		((int64 *) v->data)[0] = zone[i].min;
		((int64 *) v->data)[1] = zone[i].max;
		v->type = SVEC_I64;
		v->valid = NULL;
		v->nan = NULL;
		v->all_valid = true;
		v->len = 2;
		v->dense = true;
		v->sel = NULL;
	}
	memset(bs->mask, 0, sizeof(bs->mask));
	bs->mask[0] = 3;
	bs->n_raw = 2;
	bs->n_active = 2;
	bs->all_active = true;
	bs->has_sel = false;
	sift_v6_group_update(s->group, bs, s->keyvec, s->argvec);
	s->stats->rows += d->nrows;
	return true;
}

/* The columns of the n rows from off of a batch that vet_slice kept, folded into the groups, the pipe or the caller's emit */
static void
fold_slice(V6Scan *s, const ColView *views, const float4 *score, const uint64 *tid, uint64 part_gen, int off, int n)
{
	sift_f2_v6_raise_kept(s, views, sift_f2_v6_fill_cols(s, views, score, part_gen, off, n), off, n);
	if (s->bq != NULL)
	{
		sift_v6_bq_filter(s->bq, &s->bs);
		if (s->bs.n_active == 0)
			return;
	}
	sift_f2_v6_refuse_nans(s);
	if (s->unnest != NULL)
	{
		s->stats->rows += s->bs.n_active;
		sift_v6_unnest_rows_run(s->unnest, s->fact, &s->bs, tid + off);
		return;
	}
	sift_f2_v6_derive_columns(s);
	sift_f2_v6_bin_keys(s);
	s->stats->rows += s->bs.n_active;
	if (s->emit != NULL)
		s->emit(s->emit_arg, &s->bs, s->keyvec, s->argvec);
	else if (s->pipe != NULL)
	{
		if (s->rows != NULL)
			fill_tids(s, tid + off, n);
		sift_v6_pipe_run(s->pipe, s->fact, &s->bs);
	}
	else
	{
		if (s->rows != NULL)
			sift_v6_rows_update(s->rows, &s->bs, s->keyvec, tid + off);
		if (s->group != NULL)
			sift_v6_group_update(s->group, &s->bs, s->keyvec, s->argvec);
	}
}

void
sift_f2_v6_sink(void *arg, const SiftF2Batch *b)
{
	V6Scan	   *s = arg;
	const bool	visible = sift_f2_v6_batch_visible(s, b);

	if (s->stat != NULL && visible)
		sift_v6_colstat_note(s->stat, b);
	for (int off = 0; off < b->n; off += SIFT_VEC_LEN)
	{
		const int	n = Min(SIFT_VEC_LEN, b->n - off);

		s->stats->batches++;
		if (sift_f2_v6_vet_slice(s, b->live, b->tid, visible, off, n) == 0)
			continue;
		fold_slice(s, b->views, NULL, b->tid, b->part != NULL ? b->part->part_gen : 0, off, n);
	}
}

void
sift_f2_v6_flush_grans(V6Scan *s)
{
	s->stats->grans += s->grans.scanned;
	s->stats->grans_pruned_zone += s->grans.pruned_zone;
	s->stats->grans_pruned_skip += s->grans.pruned_skip;
	s->grans = (SiftF2GranCounts) {0};
}

int *
sift_f2_v6_scan_leaves(const V6Scan *s)
{
	int		   *leaves = palloc(Max(s->ncols, 1) * sizeof(int));

	for (int i = 0; i < s->ncols; i++)
		leaves[i] = s->cols[i].leaf;
	return leaves;
}

bool *
sift_f2_v6_scan_nan_leaves(const V6Scan *s, int nleaves)
{
	bool	   *nan_ok = palloc0(Max(nleaves, 1) * sizeof(bool));

	for (int i = 0; i < s->ncols; i++)
		nan_ok[s->cols[i].leaf] = s->cols[i].nan_ok;
	return nan_ok;
}

static bool *
dict_leaves(const V6Scan *s)
{
	bool	   *dict_leaf = palloc(Max(s->ncols, 1) * sizeof(bool));

	for (int i = 0; i < s->ncols; i++)
		dict_leaf[i] = (s->cols[i].key_only || s->cols[i].by_code) && s->cols[i].cls == SV6C_TEXT && !s->cols[i].json;
	return dict_leaf;
}

/* A part of a statement that is nothing but the COUNT(DISTINCT) of columns of text, and that the scan would read whole: the strings of its dictionaries are the strings of its rows, and no row is read */
static bool
part_dict_offer(void *arg, const SiftPartDesc *d, const SiftGranuleDict *dicts)
{
	V6Scan	   *s = arg;
	SiftDictSrc src[SIFT_V6_MAX_KEYS + 16];

	if (s->visible_part == NULL || s->spec->tid_range || s->visible_part->part_gen != d->part_gen || s->group == NULL || s->emit != NULL || s->pipe != NULL || s->rows != NULL || s->gate != NULL ||
		s->nbins > 0 || s->nderived > 0 || s->ncols > lengthof(src) || !v6g_dist_dict_only(s->group))
		return false;
	for (int i = 0; i < s->ncols; i++)
	{
		if (!s->cols[i].by_code || dicts[i].off == NULL)
			return false;
		src[i] = (SiftDictSrc) {.gen = sift_v6_dict_gen(s->host, d->part_gen),.n = dicts[i].n,.off = dicts[i].off,.bytes = dicts[i].bytes};
		if (!v6g_dist_dict_fits(&src[i]))
			return false;
	}
	for (int i = 0; i < s->ncols; i++)
		v6g_dist_dict_part(s->group, s->cols[i].attno, &src[i]);
	s->stats->rows += d->nrows;
	return true;
}

/* The place among the scan's columns of the first sort column of a row Top-N, -1 when the sort key is computed from a column (a date bin) and no column of the scan holds its values */
static int
rows_bound_leaf(const V6Scan *s)
{
	const SiftVec *key = s->keyvec[s->spec->topk_item[0].pos];

	for (int i = 0; i < s->ncols; i++)
		if (&s->cols[i].vec == key)
			return i;
	return -1;
}

/* Whether the rows of a part reach the threads as the scan reaches its own consumer: the part is all-visible, so that no row of it needs a look at the heap */
static bool
par_part_ok(void *arg, const SiftPartDesc *d)
{
	const V6Scan *s = arg;

	return s->visible_part != NULL && s->visible_part->part_gen == d->part_gen;
}

/* The statements the threads of agg/par/pinrun.h take, with the columns of the scan's batch as they decode them */
static struct SiftV6PinRun *
pin_begin(V6Scan *s, Relation index, Snapshot snap)
{
	SiftV6PinCol *cols = palloc(Max(s->ncols, 1) * sizeof(SiftV6PinCol));

	for (int i = 0; i < s->ncols; i++)
		cols[i] = (SiftV6PinCol) {.leaf = s->cols[i].leaf,.plain_int = (s->cols[i].cls == SV6C_I32 || s->cols[i].cls == SV6C_I64) && s->cols[i].scale == 0 && !s->cols[i].json &&
			s->cols[i].attno > 0 && s->cols[i].typid != NUMERICOID};
	return sift_v6_pin_begin(s->spec, s->group, s->stats, sift_col_rel(index)->first, index, snap, par_part_ok, s, s->pipe, cols, s->ncols);
}

/* One pass over the index (over the rows of the bridge, when it is one) */
static void
scan_pass(V6Scan *s, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip, const SiftTidBridge *bridge)
{
	IndexScanDesc scan = sift_index_beginscan(heap, index, snap, nkeys);
	int			nzones;
	const SiftColPrune *zones = sift_f2_v6_zone_tests(s->spec, index, &nzones, s->stats);

	index_rescan(scan, keys, nkeys, NULL, 0);
	sift_f2_scan_skip_parts(scan, skip, nskip);
	sift_f2_scan_set_scope_router(scan, s->router);
	if (bridge != NULL)
		sift_f2_scan_set_bridge(scan, bridge, NULL, NULL);
	sift_f2_scan_set_part_sink(scan, part_offer);
	sift_f2_scan_set_part_dict_sink(scan, part_dict_offer);
	sift_f2_scan_set_zone_sink(scan, zone_offer);
	if (s->rows != NULL && s->group == NULL && sift_v6_rows_bound(s->rows) != NULL)
	{
		const int	leaf = rows_bound_leaf(s);

		if (leaf >= 0)
			sift_f2_scan_set_bound(scan, leaf, sift_v6_rows_bound(s->rows));
	}
	if (s->preagg != NULL)
		sift_f2_scan_set_gran_sink(scan, sift_v6_preagg_try_granule, s->preagg);
	sift_f2_scan_set_dict_leaves(scan, dict_leaves(s));
	if (s->par != NULL)
		sift_f2_scan_set_pin_win(scan, sift_v6_par_window(s->par));
	if (s->pin != NULL)
		sift_f2_scan_set_pin_win(scan, sift_v6_pinrun_window(s->pin));
	sift_f2_scan_count_granules(scan, &s->grans);
	sift_f2_scan_set_zones(scan, zones, nzones);
	sift_f2_scan_sink(scan, sift_f2_v6_sink, s, sift_f2_v6_scan_leaves(s), s->ncols);
	sift_f2_v6_flush_grans(s);
	if (s->par != NULL)
		sift_v6_par_end(s->par);
	if (s->pin != NULL)
		sift_v6_pinrun_end(s->pin);
	s->par = NULL;
	s->pin = NULL;
	index_endscan(scan);
}

static void
scan_run(V6Scan *s, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip, SiftV6StarRun *star, SiftV6ChunkFn next_chunk,
		 void *chunk_arg)
{
	const SiftTidBridge *bridge = NULL;

	s->count_only = s->rows == NULL && s->spec->ext == NULL && s->spec->vqual == NIL && sift_v6_spec_count_only(s->spec);
	s->host = index;
	sift_f2_v6_scan_setup(s, sift_col_rel(index)->schema->cols, sift_col_rel(index)->first, star);
	s->minmax_only = s->rows == NULL && s->pipe == NULL && sift_v6_spec_minmax_only(s->spec);
	sift_v6_vet_begin(&s->vet, heap, snap, s->stats);
	sift_v6_vis_ask_index(&s->vet, index);
	sift_v6_vis_init(&s->vis, CurrentMemoryContext);
	if (s->rows == NULL && s->pipe == NULL && s->emit == NULL && nkeys == 0 && !s->spec->tid_range)
		s->stat = sift_v6_colstat_begin(s->spec, s->group, index, sift_col_rel(index)->first, CurrentMemoryContext);
	if (s->emit == NULL && s->rows == NULL && s->pipe == NULL && s->gate == NULL && next_chunk == NULL && s->group != NULL && !s->spec->tid_range)
		s->preagg = sift_v6_preagg_begin(s->spec, s->group, sift_col_rel(index)->first, &s->vet, s->stats);
	if (next_chunk == NULL)
	{
		if (s->pipe == NULL && s->emit == NULL && s->gate == NULL && !s->spec->tid_range && (s->group != NULL) != (s->rows != NULL))
			s->par = sift_v6_par_begin(s->spec, s->group, s->rows, s->stats, sift_col_rel(index)->first, index, snap, par_part_ok, s);
		if (s->par == NULL && s->rows == NULL && s->emit == NULL && s->gate == NULL && s->group != NULL && !s->spec->tid_range)
			s->pin = pin_begin(s, index, snap);
		scan_pass(s, heap, index, keys, nkeys, snap, skip, nskip, NULL);
	}
	while (next_chunk != NULL && next_chunk(chunk_arg, &bridge))
	{
		s->stats->chunks++;
		s->stats->bridge_tids += bridge->ntids;
		scan_pass(s, heap, index, keys, nkeys, snap, skip, nskip, bridge);
	}
	if (s->pipe != NULL)
		sift_v6_star_scan_end(star, s->pipe);
	if (s->stat != NULL)
		sift_v6_colstat_end(s->stat);
	sift_v6_vis_free(&s->vis);
	sift_v6_vet_end(&s->vet);
	if (s->preagg != NULL)
		sift_v6_preagg_end(s->preagg);
	if (s->batch_cx != NULL)
		MemoryContextDelete(s->batch_cx);
}

void
sift_v6_scan_run_ext(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip,
					 const SiftScopeRouter *router, SiftV6ScanStats *stats, SiftV6StarRun *star, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	V6Scan		s = {.spec = spec,.group = g,.stats = stats,.router = router};

	scan_run(&s, heap, index, keys, nkeys, snap, skip, nskip, star, next_chunk, chunk_arg);
}

SiftV6Fold *
sift_v6_fold_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, SiftV6Recheck recheck, void *recheck_arg)
{
	V6Scan	   *s = sift_palloc0_aligned(V6Scan, 1);

	s->spec = spec;
	s->group = g;
	s->stats = stats;
	s->recheck = recheck;
	s->recheck_arg = recheck_arg;
	s->host = index != NULL ? index : heap;
	s->is_table = index == NULL;
	sift_f2_v6_scan_setup(s, index != NULL ? sift_col_rel(index)->schema->cols : sift_f2_store_open(heap)->schema->cols,
			   index != NULL ? sift_col_rel(index)->first : sift_f2_store_open(heap)->att_first, NULL);
	sift_v6_vet_begin(&s->vet, heap, snap, stats);
	s->pub = palloc(Max(s->ncols, 1) * sizeof(SiftV6FoldCol));
	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].attno != SIFT_V6_SCORE_ATTNO)
			s->pub[s->nstore++] = (SiftV6FoldCol) {s->cols[i].attno, s->cols[i].leaf, s->cols[i].cls, s->cols[i].scale};
	return s;
}

const SiftV6FoldCol *
sift_v6_fold_cols(const SiftV6Fold *s, int *n)
{
	*n = s->nstore;
	return s->pub;
}

void
sift_v6_fold_batch_scored(SiftV6Fold *s, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, const float4 *score, int total)
{
	for (int off = 0; off < total; off += SIFT_VEC_LEN)
	{
		const int	n = Min(SIFT_VEC_LEN, total - off);

		s->stats->batches++;
		sift_f2_v6_vet_slice(s, live, tid, false, off, n);
		if (maybe != NULL)
			vet_maybe(s, maybe, tid, off, n);
		if (s->bs.n_active > 0)
			fold_slice(s, views, score, tid, 0, off, n);
	}
}

void
sift_v6_fold_batch(SiftV6Fold *s, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, int total)
{
	sift_v6_fold_batch_scored(s, tid, live, maybe, views, NULL, total);
}

void
sift_v6_fold_end(SiftV6Fold *s)
{
	sift_v6_vet_end(&s->vet);
	if (s->batch_cx != NULL)
		MemoryContextDelete(s->batch_cx);
}

/* A scan of a parallel run: begun by the leader to list the units of the scan, and by each participant to read the runs of granules it takes; its batches go to emit */
SiftV6Scan *
sift_v6_scan_begin(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, const SiftStoreQuals *quals, Snapshot snap, SiftV6ScanStats *stats,
				   SiftV6BatchFn emit, void *emit_arg)
{
	V6Scan	   *s = sift_palloc0_aligned(V6Scan, 1);
	const SiftColPrune *zones;
	int			nzones;

	s->spec = spec;
	s->stats = stats;
	s->emit = emit;
	s->emit_arg = emit_arg;
	if (index == NULL)
	{
		sift_f2_v6_scan_table_morsels(s, heap, quals, snap);
		return s;
	}
	s->host = index;
	sift_f2_v6_scan_setup(s, sift_col_rel(index)->schema->cols, sift_col_rel(index)->first, NULL);
	s->leaves = sift_f2_v6_scan_leaves(s);
	sift_v6_vet_begin(&s->vet, heap, snap, stats);
	s->scan = sift_index_beginscan(heap, index, snap, nkeys);
	index_rescan(s->scan, keys, nkeys, NULL, 0);
	sift_f2_scan_set_dict_leaves(s->scan, dict_leaves(s));
	sift_f2_scan_count_granules(s->scan, &s->grans);
	zones = sift_f2_v6_zone_tests(spec, index, &nzones, stats);
	sift_f2_scan_set_zones(s->scan, zones, nzones);
	s->preagg = emit != NULL && !spec->tid_range && !sift_v6_spec_distinct(spec) ? sift_v6_preagg_begin(spec, NULL, sift_col_rel(index)->first, &s->vet, stats) : NULL;
	if (s->preagg != NULL)
		sift_f2_scan_set_gran_sink(s->scan, sift_v6_preagg_try_granule, s->preagg);
	return s;
}

void
sift_v6_scan_plan(SiftV6Scan *s, SiftF2ParPlan *out)
{
	if (s->scan != NULL)
		sift_f2_par_plan_build(s->scan, out);
	else if (s->unsatisfiable)
		*out = (SiftF2ParPlan) {.refs = palloc(sizeof(SiftParPartRef))};
	else
		sift_f2_store_par_plan(s->store_scan, out);
}

void
sift_v6_scan_morsel_begin(SiftV6Scan *s, const uint64 *staged_dead, uint32 nstaged)
{
	if (s->scan != NULL)
		sift_f2_scan_morsel_begin(s->scan, sift_f2_v6_sink, s, s->leaves, s->ncols, staged_dead, nstaged);
	else
		sift_f2_store_scan_morsel_begin(s->store_scan, staged_dead, nstaged);
}

void
sift_v6_scan_morsel(SiftV6Scan *s, SiftV6Group *into, const SiftParPartRef *ref, uint32 first, uint32 n)
{
	if (s->scan == NULL)
	{
		sift_f2_store_scan_morsel(s->store_scan, ref, first, n, sift_f2_v6_sink, s);
		return;
	}
	if (s->preagg != NULL)
		sift_v6_preagg_set_group(s->preagg, into);
	s->all_visible = (ref->flags & SIFT_PAR_PART_ALL_VISIBLE) != 0;
	sift_f2_scan_morsel(s->scan, ref, first, n);
	sift_f2_v6_flush_grans(s);
	s->all_visible = false;
}

void
sift_v6_scan_end(SiftV6Scan *s)
{
	if (s->scan == NULL)
		sift_f2_store_scan_close(s->store_scan);
	else
	{
		if (s->preagg != NULL)
			sift_v6_preagg_end(s->preagg);
		index_endscan(s->scan);
		sift_v6_vet_end(&s->vet);
	}
	if (s->batch_cx != NULL)
		MemoryContextDelete(s->batch_cx);
	pfree(s->leaves);
	pfree(s);
}

void
sift_v6_scan_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip,
				 const SiftScopeRouter *router, SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	sift_v6_scan_run_ext(spec, g, heap, index, keys, nkeys, snap, skip, nskip, router, stats, NULL, next_chunk, chunk_arg);
}

void
sift_v6_scan_rows(const SiftV6Spec *spec, SiftV6Rows *rows, SiftV6Group *totals, SiftV6StarRun *star, Relation heap, Relation index, ScanKey keys, int nkeys,
				  const SiftStoreQuals *quals, Snapshot snap, SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	V6Scan		s = {.spec = spec,.rows = rows,.group = totals,.stats = stats};

	if (index == NULL)
		sift_f2_v6_scan_table_run(&s, heap, quals, snap, NULL, 0, star, next_chunk, chunk_arg);
	else
		scan_run(&s, heap, index, keys, nkeys, snap, NULL, 0, star, next_chunk, chunk_arg);
}
