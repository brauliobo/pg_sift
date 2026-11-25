/*
 * col/f2/agg/scan_arms.c - the arms of a cohort of statements over one pass of an index (f2_v6_scan.c): each batch is given to every arm that takes its rows, and folded into the groups of the arm.
 */
/* check-memory: allow-allocs every array here follows the arms of one cohort, not the rows of the table */
#include "postgres.h"

#include "access/genam.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/fuse/fuse.h"
#include "agg/vet.h"
#include "col/f2/sink/sink.h"
#include "col/f2/agg/scan_int.h"

typedef struct V6Arms
{
	V6Scan	   *s;				/* [narms], the leader first */
	SiftV6FuseCohort *cohort;
	const SiftF2Batch *b;		/* the slice being given: its rows and the offset of the slice in the granule */
	int			off;
	int			n;
	uint16		n_live;			/* the rows of the slice that are visible and live */
	uint64		live[SIFT_VEC_WORDS];
} V6Arms;

static void
arms_select(const V6Arms *a, V6Scan *s, const uint64 *mask, uint16 n_active)
{
	memcpy(s->bs.mask, mask, SIFT_VEC_WORDS * sizeof(uint64));
	s->bs.n_raw = a->n;
	s->bs.n_active = n_active;
	s->bs.all_active = n_active == a->n;
	s->bs.has_sel = false;
}

static void
arms_fill(const V6Arms *a, V6Scan *s, const uint64 *mask, uint16 n_active)
{
	arms_select(a, s, mask, n_active);
	sift_f2_v6_fill_cols(s, a->b->views, NULL, a->b->part != NULL ? a->b->part->part_gen : 0, a->off, a->n);
}

/* the rows of the pass the arm takes: all of them when the arms have the keys of the leader, else those its gate allows */
static uint16
arms_mask(void *ctx, int arm, const uint64 *live, uint64 *out)
{
	V6Arms	   *a = ctx;
	V6Scan	   *s = &a->s[arm];
	uint64		allowed[SIFT_VEC_WORDS];
	uint16		n = 0;

	if (s->gate == NULL)
	{
		memcpy(out, live, SIFT_VEC_WORDS * sizeof(uint64));
		return a->n_live;
	}
	arms_fill(a, s, live, a->n_live);
	sift_clause_eval(s->gate, s->argvec + sift_v6_spec_fvec(s->spec), a->n, allowed);
	for (int w = 0; w < SIFT_VEC_WORDS; w++)
		n += pg_popcount64(out[w] = live[w] & allowed[w]);
	return n;
}

static void
arms_consume(void *ctx, int arm, const uint64 *mask, uint16 n_active)
{
	V6Arms	   *a = ctx;
	V6Scan	   *s = &a->s[arm];

	if (s->gate == NULL)
		arms_fill(a, s, mask, n_active);
	else
		arms_select(a, s, mask, n_active);
	sift_f2_v6_refuse_nans(s);
	sift_f2_v6_derive_columns(s);
	sift_f2_v6_bin_keys(s);
	sift_v6_group_update(s->group, &s->bs, s->keyvec, s->argvec);
	s->stats->rows += s->bs.n_active;
}

static void
arms_sink(void *arg, const SiftF2Batch *b)
{
	static const SiftV6FuseOps ops = {.arm_mask = arms_mask,.consume = arms_consume};
	V6Arms	   *a = arg;
	V6Scan	   *lead = &a->s[0];

	for (int off = 0; off < b->n; off += SIFT_VEC_LEN)
	{
		const int	n = Min(SIFT_VEC_LEN, b->n - off);

		lead->stats->batches++;
		a->b = b;
		a->off = off;
		a->n = n;
		a->n_live = sift_f2_v6_vet_slice(lead, b->live, b->tid, sift_f2_v6_batch_visible(lead, b), off, n);
		memcpy(a->live, lead->bs.mask, sizeof(a->live));
		sift_v6_fuse_dispatch(a->cohort, &ops, a, a->live, a->n_live);
	}
}

void
sift_v6_scan_run_arms(SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap)
{
	V6Arms		a = {.cohort = cohort};
	IndexScanDesc scan;
	int		   *leaves;
	int			nleaves = 0;

	a.s = sift_palloc0_aligned(V6Scan, cohort->narms);
	for (int i = 0; i < cohort->narms; i++)
	{
		a.s[i] = (V6Scan) {.spec = arms[i].spec,.group = arms[i].group,.stats = arms[i].stats,.gate = arms[i].gate};
		sift_f2_v6_scan_setup(&a.s[i], sift_col_rel(index)->schema->cols, sift_col_rel(index)->first, NULL);
		nleaves += a.s[i].ncols;
	}
	leaves = palloc(Max(nleaves, 1) * sizeof(int));
	nleaves = 0;
	for (int i = 0; i < cohort->narms; i++)
		for (int c = 0; c < a.s[i].ncols; c++)
			leaves[nleaves++] = a.s[i].cols[c].leaf;
	sift_v6_vet_begin(&a.s[0].vet, heap, snap, a.s[0].stats);
	scan = sift_index_beginscan(heap, index, snap, nkeys);
	index_rescan(scan, keys, nkeys, NULL, 0);
	sift_f2_scan_sink(scan, arms_sink, &a, leaves, nleaves);
	index_endscan(scan);
	sift_v6_vet_end(&a.s[0].vet);
	for (int i = 0; i < cohort->narms; i++)
		if (a.s[i].batch_cx != NULL)
			MemoryContextDelete(a.s[i].batch_cx);
}
