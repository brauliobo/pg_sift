/*
 * agg/topn/run.c - the top-N reader over the runs of a unit
 * The walk of the runs of the first key: the window, the residual conditions, the seek by a
 * bound, and the rows a unit gives from its runs.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/topn/int.h"

/* Entries of a sorted run are read in windows that double up to this many */
#define RUN_WIN_MIN		32

#define RUN_WIN_MAX		512

/* the candidate buffers of a unit read through its run start this size and grow with the rows taken (the rows of the unit are most of them never candidates) */
#define RUN_CAND_MIN	64

typedef struct RunWalk
{
	SiftTopN   *t;
	SiftSrcSource *src;
	const SiftSrcUnit *unit;
	const SiftSrcVec *key0;		/* the first key of every row, or NULL: the keys of the window's rows are read by position (wkey) */
	SiftSrcVec	wkey;
	const uint64 *rmask;
	uint32		n;				/* rows of the unit: entries of the run */
	uint32		first,
				end;			/* the entries run_values reads: the keys of the others are not read */
	uint32		lo,
				hi;				/* the entries rwin holds */
	uint32		win;
	int			nc;				/* candidate rows in t->cand */
	int			passing;		/* rows that pass in the groups taken so far */
	Rank		bar;
	bool		barred;
	bool		have;			/* prev is the rank of the group before */
	Rank		prev;
	int			budget;			/* rows to take before leaving the rest of the unit to the second pass (0: no limit) */
	bool		resumed;		/* the second pass of this unit: the groups before resume are taken */
	Rank		resume;
	bool		trunc;
	Rank		next;			/* trunc: the rank of the first group left */
	bool		lazy;			/* rmask holds the live rows, not the ones that pass: the window's rows are tested by t->resid (rslot is RSLOT_OUT for those that fail) */
	SiftSrcVec	rv[TOP_RESID];	/* lazy: the leaves of t->resid for the rows of the window that rmask keeps */
	uint32		nfail;			/* lazy: the rows that failed t->resid; abort: so many that the filters of the unit are cheaper */
	bool		abort;
} RunWalk;

/* a lazy walk that finds more than a 32nd of the rows of the unit failing the conditions is left to the filters, which test a row for a few instructions */
#define LAZY_FAIL_DIV	32

#define RSLOT_OUT	0xFFFF

#define RESID_MAYBE	2

#define RUN_SEEK_TAIL	16

static void
run_corrupt(RunWalk *w, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift_col sorted run is corrupted"),
			 errdetail("The sorted run of the first sort column of a unit of %u rows %s.", w->n, what)));
}

static inline bool
run_pass(const RunWalk *w, uint32 row)
{
	return w->rmask[row >> 6] >> (row & 63) & 1;
}

/* the value of a leaf of a condition at a row of the window: ki its place in the read of the key, oi in that of the other leaves; false for NULL */
static bool
resid_value(const RunWalk *w, int leaf, int ki, int oi, int64 *out)
{
	const SiftTopN *t = w->t;
	int			j = 0,
				i = leaf == t->key[0].leaf ? ki : oi;

	while (t->rleaf[j] != leaf)
		j++;
	if (w->rv[j].isnull && w->rv[j].isnull[i])
		return false;
	*out = t->rwide[j] ? ((const int64 *) w->rv[j].data)[i] : ((const int32 *) w->rv[j].data)[i];
	return true;
}

/* Does the row pass the conditions: a condition on a column other than the key is not known (RESID_MAYBE) until that column is read (oi >= 0) */
static int
resid_test(const RunWalk *w, const SiftTopGPred *p, int ki, int oi)
{
	bool		unknown = false;
	int64		v;

	if (p->kind == STG_NOTNULL || p->kind == STG_CMP)
	{
		if (oi < 0 && p->leaf != w->t->key[0].leaf)
			return RESID_MAYBE;
		return resid_value(w, p->leaf, ki, oi, &v) && (p->kind == STG_NOTNULL || gcond_cmp(p->op, v, p->val));
	}
	for (int c = 0; c < p->nchild; c++)
	{
		int			r = resid_test(w, p->child[c], ki, oi);

		if (r == (p->kind == STG_OR))
			return r;
		unknown |= r == RESID_MAYBE;
	}
	return unknown ? RESID_MAYBE : p->kind == STG_AND;
}

/* The rows of the window that fail the conditions leave it (rslot): the key decides most, and the other columns are read for the rows it does not */
static void
run_resid(RunWalk *w, uint32 cnt)
{
	SiftTopN   *t = w->t;
	int			k = 0;

	for (int j = 0; j < t->nresid; j++)
		if (t->rleaf[j] == t->key[0].leaf)
			w->rv[j] = w->wkey;
	for (uint32 i = 0; i < cnt; i++)
	{
		int			r;

		if (t->rslot[i] == RSLOT_OUT)
			continue;
		r = resid_test(w, t->resid, t->rslot[i], -1);
		if (r == RESID_MAYBE)
		{
			t->rmrow[k] = t->rpass[t->rslot[i]];
			t->rmay[k++] = i;
		}
		else if (!r)
		{
			t->rslot[i] = RSLOT_OUT;
			w->nfail++;
		}
	}
	if (k > 0)
	{
		for (int j = 0; j < t->nresid; j++)
			if (t->rleaf[j] != t->key[0].leaf)
				sift_colsrc_read_rows(w->src, w->unit, t->rleaf[j], t->rmrow, k, &w->rv[j]);
		for (int q = 0; q < k; q++)
			if (!resid_test(w, t->resid, t->rslot[t->rmay[q]], q))
			{
				t->rslot[t->rmay[q]] = RSLOT_OUT;
				w->nfail++;
			}
	}
	w->abort = w->nfail > Max(w->n / LAZY_FAIL_DIV, 64);
}

/* The row at position pos of the run; the window is read toward the direction of the walk */
static uint32
run_at(RunWalk *w, uint32 pos, bool back)
{
	if (pos < w->lo || pos >= w->hi)
	{
		uint32		cnt = back ? Min(w->win, pos + 1) : Min(w->win, w->n - pos),
					lo = back ? pos + 1 - cnt : pos;

		sift_colsrc_run_read(w->src, w->unit, w->t->key[0].leaf, lo, cnt, w->t->rwin);
		if (w->key0 == NULL)
		{
			/* a key is read for a row that passes only: the walk skips the others, and reading one by position costs ten times what decoding it with its unit does */
			int			m = 0;

			for (uint32 i = 0; i < cnt; i++)
			{
				w->t->rslot[i] = lo + i >= w->first && lo + i < w->end && run_pass(w, w->t->rwin[i]) ? m : RSLOT_OUT;
				if (w->t->rslot[i] != RSLOT_OUT)
					w->t->rpass[m++] = w->t->rwin[i];
			}
			if (m > 0)
				sift_colsrc_read_rows(w->src, w->unit, w->t->key[0].leaf, w->t->rpass, m, &w->wkey);
			if (w->lazy && m > 0)
				run_resid(w, cnt);
		}
		w->lo = lo;
		w->hi = lo + cnt;
		w->win = Min(w->win * 2, RUN_WIN_MAX);
	}
	return w->t->rwin[pos - w->lo];
}

static void
run_key(RunWalk *w, uint32 pos, uint32 row, int64 *v)
{
	if (!(w->key0 ? row_key(&w->t->key[0], w->key0, row, v) : row_key(&w->t->key[0], &w->wkey, w->t->rslot[pos - w->lo], v)))
		run_corrupt(w, "has a NULL row among those with a value");
}

/* A group of equal first keys, rank, begins: false when the first pass took it. The ranks must come in order */
static bool
run_open(RunWalk *w, Rank rank)
{
	if (w->have && !rank_after(rank, w->prev))
		run_corrupt(w, "is not in the order of its keys");
	w->prev = rank;
	w->have = true;
	return !(w->resumed && rank_after(w->resume, rank));
}

/* The group ends; its rows that pass are t->cand[g0, nc), backwards when the walk was. With one sort key the tied rows after the n-th come after it by TID and are not in the result */
static void
run_close(RunWalk *w, int g0, Rank rank, bool back)
{
	SiftTopN   *t = w->t;

	for (int a = g0, b = w->nc - 1; back && a < b; a++, b--)
	{
		uint32		x = t->cand[a];

		t->cand[a] = t->cand[b];
		t->cand[b] = x;
	}
	if (t->nkey == 1)
		w->nc = Min(w->nc, g0 + t->limit - w->passing);
	w->passing += w->nc - g0;
	if (w->passing >= t->limit && (!w->barred || rank_after(w->bar, rank)))
	{
		w->bar = rank;
		w->barred = true;
	}
}

/* Whether the rows of the group at hand reach the n-th, so that the rest of an ascending group is out */
static bool
run_full(const RunWalk *w, int g0)
{
	return w->t->nkey == 1 && w->passing + w->nc - g0 >= w->t->limit;
}

static inline void
run_room(RunWalk *w)
{
	if (w->nc >= w->t->ccap)
		grow_unit_buffers(w->t, w->nc + 1);
}

static bool
run_stop(const RunWalk *w, Rank r)
{
	return w->barred && rank_after(r, w->bar);
}

/* The first pass has its rows: the group of rank r and the ones after are left to the second pass, which the heap's bar may spare */
static bool
run_cut(RunWalk *w, Rank r)
{
	if (w->budget == 0 || w->passing < w->budget)
		return false;
	w->trunc = true;
	w->next = r;
	return true;
}

/* The rows of the NULL group (positions [nvals, nvals + nnull)) */
static void
run_nulls(RunWalk *w, uint32 nvals, uint32 nnull)
{
	Rank		r = rank_of(&w->t->key[0], true, 0);
	int			g0 = w->nc;
	uint32		prev = 0;

	if (nnull == 0 || run_stop(w, r) || !run_open(w, r))
		return;
	for (uint32 pos = nvals; pos < nvals + nnull; pos++)
	{
		uint32		row = run_at(w, pos, false);

		if (pos > nvals && row <= prev)
			run_corrupt(w, "has the rows of equal keys out of order");
		prev = row;
		if (!run_pass(w, row))
			continue;
		run_room(w);
		w->t->cand[w->nc++] = row;
		if (run_full(w, g0))
			break;
	}
	run_close(w, g0, r, false);
}

/* The keys of the entries [pos, pos + cnt) of the run (positions among the values): from key0 when the unit has it whole, which a read of rows would overwrite */
static void
run_keys_of(RunWalk *w, uint32 pos, uint32 cnt, int64 *keys)
{
	const TopKey *k0 = &w->t->key[0];
	uint32		rows[RUN_SEEK_TAIL];
	SiftSrcVec	v = {0};

	sift_colsrc_run_read(w->src, w->unit, k0->leaf, pos, cnt, rows);
	if (!w->key0)
		sift_colsrc_read_rows(w->src, w->unit, k0->leaf, rows, cnt, &v);
	for (uint32 i = 0; i < cnt; i++)
		if (!(w->key0 ? row_key(k0, w->key0, rows[i], &keys[i]) : row_key(k0, &v, i, &keys[i])))
			run_corrupt(w, "has a NULL row among those with a value");
}

/*
 * The first position of the run [0, nvals) whose key is after bound (at bound itself when incl). A key costs the opening of its chunk, and the zone of the unit
 * bounds the keys: the ends answer outright, and the next probe goes where the keys, spread evenly between the ends known so far, would put the bound; a probe
 * that does not halve what is left is followed by one at the middle, so that keys that are not spread evenly cost twice a bisection and not more. The last entries
 * are read together.
 */
static uint32
run_seek(RunWalk *w, uint32 nvals, int64 bound, bool incl)
{
	SiftSrcZone z;
	uint32		lo = 0,
				hi = nvals;
	int64		kl = 0,
				kh = 0;
	bool		even,
				mid = false;

	sift_colsrc_zone(w->src, w->unit, w->t->key[0].leaf, &z);
	even = !z.inexact && z.nvals == nvals;
	if (even)
	{
		if (incl ? z.min > bound : z.min >= bound)
			return 0;
		if (!(incl ? z.max > bound : z.max >= bound))
			return nvals;
		kl = z.min;
		kh = z.max;
	}
	while (lo < hi)
	{
		uint32		left = hi - lo,
					m = lo + left / 2;
		int64		keys[RUN_SEEK_TAIL];

		if (left <= RUN_SEEK_TAIL)
		{
			run_keys_of(w, lo, left, keys);
			for (m = 0; m < left; m++)
				if (incl ? keys[m] > bound : keys[m] >= bound)
					break;
			return lo + m;
		}
		if (even && !mid && kh > kl)
			m = lo + (uint32) ((double) (left - 1) * Min(Max(((double) bound - (double) kl) / ((double) kh - (double) kl), 0.0), 1.0));
		run_keys_of(w, m, 1, keys);
		if (incl ? keys[0] > bound : keys[0] >= bound)
		{
			hi = m;
			kh = keys[0];
		}
		else
		{
			lo = m + 1;
			kl = keys[0];
		}
		mid = (hi - lo) * 2 > left;
	}
	return lo;
}

/*
 * The rows with a value, best group first, until a group cannot be in the result. A row is tested against the mask before its key is read, so the
 * keys of the rows that do not pass (zero in a vector read for the rows that pass only) are never looked at; a group is the rows that pass with an
 * equal key. The run holds equal keys by row ascending: a descending walk meets them backwards and puts them right at the end of the group.
 */
static void
run_values(RunWalk *w, uint32 first, uint32 end)
{
	const TopKey *k0 = &w->t->key[0];
	bool		desc = k0->desc,
				open = false,
				skip = false;
	int			g0 = 0;
	int64		gv = 0;
	uint32		prev = 0;
	Rank		r = {0};

	for (uint32 i = first; i < end; i++)
	{
		uint32		pos = desc ? end - 1 - (i - first) : i,
					row = run_at(w, pos, desc);
		int64		v;

		if (w->abort)
			return;
		if (!run_pass(w, row) || (w->lazy && w->t->rslot[pos - w->lo] == RSLOT_OUT))
			continue;
		run_key(w, pos, row, &v);
		if (open && v == gv)
		{
			if (desc ? row >= prev : row <= prev)
				run_corrupt(w, "has the rows of equal keys out of order");
		}
		else
		{
			if (open && !skip)
				run_close(w, g0, r, desc);
			r = rank_of(k0, false, v);
			if (run_stop(w, r) || run_cut(w, r))
				return;
			open = true;
			skip = !run_open(w, r);
			g0 = w->nc;
			gv = v;
		}
		prev = row;
		if (skip)
			continue;
		run_room(w);
		w->t->cand[w->nc++] = row;
		if (!desc && run_full(w, g0))
			break;
	}
	if (open && !skip)
		run_close(w, g0, r, desc);
}

/* Has the unit a run the walk can read: the run orders the column, not the expression of it (coalesce moves the NULLs, lower() folds the strings) */
bool
run_ready(const SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	const TopKey *k0 = &t->key[0];

	return sift_topn_runs && k0->xk == TXK_NONE && k0->leaf >= 0 && sift_colsrc_run_info(src, unit, k0->leaf, true, nvals, nnull, id);
}

/*
 * The unit from its sorted run: the groups of equal first keys in the order of the sort, from the best, until one cannot be in the
 * result, where the rows are read and tested one by one otherwise. The candidates are what the other loop finds: the rows that pass whose first key is
 * not after the n-th of the unit's or the heap's, so the rest is the same. False: the unit has no run, or the rows that pass are too few for the walk
 * to pay (it reads about n / share of the rows that pass) and the heap has no bar yet.
 */
bool
run_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow, bool lazy)
{
	RunWalk		w = {.t = t, .src = src, .unit = unit, .key0 = key0, .rmask = rmask, .n = nrow, .end = nrow, .win = RUN_WIN_MIN, .lazy = lazy};
	uint32		nvals,
				nnull,
				first,
				end,
				npass = 0;
	uint64		id;
	int			m = 0;
	bool		bounded;
	const TopKey *k0 = &t->key[0];
	RunTrunc   *rt = NULL;
	SiftSrcVec	cv = {0};

	if (!run_ready(t, src, unit, &nvals, &nnull, &id))
		return false;
	if (t->revisit)
	{
		/* the units the first pass finished, or took no run of, are not read again */
		for (int i = 0; i < t->ntrunc && rt == NULL; i++)
			if (t->trunc[i].id == id && !t->trunc[i].done)
				rt = &t->trunc[i];
		if (rt == NULL)
			return true;
		w.resumed = true;
		w.resume = rt->next;
	}
	else
	{
		for (int i = 0; i < (nrow + 63) / 64 && !lazy; i++)
			npass += pg_popcount64(rmask[i]);
		/* a unit with fewer rows than the walk would take for its own bar is read through the run only when the heap's bar already cuts it (a lazy unit is not counted: the rows that pass are what the walk finds) */
		if (!lazy && (uint64) (t->limit + 1) * 2 > npass && !bar_rank(t, &w.bar))
			return false;
		w.budget = t->budget;
	}
	grow_unit_buffers(t, Min(nrow, RUN_CAND_MIN));
	if (t->rwin == NULL)
	{
		t->rwin = MemoryContextAlloc(GetMemoryChunkContext(t), RUN_WIN_MAX * (3 * sizeof(uint32) + 2 * sizeof(uint16)));
		t->rpass = t->rwin + RUN_WIN_MAX;
		t->rmrow = t->rpass + RUN_WIN_MAX;
		t->rslot = (uint16 *) (t->rmrow + RUN_WIN_MAX);
		t->rmay = t->rslot + RUN_WIN_MAX;
	}
	w.barred = bar_rank(t, &w.bar);
	if (rt && run_stop(&w, rt->next))
	{
		rt->done = true;
		return true;
	}
	first = 0;
	end = nvals;
	/* the conditions on the key bound the part of the run that can pass: the rows outside it, and the NULLs, fail them */
	bounded = t->kbox.has_lo || t->kbox.has_hi;
	if (bounded)
	{
		first = t->kbox.has_lo ? run_seek(&w, nvals, t->kbox.lo, false) : 0;
		end = t->kbox.has_hi ? run_seek(&w, nvals, t->kbox.hi, true) : nvals;
	}
	if (k0->nulls_first && !bounded)
		run_nulls(&w, nvals, nnull);
	w.first = first;
	w.end = end;
	run_values(&w, first, end);
	if (w.abort)
		return false;
	if (rt)
		rt->done = true;
	if (!k0->nulls_first && !w.trunc && !bounded)
		run_nulls(&w, nvals, nnull);
	/* the rows whose value overflowed the leaf are tested one by one: the run does not order them */
	for (uint32 pos = nvals + nnull; pos < (uint32) nrow && !w.resumed; pos++)
	{
		uint32		row = run_at(&w, pos, false);

		if (rmask[row >> 6] >> (row & 63) & 1)
		{
			run_room(&w);
			t->cand[w.nc++] = row;
		}
	}
	sort_rows(t->cand, w.nc);
	if (key0 == NULL && w.nc > 0)
		sift_colsrc_read_rows(src, unit, k0->leaf, t->cand, w.nc, &cv);
	for (int q = 0; q < w.nc; q++)
	{
		TopEnt	   *e = &t->ce[m];
		int64		v = 0;
		bool		isnull = !(key0 ? row_key(k0, key0, t->cand[q], &v) : row_key(k0, &cv, q, &v));

		if (q > 0 && t->cand[q] == t->cand[q - 1])
			run_corrupt(&w, "names a row twice");
		if (w.barred && rank_after(rank_of(k0, isnull, v), w.bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->k[0] = v;
		e->nul = isnull;
		e->row = e->tie = t->cand[q];
		t->cand[m++] = e->row;
	}
	t->nrun++;
	t->nlazy += lazy;
	if (w.trunc)
	{
		if (t->ntrunc == t->captrunc)
		{
			t->captrunc = Max(16, 2 * t->captrunc);
			t->trunc = grow(t, t->trunc, t->captrunc * sizeof(RunTrunc));
		}
		t->trunc[t->ntrunc++] = (RunTrunc) {id, unit->sb, w.next, false};
	}
	if (m > 0)
		finish_unit(t, src, unit, m);
	return true;
}

/* Can the heads of the runs of the first key answer: one integer key, no expression, no skipped or reordered input */
bool
heads_ok(const SiftTopN *t)
{
	const TopKey *k0 = &t->key[0];

	return sift_topn_heads && sift_topn_runs && t->nkey == 1 && t->nexpr == 0 && !t->all && !t->sort && !t->revisit && k0->xk == TXK_NONE && k0->leaf >= 0 && !k0->blob &&
		!k0->nenum && (k0->vtype == SCV_I32 || k0->vtype == SCV_I64) && t->limit > 0;
}
