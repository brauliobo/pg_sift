/*
 * agg/v6/par/tseries_run.c - the time-bucket aggregation on the compute threads (agg/v6/par/tseries_run.h): the shape of the statement, what a thread does with a granule of the window, and the
 * groups the leader folds into the statement's.
 */
#include "postgres.h"

#include "agg/v6/par/tseries.h"
#include "agg/v6/par/tseries_run.h"

#define SCRATCH_BYTES	(8u << 20)

typedef struct TsRun
{
	const SiftV6Spec *spec;
	SiftV6Group *g;
	SiftTsShape sh;
	int			nleaves;
	int			leaf[SIFT_TS_LEAVES_MAX];
} TsRun;

/* ---- the shape of the statement ---- */

static bool
plain_col(const SiftV6Col *c)
{
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->scale == 0 && c->attno > 0;
}

static int
add_leaf(TsRun *t, const int *first, const SiftV6Col *c)
{
	const int	leaf = first != NULL ? first[c->attno] : 0;

	for (int i = 0; i < t->nleaves; i++)
		if (t->leaf[i] == leaf)
			return i;
	t->leaf[t->nleaves] = leaf;
	return t->nleaves++;
}

static bool
keys_of(const SiftV6Spec *sp, const int *first, TsRun *t)
{
	SiftTsShape *sh = &t->sh;

	if (sp->nkeys < 1 || sp->nkeys > SIFT_PAR_GROUP_MAX_KEYS)
		return false;
	sh->nk = sp->nkeys;
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Col *c = &sp->key[i];
		const SiftV6Class cls = sift_v6_class(c->typid);

		if (!plain_col(c) || !(cls == SV6C_I32 || cls == SV6C_I64))
			return false;
		sh->key[i].at = add_leaf(t, first, c);
	}
	for (int b = 0; b < sp->nbins; b++)
	{
		SiftTsKey  *k = &sh->key[sp->bin[b].key];

		k->binned = true;
		k->month = sp->bin[b].stride_us == SIFT_DATEBIN_MONTH;
		k->bin = (SiftDateBin) {.stride_us = sp->bin[b].stride_us,.origin = sp->bin[b].origin,.stride_status = SDB_OK};
	}
	return true;
}

static bool
aggs_of(const SiftV6Spec *sp, const int *first, TsRun *t)
{
	SiftTsShape *sh = &t->sh;

	for (int j = 0; j < sp->naggs; j++)
	{
		const SiftV6Agg *a = &sp->agg[j];

		if (a->fclause != 0 || a->nxcols != 0)
			return false;
		if (a->kind == SV6_COUNT_STAR)
			continue;
		if (!plain_col(&a->arg))
			return false;
		if (a->kind == SV6_COUNT)
		{
			if (sh->ncount == SIFT_PAR_GROUP_MAX_SUM)
				return false;
			sh->count_at[sh->ncount++] = add_leaf(t, first, &a->arg);
			continue;
		}
		if ((a->kind != SV6_SUM && a->kind != SV6_AVG) || (a->arg.typid != INT2OID && a->arg.typid != INT4OID) || sh->nsum == SIFT_PAR_GROUP_MAX_SUM)
			return false;
		sh->sum_at[sh->nsum++] = add_leaf(t, first, &a->arg);
	}
	return true;
}

static bool
shape_of(const SiftV6Spec *sp, const int *first, TsRun *t)
{
	*t = (TsRun) {.spec = sp};
	if (sp->is_table || sp->nparts != 0 || sp->ext != NULL || sp->rows || sp->nsets != 0 || sp->nbins < 1 || sp->nderive != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 ||
		sp->affine || sp->jsonqual != NIL || OidIsValid(sp->text_indexoid) || sp->naggs < 1 || sp->topk != 0)
		return false;
	if (!keys_of(sp, first, t) || !aggs_of(sp, first, t))
		return false;
	t->sh.ncols = t->nleaves;
	t->sh.gs = (SiftParGroupSpec) {.nk = t->sh.nk,.nsum = t->sh.nsum};
	return true;
}

bool
sift_v6_tseries_wanted(const SiftV6Spec *spec)
{
	TsRun		t;

	return shape_of(spec, NULL, &t);
}

/* ---- a thread ---- */

static bool
ts_fill(void *arg, int worker, const SiftPinSlot *sl, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	const TsRun *t = arg;
	ColView		cv[SIFT_TS_LEAVES_MAX];
	SiftParCol	col[SIFT_TS_LEAVES_MAX];
	uint64	   *active;
	uint32		nact;

	if (!sift_v6_pin_slot_cols(sl, scratch, err, t->leaf, t->nleaves, NULL, cv, &active, &nact))
		return false;
	if (nact == 0)
		return true;
	for (int i = 0; i < t->nleaves; i++)
		col[i] = (SiftParCol) {.v = cv[i].i,.isnull = cv[i].isnull};
	return sift_tseries_batch(&t->sh, col, active, nact, sl->nrows, scratch, err, out);
}

/* ---- the leader ---- */

static void
ts_fold(void *arg, const SiftParGroupRow *row)
{
	const TsRun *t = arg;
	int64		key[SIFT_V6_MAX_KEYS] = {0};
	uint64		nulls = 0;

	for (int i = 0; i < t->sh.nk; i++)
		nulls |= (uint64) !sift_tseries_key_value(row->key[i], &key[i]) << i;
	sift_v6_pin_fold_row(t->g, t->spec, row, key, NULL, nulls, NULL);
}

SiftV6PinRun *
sift_v6_tseries_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg)
{
	TsRun	   *t = palloc(sizeof(TsRun));
	SiftV6PinDesc d;

	if (!shape_of(spec, first, t))
	{
		pfree(t);
		return NULL;
	}
	t->g = g;
	d = (SiftV6PinDesc) {.name = "tseries",.gs = t->sh.gs,.leaf = t->leaf,.nleaves = t->nleaves,.scratch_bytes = SCRATCH_BYTES,.fill = ts_fill,.fold = ts_fold,.arg = t};
	return sift_v6_pinrun_new(&d, spec, stats, index, snap, part_ok, part_arg);
}
