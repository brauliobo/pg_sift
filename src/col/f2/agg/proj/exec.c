/*
 * col/f2/agg/proj/exec.c - the groups of a SiftAggV6 node from the projections of its relation. A SIFT_PROJ_AGG projection that is ready, has a definition that still parses and can answer the
 * statement (f2_v6_proj.h) stands for some of the base parts under the snapshot (a link of the part as it is now, or of a part whose dead rows can be taken away; the heap shows every address of
 * a sift_col part to every snapshot) and costs less than the scan. The cells of those parts are rolled up into the groups of the query and the rows the deletion vectors have since killed are taken
 * away; every row of the rest of the relation (the parts it does not stand for, the slabs, the partitions with deletes pending) comes from the host's scan and is added. The aggregates are then
 * finished as the groups of the scan finish them (agg/group/group.c), so the answer is the scan's, row for row.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/numeric.h"

#include "col/f2/agg/colstat.h"
#include "col/f2/agg/proj/ctx.h"
#include "col/f2/agg/proj/exec.h"
#include "col/f2/agg/proj/fold.h"
#include "col/f2/agg/proj/key.h"
#include "col/am.h"
#include "core/errctx.h"
#include "miscadmin.h"

/* The aggregate as a measure of a projection, with its FILTER: false for what a projection cannot answer, and never with a FILTER left out */
static bool
measure_of(const SiftV6ProjHost *h, const SiftV6Spec *spec, const SiftV6Agg *a, SiftProjMeasure *m)
{
	static const struct
	{
		SiftV6Kind	kind;
		SiftProjMeasureFn fn;
	}			map[] = {{SV6_COUNT_STAR, SPM_COUNT_STAR}, {SV6_COUNT, SPM_COUNT}, {SV6_SUM, SPM_SUM}, {SV6_AVG, SPM_AVG}, {SV6_MIN, SPM_MIN}, {SV6_MAX, SPM_MAX},
	{SV6_COUNT_DISTINCT, SPM_COUNT_DISTINCT}};

	if (sift_v6_class(a->arg.typid) == SV6C_TEXT)
		return false;
	for (int i = 0; i < lengthof(map); i++)
		if (map[i].kind == a->kind && a->nxcols == 0)
		{
			*m = (SiftProjMeasure) {.fn = map[i].fn,.leaf = a->kind == SV6_COUNT_STAR ? -1 : h->store->att_first[a->arg.attno]};
			return a->fclause == 0 || sift_v6_proj_preds_of_quals(h->store, h->rel, list_nth_node(List, spec->filter, a->fclause - 1), m->filter, SIFT_PROJ_FILTER_MAX, &m->nfilter, CurrentMemoryContext);
		}
	return false;
}

/* The query as the selector sees it; false for what a projection cannot answer (sets, a count(DISTINCT) of several columns, no group, more columns or predicates than a projection has) */
static bool
query_of(const SiftV6ProjHost *h, const SiftV6Spec *spec, SiftV6ProjQuery *q)
{
	*q = (SiftV6ProjQuery) {.limit = -1};
	if (h->heap != NULL && h->npreds == 0 && sift_v6_spec_whole_parts(spec))
		return false;
	if (spec->nderive > 0 || spec->vqual != NIL || spec->affine || spec->nsets > 0 || spec->nfuncs > 0 || spec->nkeys > SV6P_MAX || spec->naggs > SV6P_MAX || h->npreds > SV6P_MAX)
		return false;
	for (int i = 0; i < spec->nkeys; i++)
		q->group[q->ngroup++] = (SiftProjKey) {.leaf = h->store->att_first[spec->key[i].attno],.fn = SPF_COLUMN};
	for (int j = 0; j < spec->naggs; j++)
		if (!measure_of(h, spec, &spec->agg[j], &q->agg[q->nagg++]))
			return false;
	for (int k = 0; k < h->npreds; k++)
		q->pred[q->npred++] = sift_v6_proj_query_pred(&h->preds[k]);
	return true;
}

typedef struct FoldIO
{
	SiftProjFold *fold;
	SiftCell   *buf;
} FoldIO;

static void
stored_row(void *arg, const SiftRowBuf *rows, int r)
{
	FoldIO	   *io = arg;

	for (int i = 0; i < io->fold->nlay; i++)
		io->buf[i] = sift_rowbuf_cell(rows, i, r);
	sift_proj_fold_stored(io->fold, io->buf);
}

static void
dead_row(void *arg, const SiftRowBuf *rows, int r)
{
	FoldIO	   *io = arg;

	for (int c = 0; c < io->fold->ncols; c++)
		io->buf[c] = sift_rowbuf_cell(rows, c, r);
	sift_proj_fold_dead(io->fold, io->buf);
}

static void
live_row(void *arg, uint64 tid, const SiftCell *row)
{
	sift_proj_fold_live(arg, row);
}

static Datum
int_datum(Oid typid, int64 x)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) x);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) x);
		case BOOLOID:
			return BoolGetDatum(x != 0);
		default:
			return Int64GetDatum(x);
	}
}

static Datum
key_datum(const SiftV6Col *k, const SiftCell *c, bool *isnull)
{
	*isnull = c->isnull;
	if (c->isnull)
		return (Datum) 0;
	if (sift_v6_class(k->typid) == SV6C_TEXT)
		return PointerGetDatum(cstring_to_text_with_len(c->s, c->slen));
	return int_datum(k->typid, c->i);
}

static Datum
agg_datum(const SiftV6Agg *a, const SiftProjGroup *g, const SiftProjMState *st, bool *isnull)
{
	SiftI128	sum = {.lo = (uint64) st->sum,.hi = (int64) (st->sum >> 64)};

	*isnull = false;
	switch (a->kind)
	{
		case SV6_COUNT_STAR:
			return Int64GetDatum(a->fclause != 0 ? st->cnt : g->rows);
		case SV6_COUNT:
		case SV6_COUNT_DISTINCT:
			return Int64GetDatum(st->cnt);
		case SV6_SUM:
		case SV6_AVG:
			*isnull = st->cnt == 0;
			if (st->cnt == 0)
				return (Datum) 0;
			if (a->kind == SV6_AVG)
				return DirectFunctionCall2(numeric_div, sift_v6_scaled_numeric(sum, a->arg.scale), NumericGetDatum(int64_to_numeric(st->cnt)));
			if (a->restype == INT8OID)
			{
				if ((sum.hi == 0 && (int64) sum.lo >= 0) || (sum.hi == -1 && (int64) sum.lo < 0))
					return Int64GetDatum((int64) sum.lo);
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			}
			return sift_v6_scaled_numeric(sum, a->arg.scale);
		default:
			*isnull = !st->has;
			if (!st->has)
				return (Datum) 0;
			if (a->arg.typid == FLOAT4OID)
				return Float4GetDatum((float4) st->mf);
			if (a->arg.typid == FLOAT8OID)
				return Float8GetDatum(st->mf);
			if (a->arg.typid == NUMERICOID)
				return sift_v6_scaled_numeric((SiftI128) {.lo = (uint64) st->mi,.hi = st->mi < 0 ? -1 : 0}, a->arg.scale);
			return int_datum(a->arg.typid, st->mi);
	}
}

static void
fill_result(const SiftV6Spec *spec, SiftProjCube *cube, MemoryContext cx, SiftProjResult *out)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	const int	ngroups = sift_proj_cube_ngroups(cube);
	const int	n = Max(ngroups, spec->nkeys == 0);
	SiftProjGroup empty = {.st = palloc0(Max(spec->naggs, 1) * sizeof(SiftProjMState))};

	out->natts = spec->nkeys + spec->naggs;
	out->nrows = n;
	out->values = palloc(Max(n * out->natts, 1) * sizeof(Datum));
	out->isnull = palloc(Max(n * out->natts, 1) * sizeof(bool));
	for (int i = 0; i < n; i++)
	{
		const SiftProjGroup *g = i < ngroups ? sift_proj_cube_group(cube, i) : &empty;
		Datum	   *v = out->values + (size_t) i * out->natts;
		bool	   *nl = out->isnull + (size_t) i * out->natts;

		for (int k = 0; k < spec->nkeys; k++)
			v[k] = key_datum(&spec->key[k], &g->key[k], &nl[k]);
		for (int j = 0; j < spec->naggs; j++)
			v[spec->nkeys + j] = agg_datum(&spec->agg[j], g, &g->st[j], &nl[spec->nkeys + j]);
	}
	MemoryContextSwitchTo(old);
}

/* The leaves a scan of the rest has to decode: the groups, the measures and the predicates of the statement */
static int
rest_leaves(const SiftV6ProjQuery *q, int *leaves)
{
	int			all[3 * SV6P_MAX + SV6P_MAX * SIFT_PROJ_FILTER_MAX],
				na = 0,
				n = 0;

	for (int i = 0; i < q->ngroup; i++)
		all[na++] = q->group[i].leaf;
	for (int j = 0; j < q->nagg; j++)
		if (q->agg[j].leaf >= 0)
			all[na++] = q->agg[j].leaf;
	for (int k = 0; k < q->npred; k++)
		all[na++] = q->pred[k].leaf;
	for (int j = 0; j < q->nagg; j++)
		for (int f = 0; f < q->agg[j].nfilter && na < lengthof(all); f++)
			all[na++] = q->agg[j].filter[f].leaf;
	for (int i = 0; i < na; i++)
	{
		bool		seen = false;

		for (int j = 0; j < n; j++)
			seen |= leaves[j] == all[i];
		if (!seen)
			leaves[n++] = all[i];
	}
	return n;
}

/* The cube of the statement: the cells of every part the projection stands for, the dead rows of those taken away, the rows of the rest added; false when it cannot be exact */
static bool
fold_sources(const SiftV6ProjCtx *c, const SiftV6ProjSource *src, const SiftProjRollup *map, const SiftV6ProjQuery *q, const SiftProjSpec *qspec, SiftProjFold **out, SiftProjResult *res)
{
	const SiftV6ProjHost *h = c->h;
	SiftProjPartSpec *pps = palloc(sizeof(SiftProjPartSpec));
	SiftProjFold *f;
	FoldIO		io;
	int			leaves[3 * SV6P_MAX + SV6P_MAX * SIFT_PROJ_FILTER_MAX],
				nleaves = rest_leaves(q, leaves),
				nskip;
	uint64	   *skip = sift_v6_proj_served_gens(c, src, &nskip);

	sift_proj_part_spec(h->store, &src->def->spec, &c->cols, pps);
	f = sift_proj_fold_new(qspec, &src->def->spec, pps->lay, pps->nlay, map, h->preds, h->npreds, h->store->ncols, &c->cols, (int64) work_mem * 1024 / 128);
	io = (FoldIO) {f, palloc(Max(Max(pps->nlay, h->store->ncols), 1) * sizeof(SiftCell))};
	*out = f;
	for (int i = 0; i < c->base.n; i++)
	{
		if (src->serve[i].serve == SPS_BASE)
			continue;
		sift_proj_scan_part(h->rel, sift_v6_proj_part_of(src, src->serve[i].proj_gen), SIFT_PROJ_PART_ID, &pps->spec, false, stored_row, &io);
		res->nparts++;
		if (src->serve[i].serve == SPS_OVERLAY)
		{
			sift_v6_host_overlay_rows(h, &c->base, i, sift_proj_link_of(&src->parts, c->base.part[i].part_gen), dead_row, &io);
			res->nsubtracted++;
		}
	}
	res->nbase = c->base.n - nskip;
	if (!sift_v6_host_scan_rest(h, c->plan, skip, nskip, leaves, nleaves, live_row, f))
		return false;
	res->nrest_rows = f->nlive;
	return sift_proj_fold_finish(f);
}

bool
sift_v6_proj_serve_agg(const SiftV6ProjHost *h, const SiftV6Spec *spec, MemoryContext cx, SiftProjResult *out)
{
	SiftV6ProjQuery q;
	SiftV6ProjCtx c;
	SiftV6ProjChoice ch;
	SiftProjSpec qspec = {.kind = SIFT_PROJ_AGG};
	SiftProjFold *fold;
	bool		ok;

	*out = (SiftProjResult) {0};
	if (!query_of(h, spec, &q) || !sift_v6_proj_ctx_open(&c, h, SIFT_PROJ_AGG))
		return false;
	ch = sift_v6_proj_ctx_choose(&c, &q);
	if (ch.cand < 0)
		return false;
	qspec.nkeys = q.ngroup;
	memcpy(qspec.key, q.group, q.ngroup * sizeof(SiftProjKey));
	qspec.nmeas = q.nagg;
	memcpy(qspec.meas, q.agg, q.nagg * sizeof(SiftProjMeasure));
	ok = fold_sources(&c, &c.src[ch.cand], &ch.rollup, &q, &qspec, &fold, out);
	if (ok)
	{
		fill_result(spec, fold->cube, cx, out);
		out->used = true;
		strlcpy(out->name, c.src[ch.cand].ref->name, sizeof(out->name));
	}
	sift_proj_fold_free(fold);
	return ok;
}

static bool
serve_keys(SiftV6ProjHost *h, const SiftV6Spec *spec, MemoryContext cx, SiftProjResult *out)
{
	SiftProjPred *preds;

	*out = (SiftProjResult) {0};
	if (!sift_proj_relation_has(h->rel))
		return false;
	h->store = sift_f2_store_open(h->rel);
	if (!sift_v6_proj_preds_of_keys(h->store, RelationGetDescr(h->rel), h->keys, h->nkeys, &preds, cx))
		return false;
	h->preds = preds;
	h->npreds = h->nkeys;
	return sift_v6_proj_serve_agg(h, spec, cx, out);
}

bool
sift_v6_proj_serve(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, MemoryContext cx, SiftProjResult *out)
{
	SiftV6ProjHost h = {.rel = index,.heap = heap,.snap = snap,.keys = keys,.nkeys = nkeys};

	return serve_keys(&h, spec, cx, out);
}

bool
sift_v6_proj_serve_table(const SiftV6Spec *spec, Relation rel, ScanKey keys, int nkeys, Snapshot snap, MemoryContext cx, SiftProjResult *out)
{
	SiftV6ProjHost h = {.rel = rel,.snap = snap,.keys = keys,.nkeys = nkeys};

	return serve_keys(&h, spec, cx, out);
}

bool
sift_v6_proj_next(SiftProjResult *r, Datum *values, bool *isnull)
{
	if (r->next >= r->nrows)
		return false;
	memcpy(values, r->values + (size_t) r->next * r->natts, r->natts * sizeof(Datum));
	memcpy(isnull, r->isnull + (size_t) r->next * r->natts, r->natts * sizeof(bool));
	r->next++;
	return true;
}
