/*
 * agg/par/run_rows.c - ORDER BY .. LIMIT K of the rows of one table on the compute threads (agg/par/run.h): each thread keeps the K best rows of its granules (agg/par/rows.h) and the
 * leader keeps the K best of all, with the heap addresses the node fetches them by. A row is named for the threads by its granule in the window and its place in it; the leader reads the addresses of
 * the granules of the winners only. The leader leaves out the granules whose zone of the first sort column cannot hold a row better than the K-th best the threads have by then.
 */
#include "postgres.h"

#include "utils/lsyscache.h"

#include "agg/par/run_int.h"
#include "agg/topn/rows.h"
#include "core/compat.h"

#define ROW_BITS	16

static bool
sort_key(const SiftV6Spec *sp, int i, ParShape *sh, const int *first)
{
	const SiftV6Col *c = &sp->key[i];
	const SiftV6TopKItem *it = &sp->topk_item[i];
	const SiftV6Class cls = sift_v6_class(c->typid);
	Oid			opfamily,
				opcintype;
	SiftCmpStrategy cmptype;

	sh->rs.str[i] = cls == SV6C_TEXT;
	if (!par_plain_col(c) || it->pos != i || !(cls == SV6C_I32 || cls == SV6C_I64 || (sh->rs.str[i] && sift_collate_is_c(it->collid))))
		return false;
	get_ordering_op_properties(it->sortop, &opfamily, &opcintype, &cmptype);
	sh->rs.desc[i] = cmptype == SIFT_CMP_GT;
	sh->rs.nulls_first[i] = it->nulls_first;
	sh->rkey_at[i] = par_add_leaf(sh, par_leaf_of(first, c), false);
	return true;
}

static bool
rows_shape(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (!sp->rows || sp->naggs != 0 || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 || sp->affine ||
		sp->nkeys < 1 || sp->nkeys > SIFT_PAR_ROWS_MAX_KEYS || sp->ntopk != sp->nkeys || sp->topk < 1 || sp->topk > SIFT_PAR_ROWS_K_MAX)
		return false;
	for (int i = 0; i < sp->nkeys; i++)
		if (!sort_key(sp, i, sh, first))
			return false;
	sh->rs.nkeys = sp->nkeys;
	sh->rs.k = sp->topk;
	return true;
}

static void
rows_sizes(const ParShape *sh, uint32 groups_est, uint64 rows, int nthreads, ParSizes *sz)
{
	sz->scratch = 8u << 20;
	sz->work = 8u << 20;
	sz->state_min = sz->state_want = 1u << 20;
}

#define ROWS_SERIAL_HEAD	2

static void
rows_setup(SiftV6ParScan *p)
{
	sift_pin_win_serial_head(p->win, ROWS_SERIAL_HEAD);
	if (!p->sh.rs.str[0])
		sift_pin_win_track_zone(p->win, p->sh.leaf[p->sh.rkey_at[0]]);
}

static bool
rows_skip(SiftV6ParScan *p, uint32 job)
{
	int64		min,
				max;
	bool		has_null;

	return p->op != NULL && sift_pin_win_job_zone(p->win, job, &min, &max, &has_null) && sift_par_rows_excludes(p->op, min, max, has_null);
}

static bool
rows_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParRowsBatch *out, SiftErr *err)
{
	SiftV6ParScan *p = arg;
	const ParShape *sh = &p->sh;
	const SiftPinSlot *sl = sift_pin_win_slot(p->win, lo);
	const int	nk = sh->rs.nkeys;
	ParLoad		ld;
	uint64	   *tid;
	int64	   *key[SIFT_PAR_ROWS_MAX_KEYS] = {0};
	bool	   *isnull[SIFT_PAR_ROWS_MAX_KEYS] = {0};
	SiftStrRef *str[SIFT_PAR_ROWS_MAX_KEYS] = {0};

	if (!par_load(sh, sl, scratch, err, &ld))
		return false;
	if (ld.nact == 0)
		return true;
	if ((tid = sift_arena_alloc(scratch, ld.nact * sizeof(uint64), 8)) == NULL)
		return par_arena_full(scratch, err);
	for (int j = 0; j < nk; j++)
	{
		if (sh->rs.str[j])
			str[j] = sift_arena_alloc(scratch, ld.nact * sizeof(SiftStrRef), 8);
		else
		{
			key[j] = sift_arena_alloc(scratch, ld.nact * sizeof(int64), 8);
			isnull[j] = sift_arena_alloc(scratch, ld.nact, 8);
		}
		if (str[j] == NULL && key[j] == NULL)
			return par_arena_full(scratch, err);
	}
	PAR_EACH_ROW(&ld, r, i)
	{
		tid[i] = ((uint64) sift_pin_slot_job(sl) << ROW_BITS) | r;
		for (int j = 0; j < nk; j++)
		{
			const ColView *cv = &ld.cv[sh->rkey_at[j]];

			if (sh->rs.str[j])
				str[j][i] = cv->isnull[r] ? (SiftStrRef) {NULL, SIFT_STR_NULL} : par_str_at(&ld, cv, r);
			else
			{
				key[j][i] = cv->i[r];
				isnull[j][i] = cv->isnull[r];
			}
		}
	}
	p->w[worker].rows += ld.nact;
	out->n = ld.nact;
	out->tid = tid;
	for (int j = 0; j < nk; j++)
	{
		out->key[j] = key[j];
		out->isnull[j] = isnull[j];
		out->str[j] = str[j];
	}
	return true;
}

/* A winner into the heap of the statement, by its heap address */
static void
rows_emit(void *arg, const SiftParRowsRow *row)
{
	SiftV6ParScan *p = arg;
	const char *str[SIFT_PAR_ROWS_MAX_KEYS] = {0};
	uint32		slen[SIFT_PAR_ROWS_MAX_KEYS] = {0};
	bool		isnull[SIFT_PAR_ROWS_MAX_KEYS] = {0};

	if (p->finishing && p->npending > 0)
		return;
	for (int j = 0; j < p->sh.rs.nkeys; j++)
	{
		isnull[j] = row->isnull[j];
		str[j] = (const char *) row->str[j].p;
		slen[j] = row->str[j].len;
	}
	sift_v6_rows_put_raw(p->rows, row->key, str, slen, isnull, sift_pin_win_tid(p->win, (uint32) (row->tid >> ROW_BITS), (uint32) (row->tid & ((1u << ROW_BITS) - 1))));
}

static void *
rows_create(SiftV6ParScan *p, SiftParArenas *ar, uint64 rows, SiftErr *err)
{
	const SiftParRowsHooks hooks = {.fill = rows_fill,.emit = rows_emit,.arg = p};

	return sift_par_rows_create(ar, &p->sh.rs, &hooks, err);
}

const ParKind par_kind_rows = {.shape = rows_shape,.sizes = rows_sizes,.create = rows_create,.ops = &sift_par_rows_ops,.setup = rows_setup,.skip = rows_skip,.merges = true};
