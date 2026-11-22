/*
 * agg/topn_v6.c - the Top-N tournament of agg/topn_v6.h. The queue of units is a binary heap with the best bound at the root; the rows kept are a binary heap
 * with the worst of them at the root (theta), and for WITH TIES the rows that tie theta are kept aside while theta stays as it is.
 *
 * Whether a unit can be skipped is decided on its best value of the leading key and, when the leading keys tie, of the second: a bound of NULLs and numbers
 * is a class (2: NULLs come first and the unit may hold one, 1: a number, 0: NULLs come last and the unit holds nothing else) and a value, and theta is the same
 * pair. A unit strictly below theta on the leading key ends the tournament, because every unit still queued is below it too; below on the second key it is only
 * skipped.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/topn_v6.h"
#include "core/compat.h"

typedef enum Verdict
{
	V_BEAT,						/* the unit may hold a row that is better than theta, or that ties it where ties are kept */
	V_CANNOT,					/* it cannot, by the second key */
	V_CANNOT_KEY0				/* it cannot, by the leading key: nor can any unit behind it */
} Verdict;

struct SiftTournament
{
	SiftTournSpec spec;
	uint64		k;
	uint64		max_tie_rows;
	SiftMorselQueue *queue;
	MemoryContext cxt;

	SiftTopNSchedTok *sched;
	uint32		sched_len;
	uint32		sched_cap;
	SiftTopNWinRow *win;
	uint32		win_len;
	uint32		win_cap;
	SiftTopNWinRow *tie;
	uint32		tie_len;
	uint32		tie_cap;
	SiftTopNWinRow *res;

	SiftTournStats st;
};

SiftTournament *
sift_tourn_create(MemoryContext cxt, const SiftTournSpec *spec, uint64 max_bytes, SiftMorselQueue *queue)
{
	SiftTournament *t;
	const uint64 k = spec->offset + spec->limit;

	if (spec->nkeys < 1 || spec->nkeys > SIFT_TOURN_MAX_KEYS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a Top-N of %d sort keys is not run by the tournament", spec->nkeys),
						errhint("Use at most %d sort keys.", SIFT_TOURN_MAX_KEYS)));
	if (k < spec->offset || k > SIFT_TOURN_MAX_K)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a Top-N of more than %d rows is not run by the tournament", SIFT_TOURN_MAX_K),
						errhint("Use a smaller LIMIT plus OFFSET.")));
	t = MemoryContextAllocZero(cxt, sizeof(SiftTournament));
	t->spec = *spec;
	t->k = spec->limit == 0 ? 0 : k;
	t->max_tie_rows = max_bytes / sizeof(SiftTopNWinRow);
	t->queue = queue;
	t->cxt = cxt;
	return t;
}

void
sift_tourn_free(SiftTournament *t)
{
	if (t->sched)
		pfree(t->sched);
	if (t->win)
		pfree(t->win);
	if (t->tie)
		pfree(t->tie);
	if (t->res)
		pfree(t->res);
	pfree(t);
}

const SiftTournStats *
sift_tourn_stats(const SiftTournament *t)
{
	return &t->st;
}

void
sift_tourn_add_strides_skipped(SiftTournament *t, uint64 n)
{
	t->st.strides_skipped += n;
}

/* ---- the order of the keys ---- */

static inline int
cmp_i64(int64 a, int64 b)
{
	return (a > b) - (a < b);
}

/* negative: a sorts before b on key j */
static int
key_cmp(const SiftTournKey *k, const SiftTopNWinRow *a, const SiftTopNWinRow *b, int j)
{
	const bool	an = (a->nulls >> j) & 1,
				bn = (b->nulls >> j) & 1;

	if (an != bn)
		return an == k->nulls_first ? -1 : 1;
	if (an)
		return 0;
	return k->desc ? cmp_i64(b->key[j], a->key[j]) : cmp_i64(a->key[j], b->key[j]);
}

static int
keys_cmp(const SiftTournament *t, const SiftTopNWinRow *a, const SiftTopNWinRow *b)
{
	for (int j = 0; j < t->spec.nkeys; j++)
	{
		const int	c = key_cmp(&t->spec.keys[j], a, b, j);

		if (c)
			return c;
	}
	return 0;
}

static int
row_cmp(const SiftTournament *t, const SiftTopNWinRow *a, const SiftTopNWinRow *b)
{
	const int	c = keys_cmp(t, a, b);

	return c ? c : (a->tid > b->tid) - (a->tid < b->tid);
}

static int
row_cmp_arg(const void *a, const void *b, void *arg)
{
	return row_cmp(arg, a, b);
}

/* ---- the bound of a unit against theta ---- */

static void
best_of(const SiftTournKey *k, const SiftTournBound *b, int64 *val, uint8 *prio)
{
	*val = k->desc ? INT64_MAX : INT64_MIN;
	if (!b || !b->known)
		*prio = k->nulls_first ? 2 : 1;
	else if (b->all_null)
		*prio = k->nulls_first ? 2 : 0;
	else
	{
		*val = k->desc ? b->max : b->min;
		*prio = b->has_null && k->nulls_first ? 2 : 1;
	}
}

static uint8
class_of(const SiftTournKey *k, bool is_null)
{
	return is_null ? (k->nulls_first ? 2 : 0) : 1;
}

/* a unit of this class and best value against a row: -1 below it, 0 equal, 1 above */
static int
bound_cmp(const SiftTournKey *k, uint8 prio, int64 val, bool row_null, int64 row_val)
{
	const uint8 row_prio = class_of(k, row_null);

	if (prio != row_prio)
		return prio < row_prio ? -1 : 1;
	if (prio != 1)
		return 0;
	return k->desc ? cmp_i64(val, row_val) : cmp_i64(row_val, val);
}

/* the class and value of a leading key as one integer that grows with the key's rank, 0 = worst possible (used by the participants that share a bound) */
static uint64
goodness(const SiftTournKey *k, uint8 prio, int64 val)
{
	uint64		u;

	if (prio != 1)
		return prio == 2 ? UINT64_MAX : 0;
	u = (uint64) val ^ (UINT64CONST(1) << 63);
	if (!k->desc)
		u = ~u;
	return Max(UINT64CONST(1), Min(u, UINT64_MAX - 1));
}

static Verdict
local_verdict(const SiftTournament *t, const SiftTopNSchedTok *tok)
{
	const SiftTopNWinRow *th = &t->win[0];
	const SiftTournKey *keys = t->spec.keys;
	const Verdict tied = t->spec.with_ties ? V_BEAT : V_CANNOT;
	int			c = bound_cmp(&keys[0], tok->null_prio, tok->best_val, th->nulls & 1, th->key[0]);

	if (c)
		return c < 0 ? V_CANNOT_KEY0 : V_BEAT;
	if (t->spec.nkeys == 1)
		return tied;
	if (tok->tie_prio == SIFT_TOURN_NO_TIE)
		return V_BEAT;
	c = bound_cmp(&keys[1], tok->tie_prio, tok->best_tie, (th->nulls >> 1) & 1, th->key[1]);
	if (c)
		return c < 0 ? V_CANNOT : V_BEAT;
	return t->spec.nkeys > 2 ? V_BEAT : tied;
}

static Verdict
tok_verdict(const SiftTournament *t, const SiftTopNSchedTok *tok)
{
	const uint64 shared = t->queue ? sift_morsel_theta_load(t->queue) : 0;

	if (shared && goodness(&t->spec.keys[0], tok->null_prio, tok->best_val) < shared)
		return V_CANNOT_KEY0;
	return t->win_len < t->k ? V_BEAT : local_verdict(t, tok);
}

static void
count_pruned(SiftTournament *t, const SiftTopNSchedTok *tok)
{
	if (tok->kind == STOK_PARTITION)
		t->st.partitions_pruned++;
	else if (tok->kind == STOK_PART)
		t->st.parts_pruned++;
	else
		t->st.granules_pruned++;
}

/* ---- the queue of units: the best bound at the root ---- */

static bool
tok_before(const SiftTournament *t, const SiftTopNSchedTok *a, const SiftTopNSchedTok *b)
{
	if (a->null_prio != b->null_prio)
		return a->null_prio > b->null_prio;
	if (a->null_prio == 1 && a->best_val != b->best_val)
		return t->spec.keys[0].desc ? a->best_val > b->best_val : a->best_val < b->best_val;
	if (a->kind != b->kind)
		return a->kind < b->kind;
	return a->part_slot != b->part_slot ? a->part_slot < b->part_slot : a->local_g < b->local_g;
}

static void
sched_add(SiftTournament *t, const SiftTopNSchedTok *tok)
{
	uint32		i;

	if (t->sched_len == t->sched_cap)
	{
		t->sched_cap = t->sched_cap ? t->sched_cap * 2 : 64;
		t->sched = t->sched ? repalloc(t->sched, (Size) t->sched_cap * sizeof(*t->sched)) :
			MemoryContextAlloc(t->cxt, (Size) t->sched_cap * sizeof(*t->sched));
	}
	for (i = t->sched_len++; i > 0 && tok_before(t, tok, &t->sched[(i - 1) / 2]); i = (i - 1) / 2)
		t->sched[i] = t->sched[(i - 1) / 2];
	t->sched[i] = *tok;
}

static SiftTopNSchedTok
sched_pop(SiftTournament *t)
{
	const SiftTopNSchedTok top = t->sched[0],
				last = t->sched[--t->sched_len];
	uint32		i = 0;

	for (;;)
	{
		uint32		c = 2 * i + 1;

		if (c >= t->sched_len)
			break;
		if (c + 1 < t->sched_len && tok_before(t, &t->sched[c + 1], &t->sched[c]))
			c++;
		if (!tok_before(t, &t->sched[c], &last))
			break;
		t->sched[i] = t->sched[c];
		i = c;
	}
	if (t->sched_len)
		t->sched[i] = last;
	return top;
}

void
sift_tourn_push(SiftTournament *t, SiftTournKind kind, uint32 part_slot, uint32 local_g, uint32 part_idx, const SiftTournBound *k0, const SiftTournBound *k1)
{
	SiftTopNSchedTok tok = {.part_slot = part_slot, .local_g = local_g, .part_idx = part_idx, .kind = (uint8) kind};

	if (t->k == 0)
		return;
	best_of(&t->spec.keys[0], k0, &tok.best_val, &tok.null_prio);
	if (t->spec.nkeys > 1 && k1 && k1->known)
		best_of(&t->spec.keys[1], k1, &tok.best_tie, &tok.tie_prio);
	else
		tok.tie_prio = SIFT_TOURN_NO_TIE;
	if (tok_verdict(t, &tok) != V_BEAT)
		count_pruned(t, &tok);
	else
		sched_add(t, &tok);
}

/* ---- the rows kept ---- */

static void
win_up(SiftTournament *t, uint32 i, const SiftTopNWinRow *row)
{
	for (; i > 0 && row_cmp(t, row, &t->win[(i - 1) / 2]) > 0; i = (i - 1) / 2)
		t->win[i] = t->win[(i - 1) / 2];
	t->win[i] = *row;
}

static void
win_down(SiftTournament *t, const SiftTopNWinRow *row)
{
	uint32		i = 0;

	for (;;)
	{
		uint32		c = 2 * i + 1;

		if (c >= t->win_len)
			break;
		if (c + 1 < t->win_len && row_cmp(t, &t->win[c + 1], &t->win[c]) > 0)
			c++;
		if (row_cmp(t, &t->win[c], row) <= 0)
			break;
		t->win[i] = t->win[c];
		i = c;
	}
	t->win[i] = *row;
}

static void
win_add(SiftTournament *t, const SiftTopNWinRow *row)
{
	if (t->win_len == t->win_cap)
	{
		t->win_cap = Min((uint64) Max(t->win_cap * 2, 64), t->k);
		t->win = t->win ? repalloc(t->win, (Size) t->win_cap * sizeof(*t->win)) : MemoryContextAlloc(t->cxt, (Size) t->win_cap * sizeof(*t->win));
	}
	win_up(t, t->win_len++, row);
}

static void
tie_add(SiftTournament *t, const SiftTopNWinRow *row)
{
	if (t->tie_len >= t->max_tie_rows)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the rows that tie the last of the Top-N are more than %lu", (unsigned long) t->tie_len),
						errhint("Raise work_mem, or take the rows without WITH TIES.")));
	if (t->tie_len == t->tie_cap)
	{
		t->tie_cap = t->tie_cap ? t->tie_cap * 2 : 64;
		t->tie = t->tie ? repalloc(t->tie, (Size) t->tie_cap * sizeof(*t->tie)) : MemoryContextAlloc(t->cxt, (Size) t->tie_cap * sizeof(*t->tie));
	}
	t->tie[t->tie_len++] = *row;
}

static void
publish_theta(SiftTournament *t)
{
	const SiftTournKey *k = &t->spec.keys[0];
	const bool	is_null = t->win[0].nulls & 1;

	if (t->queue)
		sift_morsel_theta_raise(t->queue, goodness(k, class_of(k, is_null), t->win[0].key[0]));
}

bool
sift_tourn_offer(SiftTournament *t, const SiftTopNWinRow *row)
{
	SiftTopNWinRow evicted;
	int			c;

	if (t->win_len < t->k)
	{
		win_add(t, row);
		if (t->win_len == t->k)
			publish_theta(t);
		return true;
	}
	if (t->k == 0)
		return false;
	c = keys_cmp(t, row, &t->win[0]);
	if (c > 0 || (c == 0 && !t->spec.with_ties))
		return false;
	if (c == 0)
	{
		tie_add(t, row);
		return true;
	}
	evicted = t->win[0];
	win_down(t, row);
	if (t->spec.with_ties)
	{
		if (keys_cmp(t, &t->win[0], &evicted) == 0)
			tie_add(t, &evicted);
		else
			t->tie_len = 0;
	}
	publish_theta(t);
	return true;
}

bool
sift_tourn_theta(const SiftTournament *t, int64 *key0, bool *null0)
{
	if (t->k == 0 || t->win_len < t->k)
		return false;
	*key0 = t->win[0].key[0];
	*null0 = t->win[0].nulls & 1;
	return true;
}

void
sift_tourn_merge(SiftTournament *dst, const SiftTournament *src)
{
	for (uint32 i = 0; i < src->win_len; i++)
		sift_tourn_offer(dst, &src->win[i]);
	for (uint32 i = 0; i < src->tie_len; i++)
		sift_tourn_offer(dst, &src->tie[i]);
}

const SiftTopNWinRow *
sift_tourn_result(SiftTournament *t, uint32 *n)
{
	const uint32 all = t->win_len + t->tie_len;
	uint32		end,
				start;

	if (t->res)
		pfree(t->res);
	t->res = MemoryContextAlloc(t->cxt, Max(1, all) * sizeof(*t->res));
	if (t->win_len)
		memcpy(t->res, t->win, t->win_len * sizeof(*t->res));
	if (t->tie_len)
		memcpy(t->res + t->win_len, t->tie, t->tie_len * sizeof(*t->res));
	qsort_arg(t->res, all, sizeof(*t->res), row_cmp_arg, t);
	end = t->spec.with_ties ? all : (uint32) Min(all, t->k);
	start = (uint32) Min(t->spec.offset, end);
	*n = end - start;
	return t->res + start;
}

/* ---- the tournament ---- */

bool
sift_tourn_step(SiftTournament *t, const SiftTournOps *ops, void *src)
{
	SiftTopNSchedTok tok;
	Verdict		v;

	if (t->sched_len == 0)
		return false;
	tok = sched_pop(t);
	v = tok_verdict(t, &tok);
	if (v == V_CANNOT_KEY0)
	{
		count_pruned(t, &tok);
		for (uint32 i = 0; i < t->sched_len; i++)
			count_pruned(t, &t->sched[i]);
		t->sched_len = 0;
		t->st.early_terminated = true;
		return false;
	}
	if (v == V_CANNOT)
	{
		count_pruned(t, &tok);
		return true;
	}
	switch (tok.kind)
	{
		case STOK_PARTITION:
		case STOK_PART:
			ops->expand(src, t, &tok);
			break;
		case STOK_KEY_FRONTIER:
			ops->frontier_next(src, t, &tok);
			sift_fallthrough;
		case STOK_GRANULE:
			t->st.granules_scanned++;
			sift_fallthrough;
		default:
			ops->scan(src, t, &tok);
	}
	return true;
}

void
sift_tourn_run(SiftTournament *t, const SiftTournOps *ops, void *src)
{
	while (sift_tourn_step(t, ops, src))
		;
}
