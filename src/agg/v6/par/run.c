/*
 * agg/v6/par/run.c - the grouped aggregation of SiftAggV6 on the compute threads of the backend (agg/v6/par/run.h).
 *
 * What a thread does with a granule of the window (par_fill) calls nothing of the server: it filters the rows on the stored chunks, decodes the leaves of the keys and arguments of the rows that
 * pass, and gives them to the aggregation of agg/v6/par/group.h as arrays of 64-bit words. The key words of an integer are the value with its sign bit flipped, so that the unsigned order of the
 * words is the signed order of the values (the K best are ranked by the count or a sum, then by the keys in ascending order, as the statement says).
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/gpu_v6.h"
#include "agg/v6/par/group.h"
#include "agg/v6/par/run.h"
#include "col/f2/pin_win.h"
#include "col/granule/cfilt_str_simd.h"
#include "core/compat.h"
#include "core/par/pool.h"

#define SIGN_FLIP		(UINT64CONST(1) << 63)
#define SCRATCH_BYTES	(24u << 20)
#define WORK_BYTES		(16u << 20)
#define STATE_MIN		(8u << 20)
#define LEAVES_MAX		(2 * SIFT_PAR_GROUP_MAX_KW + SIFT_PAR_GROUP_MAX_SUM + SIFT_V6_MAX_KEYS)

typedef struct ParShape
{
	int			nk;
	bool		str;
	int			key_leaf[SIFT_PAR_GROUP_MAX_KW];
	int			nsum;
	int			sum_leaf[SIFT_PAR_GROUP_MAX_SUM];
	int			nleaves;
	int			leaf[LEAVES_MAX];	/* every leaf a thread decodes */
	int			key_at[SIFT_PAR_GROUP_MAX_KW];	/* the place of each key's leaf among leaf[] */
	int			sum_at[SIFT_PAR_GROUP_MAX_SUM];
	SiftParGroupSpec gs;
} ParShape;

struct SiftV6ParScan
{
	const SiftV6Spec *spec;
	SiftV6Group *g;
	SiftV6ScanStats *stats;
	Relation	index;
	SiftPinWin *win;
	ParShape	sh;
	SiftParGroup *grp;
	bool		finishing;
	uint32		fed;
	uint32		npending;
	uint32	   *pending;		/* the items a thread left to the leader */
	struct
	{
		uint64		rows;
		char		pad[56];
	}			w[SIFT_PAR_MAX_THREADS];
};

/* ---- the shape of the statement ---- */

static int
leaf_of(const int *first, const SiftV6Col *c)
{
	return first != NULL ? first[c->attno] : 0;
}

static int
add_leaf(ParShape *sh, int leaf)
{
	for (int i = 0; i < sh->nleaves; i++)
		if (sh->leaf[i] == leaf)
			return i;
	sh->leaf[sh->nleaves] = leaf;
	return sh->nleaves++;
}

static bool
plain_col(const SiftV6Col *c)
{
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->scale == 0 && c->attno > 0;
}

static bool
keys_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	sh->nk = sp->nkeys;
	if (sp->nkeys < 1 || sp->nkeys > SIFT_PAR_GROUP_MAX_KW)
		return false;
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Col *c = &sp->key[i];
		const SiftV6Class cls = sift_v6_class(c->typid);

		if (!plain_col(c) || !(cls == SV6C_I32 || cls == SV6C_I64 || (cls == SV6C_TEXT && sp->nkeys == 1)))
			return false;
		sh->str = cls == SV6C_TEXT;
		sh->key_leaf[i] = leaf_of(first, c);
		sh->key_at[i] = add_leaf(sh, sh->key_leaf[i]);
	}
	return true;
}

/* The count and the sums the aggregates need, and the leaves they read (a column that is not null for every row the statement reads: a NULL is the serial path's) */
static bool
aggs_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
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
			add_leaf(sh, leaf_of(first, &a->arg));
			continue;
		}
		if ((a->kind != SV6_SUM && a->kind != SV6_AVG) || (a->arg.typid != INT2OID && a->arg.typid != INT4OID) || sh->nsum == SIFT_PAR_GROUP_MAX_SUM)
			return false;
		sh->sum_leaf[sh->nsum] = leaf_of(first, &a->arg);
		sh->sum_at[sh->nsum] = add_leaf(sh, sh->sum_leaf[sh->nsum]);
		sh->nsum++;
	}
	return true;
}

/* The metric of the first item of ORDER BY and the keys, ascending, after it: the order of the K best of the threads */
static bool
order_of(const SiftV6Spec *sp, ParShape *sh)
{
	const SiftV6TopKItem *m = &sp->topk_item[0];
	const SiftV6Agg *a;
	Oid			opfamily,
				opcintype;
	CompareType cmptype;
	int			sum = 0;

	if (sp->topk < 1 || sp->topk > SIFT_PAR_GROUP_TOPK_MAX || sp->ntopk < 1 || sp->ntopk > 1 + sp->nkeys || m->pos < sp->nkeys || m->pos >= sp->nkeys + sp->naggs)
		return false;
	a = &sp->agg[m->pos - sp->nkeys];
	for (int j = 0; j < m->pos - sp->nkeys; j++)
		sum += sp->agg[j].kind == SV6_SUM || sp->agg[j].kind == SV6_AVG;
	if (a->kind == SV6_COUNT_STAR || a->kind == SV6_COUNT)
		sh->gs.order = 0;
	else if (a->kind == SV6_SUM)
		sh->gs.order = 1 + sum;
	else
		return false;
	get_ordering_op_properties(m->sortop, &opfamily, &opcintype, &cmptype);
	sh->gs.desc = cmptype == COMPARE_GT;
	for (int i = 1; i < sp->ntopk; i++)
	{
		const SiftV6TopKItem *it = &sp->topk_item[i];

		get_ordering_op_properties(it->sortop, &opfamily, &opcintype, &cmptype);
		if (it->pos != i - 1 || cmptype != COMPARE_LT || it->nulls_first || (sh->str && !sift_collate_is_c(it->collid)))
			return false;
	}
	sh->gs.topk = sp->topk;
	return true;
}

static bool
shape_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	*sh = (ParShape) {0};
	if (sp->is_table || sp->nparts != 0 || sp->ext != NULL || sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfcols != 0 || sp->nfilters != 0 || sp->lock_k != 0 ||
		sp->affine || sp->jsonqual != NIL || OidIsValid(sp->text_indexoid) || sp->naggs < 1)
		return false;
	if (!keys_of(sp, first, sh) || !aggs_of(sp, first, sh) || !order_of(sp, sh))
		return false;
	sh->gs.kw = sh->str ? 0 : sh->nk;
	sh->gs.str = sh->str;
	sh->gs.nsum = sh->nsum;
	return true;
}

/* ---- a thread ---- */

static bool
arena_full(SiftArena *a, SiftErr *err)
{
	sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, a->cap - a->used, "the arena of the thread has no room for a granule");
	return false;
}

static bool
serial(SiftErr *err)
{
	sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, "a NULL in a column of the aggregate");
	return false;
}

static SiftStrRef
str_at(const ColView *cv, uint32 r)
{
	if (cv->dcode != NULL)
		return (SiftStrRef) {(const uint8 *) cv->dict->bytes + cv->dict->off[cv->dcode[r]], cv->dict->off[cv->dcode[r] + 1] - cv->dict->off[cv->dcode[r]]};
	return (SiftStrRef) {(const uint8 *) cv->s[r], cv->slen[r]};
}

static bool
decode_all(const ParShape *sh, const SiftPinSlot *sl, SiftArena *a, SiftErr *err, const SiftStrMemo *memo, const uint16 *rows, int nrows, ColView *cv)
{
	for (int i = 0; i < sh->nleaves; i++)
		if (!sift_pin_slot_view_a(sl, a, err, memo, sh->leaf[i], sh->str && i == sh->key_at[0], rows, nrows, &cv[i]))
			return false;
	return true;
}

static bool
par_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	SiftV6ParScan *p = arg;
	const ParShape *sh = &p->sh;
	const SiftPinSlot *sl = sift_pin_win_slot(p->win, lo);
	SiftStrMemo *memo = sift_arena_alloc0(scratch, sizeof(SiftStrMemo), SIFT_ARENA_ALIGN);
	uint64	   *active = sift_arena_alloc(scratch, (SIFT_CFILT_WORDS + 2) * sizeof(uint64), SIFT_ARENA_ALIGN);
	ColView		cv[LEAVES_MAX];
	uint16	   *rows = NULL;
	uint64	   *key[SIFT_PAR_GROUP_MAX_KW] = {0},
			   *sum[SIFT_PAR_GROUP_MAX_SUM] = {0};
	SiftStrRef *str = NULL;
	uint32		nact,
				i = 0;
	const int	n = sl->nrows;

	if (memo == NULL || active == NULL)
		return arena_full(scratch, err);
	if (!sift_pin_slot_filter_a(sl, scratch, err, memo, active, &nact))
		return false;
	if (nact == 0)
		return true;
	if (nact * 4 < (uint32) n && nact <= SIFT_POINT_ROWS_MAX)
	{
		rows = sift_arena_alloc(scratch, nact * sizeof(uint16), 8);
		if (rows == NULL)
			return arena_full(scratch, err);
		for (int w = 0; w < (n + 63) / 64; w++)
			for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
				rows[i++] = w * 64 + __builtin_ctzll(bits);
	}
	if (!decode_all(sh, sl, scratch, err, memo, rows, rows != NULL ? (int) nact : 0, cv))
		return false;
	for (int j = 0; !sh->str && j < sh->nk; j++)
		key[j] = sift_arena_alloc(scratch, nact * sizeof(uint64), 8);
	for (int m = 0; m < sh->nsum; m++)
		sum[m] = sift_arena_alloc(scratch, nact * sizeof(uint64), 8);
	str = sh->str ? sift_arena_alloc(scratch, nact * sizeof(SiftStrRef), 8) : NULL;
	for (int j = 0; !sh->str && j < sh->nk; j++)
		if (key[j] == NULL)
			return arena_full(scratch, err);
	for (int m = 0; m < sh->nsum; m++)
		if (sum[m] == NULL)
			return arena_full(scratch, err);
	if (sh->str && str == NULL)
		return arena_full(scratch, err);
	i = 0;
	for (int w = 0; w < (n + 63) / 64; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1, i++)
		{
			const uint32 r = w * 64 + __builtin_ctzll(bits);

			for (int c = 0; c < sh->nleaves; c++)
				if (cv[c].isnull[r])
					return serial(err);
			for (int j = 0; !sh->str && j < sh->nk; j++)
				key[j][i] = (uint64) cv[sh->key_at[j]].i[r] ^ SIGN_FLIP;
			if (sh->str)
				str[i] = str_at(&cv[sh->key_at[0]], r);
			for (int m = 0; m < sh->nsum; m++)
				sum[m][i] = (uint64) cv[sh->sum_at[m]].i[r];
		}
	p->w[worker].rows += nact;
	out->n = nact;
	for (int j = 0; j < sh->nk && !sh->str; j++)
		out->key[j] = key[j];
	for (int m = 0; m < sh->nsum; m++)
		out->arg[m] = sum[m];
	out->str = str;
	return true;
}

/* ---- the leader ---- */

/* A group of the K best, or a partial aggregate of one, into the groups of the statement */
static void
par_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	SiftV6ParScan *p = arg;
	const SiftV6Spec *sp = p->spec;
	int64		key[SIFT_V6_MAX_KEYS] = {0};
	SiftV6StrIn str[SIFT_V6_MAX_KEYS] = {{0}};
	SiftV6AccIn acc[SIFT_V6_MAX_KEYS + 16];
	int			m = 0;

	if (p->finishing && p->npending > 0)
		return;
	Assert(sp->naggs <= lengthof(acc));
	if (p->sh.str)
		str[0] = (SiftV6StrIn) {(const char *) row->str, row->len};
	else
		for (int i = 0; i < p->sh.nk; i++)
			key[i] = (int64) (row->key[i] ^ SIGN_FLIP);
	for (int j = 0; j < sp->naggs; j++)
	{
		acc[j] = (SiftV6AccIn) {.n = (int64) row->count};
		if (sp->agg[j].kind == SV6_SUM || sp->agg[j].kind == SV6_AVG)
		{
			acc[j].v0 = row->sum[m++];
			acc[j].v1 = (int64) acc[j].v0 < 0 ? -1 : 0;
		}
	}
	sift_v6_group_fold_rec(p->g, key, str, p->sh.str && row->len == SIFT_STR_NULL ? 1 : 0, acc);
}

static void
par_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
}

static bool
op_feed(void *vp, uint32 item)
{
	SiftV6ParScan *p = vp;
	const bool	fed = sift_pin_win_feed(p->win, item);

	p->fed += fed;
	return fed;
}

static void
op_release(void *vp, uint32 item)
{
	sift_pin_win_release(((SiftV6ParScan *) vp)->win, item);
}

static void
op_fallback(void *vp, uint32 item)
{
	SiftV6ParScan *p = vp;

	p->pending[p->npending++] = item;
}

static bool
op_phase1(void *vp, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	return sift_par_group_ops.phase1(((SiftV6ParScan *) vp)->grp, t, lo, hi, scratch, done, err);
}

static bool
op_seal(void *vp, uint32 *nparts, SiftErr *err)
{
	return sift_par_group_ops.seal(((SiftV6ParScan *) vp)->grp, nparts, err);
}

static bool
op_phase2(void *vp, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	return sift_par_group_ops.phase2(((SiftV6ParScan *) vp)->grp, t, part, work, err);
}

static void
op_finish(void *vp)
{
	SiftV6ParScan *p = vp;

	p->finishing = true;
	sift_par_group_ops.finish(p->grp);
}

static void
op_overflow(void *vp, const SiftParRange *left, uint32 nleft)
{
	sift_par_group_ops.overflow_drain(((SiftV6ParScan *) vp)->grp, left, nleft);
}

static const SiftParOps run_ops = {.feed_slot = op_feed,.release_slot = op_release,.fallback_item = op_fallback,.phase1 = op_phase1,.seal = op_seal,.phase2 = op_phase2,.finish = op_finish,
	.overflow_drain = op_overflow};

bool
sift_v6_par_wanted(const SiftV6Spec *spec)
{
	ParShape	sh;

	return sift_par_pool_threads() > 1 && shape_of(spec, NULL, &sh);
}

SiftV6ParScan *
sift_v6_par_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg)
{
	SiftV6ParScan *p;
	ParShape	sh;
	const int	nthreads = sift_par_pool_threads();

	if (nthreads < 2 || !shape_of(spec, first, &sh))
		return NULL;
	p = palloc0(sizeof(SiftV6ParScan));
	p->spec = spec;
	p->g = g;
	p->stats = stats;
	p->index = index;
	p->sh = sh;
	p->win = sift_pin_win_new(index, snap, p->sh.leaf, p->sh.nleaves, 2 * nthreads, part_ok, part_arg);
	return p;
}

SiftPinWin *
sift_v6_par_window(SiftV6ParScan *p)
{
	return p->win;
}

static void
replay_items(SiftV6ParScan *p, const uint32 *items, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		sift_pin_win_replay(p->win, items[i]);
}

/* Whether the arenas of nthreads threads for the rows of the window fit the memory the statement may take */
static bool
arenas_fit(const ParShape *sh, uint64 rows, uint32 groups_est, int nthreads, size_t *state)
{
	const uint32 words = sh->nk + sh->nsum;
	const uint64 rec = sh->str ? 64 : 8 * (words <= 1 ? 1 : pg_nextpower2_32(words));

	*state = Max((size_t) STATE_MIN, (size_t) (4 * rec * Min(rows / nthreads, (uint64) groups_est)));
	return (uint64) nthreads * (SCRATCH_BYTES + WORK_BYTES + *state) <= (uint64) work_mem * 1024 * 3 / 4;
}

static void
run_threads(SiftV6ParScan *p, int nthreads, size_t state)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift agg v6 par", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const uint32 items = sift_pin_win_items(p->win);
	SiftParArenas *ar = sift_par_arenas_create(cx, nthreads, SCRATCH_BYTES, state, WORK_BYTES);
	const SiftParGroupHooks hooks = {.fill = par_fill,.emit = par_emit,.overflow = par_overflow,.arg = p};
	SiftErr		err = {0};
	bool		done;

	p->pending = palloc(sizeof(uint32) * items);
	p->grp = sift_par_group_create(ar, &p->sh.gs, p->spec->groups_est > SIFT_PAR_GROUP_LOW, sift_pin_win_rows(p->win), &hooks, &err);
	if (p->grp == NULL)
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		for (uint32 i = 0; i < items; i++)
			sift_pin_win_replay(p->win, i);
		return;
	}
	done = sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = 2 * nthreads}, &run_ops, p, ar, p->index);
	MemoryContextSwitchTo(old);
	if (!done)
	{
		replay_items(p, p->pending, p->npending);
		for (uint32 i = p->fed; i < items; i++)
			sift_pin_win_replay(p->win, i);
	}
	else if (p->npending > 0)
		for (uint32 i = 0; i < items; i++)
			sift_pin_win_replay(p->win, i);
	for (int t = 0; t < nthreads; t++)
		p->stats->rows += p->w[t].rows * (done && p->npending == 0);
	elog(DEBUG1, "sift agg v6: %u granules on %d threads, %s", items, nthreads, done && p->npending == 0 ? "folded by the threads" : "replayed by the scan");
	MemoryContextDelete(cx);
}

void
sift_v6_par_end(SiftV6ParScan *p)
{
	const uint32 items = sift_pin_win_items(p->win);
	const int	nthreads = Min(sift_par_pool_threads(), (int) Max(items, 1));
	size_t		state;

	PG_TRY();
	{
		if (items == 0)
			;
		else if (p->stats->rows > 0 || sift_pin_win_refused(p->win) > 0 || nthreads < 2 || !arenas_fit(&p->sh, sift_pin_win_rows(p->win), p->spec->groups_est, nthreads, &state))
		{
			elog(DEBUG1, "sift agg v6: %u granules replayed by the scan (%u refused, %lu rows read)", items, sift_pin_win_refused(p->win), (unsigned long) p->stats->rows);
			for (uint32 i = 0; i < items; i++)
				sift_pin_win_replay(p->win, i);
		}
		else
			run_threads(p, nthreads, state);
	}
	PG_FINALLY();
	{
		sift_pin_win_end(p->win);
	}
	PG_END_TRY();
	pfree(p);
}
