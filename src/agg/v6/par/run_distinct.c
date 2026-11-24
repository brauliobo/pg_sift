/*
 * agg/v6/par/run_distinct.c - COUNT(DISTINCT x) of SiftAggV6 on the compute threads (agg/v6/par/run.h): x is an integer or a text column, the statement has no key or keys (integer columns and one
 * text), and COUNT(*), COUNT(column), SUM and AVG of int2 and int4 columns and MIN and MAX of text columns next to the one distinct count (agg/v6/par/distinct.h). A NULL in a key or in a column of a
 * sum is not for a thread (the scan does it); a NULL value is not counted, and its row is a row of its group; a NULL text is not a minimum or maximum. The plan takes a text MIN or MAX only of a
 * column in the C collation (agg/v6/plan.c), the order of the bytes that the threads compare by.
 */
#include "postgres.h"

#include "agg/v6/par/keytab.h"
#include "agg/v6/par/run_int.h"

#define AGGS_MAX		16
#define STATE_MIN		(8u << 20)
#define DIRECT_MAX		(1u << 16)

static bool
int_col(const SiftV6Col *c)
{
	const SiftV6Class cls = sift_v6_class(c->typid);

	return par_plain_col(c) && (cls == SV6C_I32 || cls == SV6C_I64);
}

static bool
text_col(const SiftV6Col *c)
{
	return par_plain_col(c) && sift_v6_class(c->typid) == SV6C_TEXT;
}

static bool
keys_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	sh->nk = sp->nkeys;
	if (sp->nkeys > SIFT_PAR_GROUP_MAX_KEYS)
		return false;
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Col *c = &sp->key[i];

		sh->ktext[i] = text_col(c);
		if (!int_col(c) && !(sh->ktext[i] && !sh->ds.kstr))
			return false;
		sh->ds.kstr |= sh->ktext[i];
		sh->ds.kw += !sh->ktext[i];
		sh->key_at[i] = par_add_leaf(sh, par_leaf_of(first, c), sh->ktext[i]);
		par_check_leaf(sh, sh->key_at[i]);
	}
	return true;
}

static bool
value_of(const SiftV6Agg *a, const int *first, ParShape *sh)
{
	if (!int_col(&a->arg) && !text_col(&a->arg))
		return false;
	sh->ds.vstr = text_col(&a->arg);
	sh->val_at = par_add_leaf(sh, par_leaf_of(first, &a->arg), sh->ds.vstr);
	return true;
}

static bool
min_max_of(const SiftV6Agg *a, const int *first, ParShape *sh)
{
	if (a->fclause != 0 || a->nxcols != 0 || !text_col(&a->arg) || sh->ds.nmm == SIFT_PAR_DISTINCT_MM_MAX)
		return false;
	sh->ds.mm_max |= (a->kind == SV6_MAX) << sh->ds.nmm;
	sh->mm_at[sh->ds.nmm++] = par_add_leaf(sh, par_leaf_of(first, &a->arg), true);
	return true;
}

/* The one distinct count, the text minimums and maximums, and the counts and sums that go with them, with the leaves they read */
static bool
aggs_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	int			ndistinct = 0;

	if (sp->naggs > AGGS_MAX)
		return false;
	for (int j = 0; j < sp->naggs; j++)
	{
		const SiftV6Agg *a = &sp->agg[j];

		if (a->kind == SV6_COUNT_DISTINCT && a->fclause == 0 && a->nxcols == 0)
		{
			ndistinct++;
			if (!value_of(a, first, sh))
				return false;
		}
		else if (a->kind == SV6_MIN || a->kind == SV6_MAX)
		{
			if (!min_max_of(a, first, sh))
				return false;
		}
		else if (!par_count_sum_of(a, first, sh))
			return false;
	}
	sh->ds.nsum = sh->nsum;
	return ndistinct == 1;
}

static bool
distinct_shape(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 || sp->affine)
		return false;
	return keys_of(sp, first, sh) && aggs_of(sp, first, sh);
}

static void
distinct_sizes(const ParShape *sh, uint32 groups_est, uint64 rows, int nthreads, ParSizes *sz)
{
	const uint64 per = rows / nthreads;
	const bool	grouped = sh->ds.kw > 0 || sh->ds.kstr;
	const uint64 keys = grouped ? Min(per, groups_est > 0 ? groups_est : per / 8) : 0;
	const uint64 texts = sh->ds.vstr ? per / 8 : 0;
	const bool	direct = sh->ds.kw == 1 && !sh->ds.kstr;
	const uint64 direct_bytes = direct ? (uint64) DIRECT_MAX * (2 + sh->ds.nsum + 3 * sh->ds.nmm) * sizeof(uint64) : 0;

	sz->scratch = 8u << 20;
	sz->work = (8u << 20) + rows / 16;
	sz->state_min = STATE_MIN + direct_bytes;
	sz->state_want = sz->state_min + 3 * (grouped ? sizeof(uint64) * 2 : sizeof(uint64)) * per + 256 * (keys + texts);
}

/* ---- a thread ---- */

static void *
batch_array(SiftArena *scratch, uint32 n, size_t size)
{
	return sift_arena_alloc(scratch, n * size, 8);
}

static bool
fill_keys(const ParShape *sh, const ParLoad *ld, SiftArena *scratch, SiftParDistinctBatch *b)
{
	int			word = 0;

	for (int i = 0; i < sh->nk; i++)
	{
		const ColView *cv = &ld->cv[sh->key_at[i]];
		uint64	   *w = sh->ktext[i] ? NULL : batch_array(scratch, ld->nact, sizeof(uint64));
		SiftStrRef *s = sh->ktext[i] ? batch_array(scratch, ld->nact, sizeof(SiftStrRef)) : NULL;

		if (w == NULL && s == NULL)
			return false;
		PAR_EACH_ROW(ld, r, k)
		{
			if (s != NULL)
				s[k] = par_str_at(cv, r);
			else
				w[k] = (uint64) cv->i[r];
		}
		if (s != NULL)
			b->kstr = s;
		else
			b->key[word++] = w;
	}
	return true;
}

static bool
fill_sums(const ParShape *sh, const ParLoad *ld, SiftArena *scratch, SiftParDistinctBatch *b)
{
	for (int m = 0; m < sh->ds.nsum; m++)
	{
		const int64 *v = ld->cv[sh->sum_at[m]].i;
		uint64	   *s = batch_array(scratch, ld->nact, sizeof(uint64));

		if (s == NULL)
			return false;
		PAR_EACH_ROW(ld, r, k)
			s[k] = (uint64) v[r];
		b->sum[m] = s;
	}
	return true;
}

static bool
fill_text_values(const ColView *cv, const ParLoad *ld, SiftArena *scratch, SiftParDistinctBatch *b)
{
	SiftStrRef *s = batch_array(scratch, ld->nact, sizeof(SiftStrRef));

	if (s == NULL)
		return false;
	PAR_EACH_ROW(ld, r, k)
		s[k] = cv->isnull[r] ? (SiftStrRef) {NULL, SIFT_STR_NULL} : par_str_at(cv, r);
	b->vstr = s;
	return true;
}

static bool
fill_word_values(const ColView *cv, const ParLoad *ld, SiftArena *scratch, SiftParDistinctBatch *b)
{
	uint64	   *v = batch_array(scratch, ld->nact, sizeof(uint64));
	uint8	   *null = batch_array(scratch, ld->nact, sizeof(uint8));

	if (v == NULL || null == NULL)
		return false;
	PAR_EACH_ROW(ld, r, k)
	{
		v[k] = (uint64) cv->i[r];
		null[k] = cv->isnull[r];
	}
	b->val = v;
	b->vnull = null;
	return true;
}

static bool
fill_mm(const ParShape *sh, const ParLoad *ld, SiftArena *scratch, SiftParDistinctBatch *b)
{
	for (int m = 0; m < sh->ds.nmm; m++)
	{
		const ColView *cv = &ld->cv[sh->mm_at[m]];
		SiftStrRef *s = batch_array(scratch, ld->nact, sizeof(SiftStrRef));

		if (s == NULL)
			return false;
		PAR_EACH_ROW(ld, r, k)
			s[k] = cv->isnull[r] ? (SiftStrRef) {NULL, SIFT_STR_NULL} : par_str_at(cv, r);
		b->mm[m] = s;
	}
	return true;
}

static bool
fill_values(const ParShape *sh, const ParLoad *ld, SiftArena *scratch, SiftParDistinctBatch *b)
{
	const ColView *cv = &ld->cv[sh->val_at];

	return sh->ds.vstr ? fill_text_values(cv, ld, scratch, b) : fill_word_values(cv, ld, scratch, b);
}

static bool
distinct_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParDistinctBatch *out, SiftErr *err)
{
	SiftV6ParScan *p = arg;
	const ParShape *sh = &p->sh;
	ParLoad		ld;

	if (!par_load(sh, sift_pin_win_slot(p->win, lo), scratch, err, &ld))
		return false;
	if (ld.nact == 0)
		return true;
	for (int c = 0; c < sh->nchk; c++)
		if (par_has_null(&ld, &ld.cv[sh->chk[c]]))
			return par_serial(err, "a NULL in a column of the aggregate");
	if (!fill_keys(sh, &ld, scratch, out) || !fill_sums(sh, &ld, scratch, out) || !fill_mm(sh, &ld, scratch, out) || !fill_values(sh, &ld, scratch, out))
		return par_arena_full(scratch, err);
	p->w[worker].rows += ld.nact;
	out->n = ld.nact;
	return true;
}

static void
distinct_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
}

/* The zone of the one integer key of every granule: the keys of the run are then in a range, which may be narrow */
static void
distinct_setup(SiftV6ParScan *p)
{
	if (p->sh.ds.kw == 1 && !p->sh.ds.kstr)
		sift_pin_win_track_zone(p->win, p->sh.leaf[p->sh.key_at[0]]);
}

static void *
distinct_create(SiftV6ParScan *p, SiftParArenas *ar, uint64 rows, SiftErr *err)
{
	const SiftParDistinctHooks hooks = {.fill = distinct_fill,.overflow = distinct_overflow,.arg = p};
	SiftParDistinctSpec *ds = &p->sh.ds;
	int64		lo,
				hi;

	if (ds->kw == 1 && !ds->kstr && sift_pin_win_zone_range(p->win, &lo, &hi) && (uint64) hi - (uint64) lo < DIRECT_MAX)
	{
		ds->direct_lo = lo;
		ds->direct_n = (uint32) ((uint64) hi - (uint64) lo + 1);
	}

	return sift_par_distinct_create(ar, &p->sh.ds, &hooks, err);
}

/* ---- the leader ---- */

/* A group of the answer into the groups of the statement: the distinct count is a piece the aggregate adds up, and each group comes once */
static void
distinct_emit(void *arg, const SiftParDistinctRow *row)
{
	SiftV6ParScan *p = arg;
	const SiftV6Spec *sp = p->spec;
	int64		key[SIFT_V6_MAX_KEYS] = {0};
	SiftV6StrIn str[SIFT_V6_MAX_KEYS] = {{0}};
	SiftV6AccIn acc[AGGS_MAX] = {{0}};
	int			word = 0,
				m = 0,
				mm = 0;

	for (int i = 0; i < sp->nkeys; i++)
	{
		if (p->sh.ktext[i])
			str[i] = (SiftV6StrIn) {(const char *) row->str, row->len};
		else
			key[i] = (int64) row->key[word++];
	}
	for (int j = 0; j < sp->naggs; j++)
	{
		const SiftV6Agg *a = &sp->agg[j];

		if (a->kind == SV6_COUNT_DISTINCT)
			acc[j].v0 = row->distinct;
		else if (a->kind == SV6_MIN || a->kind == SV6_MAX)
		{
			acc[j].str = row->mm[mm].p;
			acc[j].len = row->mm[mm++].len;
		}
		else
			acc[j].n = (int64) row->rows;
		if (a->kind == SV6_SUM || a->kind == SV6_AVG)
		{
			acc[j].v0 = row->sum[m++];
			acc[j].v1 = (int64) acc[j].v0 < 0 ? -1 : 0;
		}
	}
	sift_v6_group_fold_rec(p->g, key, str, 0, acc);
}

static void
distinct_done(SiftV6ParScan *p)
{
	sift_par_distinct_each(p->op, distinct_emit, p);
}

const ParKind par_kind_distinct = {.shape = distinct_shape,.sizes = distinct_sizes,.create = distinct_create,.ops = &sift_par_distinct_ops,.setup = distinct_setup,.done = distinct_done};
