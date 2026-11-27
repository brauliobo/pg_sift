/*
 * col/f2/agg/unnest/rows.c - the rows of a batch of the aggregate scan of an unnest that reach its elements: those in the TID range whose heap row the snapshot sees, then those the quals of the
 * table no scan key says keep, and the batch quals that decide them (col/f2/agg/unnest/int.h).
 */
#include "postgres.h"

#include "core/popcount.h"
#include "col/f2/agg/unnest/int.h"

/* the rows of the batch in the TID range of the statement whose heap row the snapshot sees */
uint16
sift_f2_unnest_vet_rows(UnRun *u, const SiftF2ExtBatch *b)
{
	const SiftV6Spec *s = u->spec;

	if (s->tid_range && sift_v6_tid_mask(&u->vet, s->tid_lo, s->tid_hi, b->tid, 0, b->n, &u->bs) == 0)
		return 0;
	return sift_v6_vet_mask(&u->vet, b->tid, 0, false, b->n, &u->bs);
}

static uint16
count_active(SiftBatchSel *bs)
{
	uint16		active = 0;

	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		active += sift_pc64(bs->mask[w]);
	bs->n_active = active;
	bs->all_active = active == bs->n_raw;
	bs->has_sel = false;
	return active;
}

/* the rows of the batch the quals no scan key says keep, decided on the columns as the statement reads them; a row whose array the index cannot hold is the heap's, which tests it */
uint16
sift_f2_unnest_restrict_quals(UnRun *u, const SiftF2ExtBatch *b)
{
	SiftBatchSel *bs = &u->bs;
	uint64		heap[SIFT_VEC_WORDS];

	for (int w = 0; w < (b->n + 63) / 64; w++)
	{
		heap[w] = bs->mask[w] & b->elem->poison[w];
		bs->mask[w] &= ~heap[w];
	}
	for (int i = 0; i < u->nbq; i++)
		u->bqv[i] = u->fv[u->bqcol[i]];
	if (count_active(bs) > 0)
		sift_v6_bq_filter(u->bq, bs);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		bs->mask[w] |= heap[w];
	return count_active(bs);
}

typedef struct BqCols
{
	UnRun	   *u;
	const SiftColRel *r;
} BqCols;

static const SiftVec *
bq_col(void *arg, const SiftV6InCol *c)
{
	BqCols	   *b = arg;
	UnRun	   *u = b->u;
	const SiftV6Col col = sift_v6_vatt_is_json(c->attno) ? sift_v6_spec_vatt_col(u->spec, c->attno, c->typid, c->scale) : (SiftV6Col) {.attno = c->attno,.typid = c->typid,.scale = c->scale};

	u->bqcol[u->nbq] = sift_f2_unnest_col_index(u, b->r, &col, false);
	return &u->bqv[u->nbq++];
}

/* the quals of the table no scan key says, over the columns of the batch the pull decodes */
void
sift_f2_unnest_setup_bq(UnRun *u, const SiftColRel *r)
{
	BqCols		cols = {u, r};
	SiftV6Input in;

	if (u->spec->vqual == NIL)
		return;
	sift_v6_input_init_rel(&in, u->index);
	u->bq = sift_v6_bq_open(CurrentMemoryContext, u->spec->vqual, &in, bq_col, &cols, u->spec->exec);
}
