/*
 * core/tomb_iter.c - the passes over the tombstone log of a backend (core/tomb.h): the entries of one key, every entry in no order as arrays, and every entry in (key, seq) order, the ones
 * past the memory of the log read back from its runs
 */
#include "postgres.h"

#include "core/tomb.h"
#include "core/tomb_dv.h"
#include "core/tomb_int.h"

static void
key_enter(SiftTombKeyWalk *w)
{
	const SiftTombLog *t = w->t;

	w->arr = NULL;
	w->n = 0;
	if (w->stage < t->spilled.nruns)
		sift_srun_cur_seek(&w->cur, &t->spilled.runs[w->stage], w->key);
	else if (w->stage == t->spilled.nruns)
		w->arr = tomb_find_run(t->ents, t->nents, w->key, &w->n);
	else
		w->arr = tomb_find_run(t->recent, t->nrecent, w->key, &w->n);
}

void
sift_tomb_key_begin(SiftTombKeyWalk *w, const SiftTombLog *t, uint64 key, MemoryContext cxt)
{
	*w = (SiftTombKeyWalk) {.t = t,.key = key};
	if (t->spilled.nruns > 0)
		sift_srun_cur_init(&w->cur, NULL, sizeof(SiftTomb), cxt);
	key_enter(w);
}

const SiftTomb *
sift_tomb_key_next(SiftTombKeyWalk *w)
{
	for (;;)
	{
		if (w->stage < w->t->spilled.nruns)
		{
			const SiftTomb *rec;

			while ((rec = sift_srun_cur_peek(&w->cur)) != NULL && rec->key < w->key)
				sift_srun_cur_next(&w->cur);
			if (rec != NULL && rec->key == w->key)
			{
				sift_srun_cur_next(&w->cur);
				return rec;
			}
		}
		else if (w->n > 0)
		{
			w->n--;
			return w->arr++;
		}
		if (w->stage > w->t->spilled.nruns)
			return NULL;
		w->stage++;
		key_enter(w);
	}
}

void
sift_tomb_key_end(SiftTombKeyWalk *w)
{
	if (w->t->spilled.nruns > 0)
		pfree(w->cur.blk);
}

/* The entries of the log in (key, seq) order: the runs and the array of the log merged, equal keys oldest source first */
void
sift_tomb_walk_begin(SiftTombWalk *w, SiftTombLog *t)
{
	tomb_flatten(t);
	*w = (SiftTombWalk) {.t = t};
	w->cur = palloc(Max(1, t->spilled.nruns) * sizeof(SiftSortRunCur));
	for (uint32 r = 0; r < t->spilled.nruns; r++)
		sift_srun_cur_init(&w->cur[r], &t->spilled.runs[r], sizeof(SiftTomb), CurrentMemoryContext);
}

/* The next entry, NULL at the end: one of the array in place (keep may rewrite it), or a copy of one of a run */
SiftTomb *
sift_tomb_walk_next(SiftTombWalk *w)
{
	const SiftTombLog *t = w->t;
	int			from = -1;
	uint64		key = PG_UINT64_MAX;
	const SiftTomb *rec;

	for (uint32 r = 0; r < t->spilled.nruns; r++)
		if ((rec = sift_srun_cur_peek(&w->cur[r])) != NULL && (from < 0 || rec->key < key))
			from = r, key = rec->key;
	if (w->i < t->nents && (from < 0 || t->ents[w->i].key < key))
		return &w->t->ents[w->i++];
	if (from < 0)
		return NULL;
	w->copy = *(const SiftTomb *) sift_srun_cur_peek(&w->cur[from]);
	sift_srun_cur_next(&w->cur[from]);
	return &w->copy;
}

SiftTomb *
sift_tomb_walk_next_rec(SiftTombWalk *w, SiftTomb *buf, uint32 *n)
{
	SiftTomb   *e = sift_tomb_walk_next(w);
	uint32		want;

	*n = 1;
	if (e == NULL || e->kind != SIFT_TOMB_DVEC)
		return e;
	buf[0] = *e;
	e = sift_tomb_walk_next(w);
	SIFT_INVARIANT(e != NULL && e->kind == SIFT_TOMB_DVEC_BODY, "sift: a deletion record of the tombstone log is cut short");
	buf[1] = *e;
	want = sift_dv_slots_of(buf, SIFT_TOMB_CAP);
	SIFT_INVARIANT(want != 0, "sift: a deletion record of the tombstone log is damaged");
	for (*n = 2; *n < want; (*n)++)
	{
		e = sift_tomb_walk_next(w);
		SIFT_INVARIANT(e != NULL && e->kind == SIFT_TOMB_DVEC_BODY, "sift: a deletion record of the tombstone log is cut short");
		buf[*n] = *e;
	}
	return buf;
}

void
sift_tomb_walk_end(SiftTombWalk *w)
{
	for (uint32 r = 0; r < w->t->spilled.nruns; r++)
		pfree(w->cur[r].blk);
	pfree(w->cur);
}

void
sift_tomb_each_begin(SiftTombEach *e, const SiftTombLog *t, MemoryContext cxt)
{
	*e = (SiftTombEach) {.t = t};
	if (t->spilled.nruns > 0)
		sift_srun_cur_init(&e->cur, NULL, sizeof(SiftTomb), cxt);
}

const SiftTomb *
sift_tomb_each_next(SiftTombEach *e, uint32 *n)
{
	const SiftTombLog *t = e->t;

	if (e->arrays == 0 && (e->arrays = 1) && t->nents > 0)
	{
		*n = t->nents;
		return t->ents;
	}
	if (e->arrays == 1 && (e->arrays = 2) && t->nrecent > 0)
	{
		*n = t->nrecent;
		return t->recent;
	}
	for (; e->run < t->spilled.nruns; e->run++)
	{
		const SiftTomb *a;

		if (e->cur.run != &t->spilled.runs[e->run])
			sift_srun_cur_seek(&e->cur, &t->spilled.runs[e->run], 0);
		if ((a = sift_srun_cur_chunk(&e->cur, n)) != NULL)
			return a;
	}
	return NULL;
}

void
sift_tomb_each_end(SiftTombEach *e)
{
	if (e->t->spilled.nruns > 0)
		pfree(e->cur.blk);
}

bool
sift_tomb_empty(const SiftTombLog *t)
{
	return t->nents + t->nrecent == 0 && t->spilled.nruns == 0;
}

bool
sift_tomb_has_hot(const SiftTombLog *t)
{
	return t->hotm != NULL || t->hotspilled.nruns > 0;
}
