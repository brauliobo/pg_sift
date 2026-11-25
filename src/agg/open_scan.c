/*
 * agg/open_scan.c - the groups of a statement from the batches of SiftStoreOps.open (agg/open_scan.h). The columns the keys, aggregates and batch quals read are opened as the wide
 * vectors the operators fold (an integer, a date or a timestamp as int64, a float as double, a text as a view, a numeric as its scaled integer); the batch quals run over them as they do for
 * the scan of a zone-pruned format, and the rows left go through the pipe of the groups.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the relation */
#include "postgres.h"

#include "core/popcount.h"

#include "agg/qual/bqual.h"
#include "agg/open_scan.h"
#include "agg/pipe.h"

typedef struct OpenScan
{
	const SiftV6Spec *spec;
	SiftV6Input in;
	SiftVec		vec[SIFT_V6_MAX_COLS];
	SiftVec    *fact[SIFT_V6_MAX_COLS];
} OpenScan;

static bool
class_ok(const SiftV6Col *c)
{
	const SiftV6Class cls = sift_v6_class(c->typid);

	return c->attno > 0 && c->src == SV6SRC_ATTR && c->jleaf == 0 && c->derive == 0 && c->typid != NUMERICOID && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64 || cls == SV6C_TEXT);
}

static bool
agg_ok(const SiftV6Agg *a)
{
	if (a->kind == SV6_GEN_PG || a->kind == SV6_OSA || a->kind == SV6_PCT_CONT || a->fclause != 0)
		return false;
	return (!sift_v6_kind_reads_column(a->kind) || class_ok(&a->arg)) && (a->arg2.attno == 0 || class_ok(&a->arg2)) && a->nxcols == 0;
}

bool
sift_v6_open_scan_ok(const SiftV6Spec *s)
{
	if (s->rows || s->nparts != 0 || s->ext != NULL || s->indexqual != NIL || s->jsonqual != NIL || OidIsValid(s->text_indexoid) || s->par_strategy != 0 || s->nderive != 0 || s->nbins != 0 ||
		s->nfilters != 0 || s->nfcols != 0 || s->affine || s->genref != NIL)
		return false;
	for (int i = 0; i < s->nkeys; i++)
		if (!class_ok(&s->key[i]))
			return false;
	for (int j = 0; j < s->naggs; j++)
		if (!agg_ok(&s->agg[j]))
			return false;
	return true;
}

static SiftV6InCol
wide_col(const SiftV6InCol *c)
{
	SiftV6InCol w = *c;

	switch (sift_v6_class(c->typid))
	{
		case SV6C_F64:
			w.type = SVEC_F64;
			break;
		case SV6C_TEXT:
			w.type = SVEC_STRVIEW;
			break;
		default:
			w.type = c->typid == NUMERICOID ? SVEC_NUM64 : SVEC_I64;
	}
	return w;
}

/* the slot of the column of the input that holds attribute attno, added when it is not one yet */
static int
slot_of(OpenScan *os, const SiftV6InCol *c)
{
	for (int k = 0; k < os->in.ncols; k++)
		if (os->in.col[k].attno == c->attno)
			return k;
	SIFT_INVARIANT(os->in.ncols < SIFT_V6_MAX_COLS, "the aggregate scan of relation %u reads more columns than a batch holds", os->in.indexoid);
	os->in.col[os->in.ncols] = wide_col(c);
	return os->in.ncols++;
}

static int
col_slot(OpenScan *os, const SiftV6Col *c)
{
	return slot_of(os, &(SiftV6InCol) {.attno = c->attno,.typid = c->typid,.scale = c->scale});
}

static const SiftVec *
bq_col(void *arg, const SiftV6InCol *c)
{
	OpenScan   *os = arg;

	return &os->vec[slot_of(os, c)];
}

/* the slots of the columns the keys and aggregates read, so that the batch quals add theirs after them */
static void
spec_slots(OpenScan *os)
{
	for (int i = 0; i < os->spec->nkeys; i++)
		col_slot(os, &os->spec->key[i]);
	for (int v = 0; v < sift_v6_spec_nvec(os->spec); v++)
		if (sift_v6_vec_col(os->spec, v) != NULL)
			col_slot(os, sift_v6_vec_col(os->spec, v));
}

static SiftV6Pipe *
pipe_new(OpenScan *os, SiftV6Group *group)
{
	const SiftV6Spec *spec = os->spec;
	SiftVecType type[SIFT_V6_MAX_COLS];
	SiftV6Pipe *pipe;

	for (int i = 0; i < os->in.ncols; i++)
		type[i] = os->in.col[i].type;
	pipe = sift_v6_pipe_new(CurrentMemoryContext, spec, group, NULL, os->in.ncols, type);
	for (int i = 0; i < spec->nkeys; i++)
		pipe->key_slot[i] = col_slot(os, &spec->key[i]);
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		pipe->arg_slot[v] = sift_v6_vec_col(spec, v) == NULL ? -1 : col_slot(os, sift_v6_vec_col(spec, v));
	return pipe;
}

static void
sel_of(SiftBatchSel *bs, const SiftV6Batch *b)
{
	const int	nw = (b->n + 63) / 64;
	uint16		n = 0;

	memset(bs->mask, 0, sizeof(bs->mask));
	for (int w = 0; w < nw; w++)
	{
		bs->mask[w] = b->mask != NULL ? b->mask[w] : ~UINT64CONST(0);
		if (w == nw - 1 && (b->n & 63) != 0)
			bs->mask[w] &= (UINT64CONST(1) << (b->n & 63)) - 1;
		n += (uint16) sift_pc64(bs->mask[w]);
	}
	bs->n_raw = b->n;
	bs->n_active = n;
	bs->all_active = n == b->n;
	bs->has_sel = false;
}

void
sift_v6_open_scan_run(const SiftV6Spec *spec, SiftV6Group *group, const SiftStoreOps *ops, EState *estate, Relation heap, Relation index, SiftV6ScanStats *stats)
{
	OpenScan   *os = palloc0(sizeof(OpenScan));
	SiftV6Bq   *bq = NULL;
	SiftV6Pipe *pipe;
	SiftV6Source *src;
	SiftV6Batch b;
	SiftBatchSel bs;

	os->spec = spec;
	if (index != NULL)
		sift_v6_input_init_rel(&os->in, index);
	else
		sift_v6_input_init_table(&os->in, heap);
	sift_v6_input_bind(&os->in, estate);
	spec_slots(os);
	if (spec->vqual != NIL)
	{
		SiftV6Input probe = os->in;

		probe.ncols = 0;
		bq = sift_v6_bq_open(CurrentMemoryContext, spec->vqual, &probe, bq_col, os, spec->exec);
	}
	pipe = pipe_new(os, group);
	for (int i = 0; i < os->in.ncols; i++)
		os->fact[i] = &os->vec[i];
	src = ops->open(estate, &os->in, NULL, false);
	while (src->next(src, &b))
	{
		for (int i = 0; i < os->in.ncols; i++)
			os->vec[i] = b.col[i];
		sel_of(&bs, &b);
		if (bq != NULL)
			sift_v6_bq_filter(bq, &bs);
		stats->batches++;
		stats->rows += bs.n_active;
		sift_v6_pipe_run(pipe, os->fact, &bs);
	}
	src->close(src);
}
