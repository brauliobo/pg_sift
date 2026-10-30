/*
 * agg/dist/convert.c - count(DISTINCT): changing the mode of a set
 * The walk of a set, its conversion between bitmap, hash and sort modes, and the chunks and
 * compaction that make room.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "miscadmin.h"
#include "utils/tuplesort.h"

#include "agg/dist/int.h"

typedef struct DistMove
{
	AggExec    *ex;
	Dist	   *to;
} DistMove;

/* every pair of a bitmap or hash structure (not a sort), through fn */
void
dist_walk(const Dist *d, DistFn fn, void *arg)
{
	if (d->mode == DD_WORDS)
	{
		for (uint32 g = 0; g < d->nng; g++)
			for (uint32 w = 0; w < d->nk; w++)
				for (uint64 m = d->nw[(size_t) g * d->nk + w]; m; m &= m - 1)
					fn(arg, g, (uint32) (d->nlo + w * 64 + pg_rightmost_one_pos64(m)));
	}
	else if (d->mode == DD_BITS)
	{
		for (uint32 g = 0; g < d->ngd; g++)
		{
			DistChunk  *dir = d->gd[g];

			for (uint32 c = 0; dir && c < d->ncd; c++)
			{
				if (!dir[c])
					continue;
				for (int w = 0; w < DB_WORDS; w++)
					for (uint64 m = dir[c][w]; m; m &= m - 1)
					{
						int64		x = (d->cbase + c) * (1 << DB_SHIFT) + w * 64 + pg_rightmost_one_pos64(m);

						fn(arg, g, (uint32) x);
					}
			}
		}
	}
	else if (d->mode == DD_HASH)
	{
		for (uint64 i = 0; i < d->ds.cap; i++)
			if (d->ds.slot[i])
				fn(arg, (d->ds.slot[i] >> 32) & 0x7fffffff, (uint32) d->ds.slot[i]);
	}
}

static void
dist_free_bits(Dist *d)
{
	for (uint32 g = 0; g < d->ngd; g++)
		if (d->gd[g])
			pfree(d->gd[g]);
	if (d->gd)
		pfree(d->gd);
	while (d->arena)
	{
		uint64	   *prev = (uint64 *) d->arena[0];

		pfree(d->arena);
		d->arena = prev;
	}
}

static void
dist_move(void *arg, uint32 g, uint32 v)
{
	DistMove   *mv = arg;

	(void) dist_add(mv->ex, mv->to, g, v);
}

/* a tuplesort of (int4, int8) rows ordered by both, and the slots that put rows in and read them back; a third int4 column rides along unsorted. In the current memory context */
void
agg_pair_sort(int natts, int flags, Tuplesortstate **sort, TupleTableSlot **put, TupleTableSlot **get)
{
	TupleDesc	desc = CreateTemplateTupleDesc(natts);
	AttrNumber	att[2] = {1, 2};
	Oid			ops[2] = {Int4LessOperator, Int8LessOperator},
				coll[2] = {InvalidOid, InvalidOid};
	bool		nulls[2] = {false, false};

	TupleDescInitEntry(desc, 1, "a", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "b", INT8OID, -1, 0);
	if (natts > 2)
		TupleDescInitEntry(desc, 3, "c", INT4OID, -1, 0);
	sift_tupdesc_finalize(desc);
	*put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	*get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	*sort = tuplesort_begin_heap(desc, 2, att, ops, coll, nulls, work_mem, NULL, flags);
}

/* the structure of d moves to mode `to`, its pairs added to the new one */
void
dist_convert(AggExec *ex, Dist *d, DistMode to)
{
	Dist		old = *d;
	DistMove	mv = {ex, d};

	if (to == DD_SORT)
	{
		MemoryContext mc = MemoryContextSwitchTo(ex->cxt);

		d->sort = tuplesort_begin_datum(INT8OID, Int8LessOperator, InvalidOid, false, work_mem, NULL, TUPLESORT_RANDOMACCESS);
		MemoryContextSwitchTo(mc);
		d->nsid = d->nsidcap = d->sidgroups = 0;
		d->sid = NULL;
		ex->n_dist_spilled++;
	}
	memset(&d->ds, 0, sizeof(DSet));
	d->mode = to;
	d->nw = NULL;
	d->nk = d->nng = 0;
	d->gd = NULL;
	d->ngd = d->ncd = 0;
	d->arena = NULL;
	d->arena_left = 0;
	d->bytes = 0;
	d->maxn = Max(d->budget / (4 * sizeof(uint64)), 256);	/* the table is at most half full, and a power of 2 */
	/* a window is aligned to 64 values: its words go into the chunks as words, not a bit at a time */
	if (old.mode == DD_WORDS && to == DD_BITS)
		for (uint32 g = 0; g < old.nng; g++)
			(void) dist_or_words(ex, d, g, old.nlo / 64, (const uint8 *) (old.nw + (size_t) g * old.nk), old.nk);
	else
		dist_walk(&old, dist_move, &mv);
	if (old.mode == DD_BITS)
		dist_free_bits(&old);
	else if (old.nw)
		pfree(old.nw);
	else if (old.ds.slot)
		pfree(old.ds.slot);
}

/* a bitmap chunk of 4096 zero bits */
uint64 *
dist_chunk(AggExec *ex, Dist *d)
{
	if (d->freec)
	{
		uint64	   *c = d->freec;

		d->freec = ((uint64 **) c)[0];
		memset(c, 0, DB_WORDS * sizeof(uint64));
		return c;
	}
	if (d->arena_left == 0)
	{
		uint64	   *blk = MemoryContextAllocZero(ex->tcxt, (DB_ARENA + 1) * DB_WORDS * sizeof(uint64));

		blk[0] = (uint64) d->arena;
		d->arena = blk;
		d->arena_left = DB_ARENA;
	}
	return d->arena + (size_t) (DB_ARENA - --d->arena_left) * DB_WORDS;
}

/*
 * A streaming scan (dist_stream_scan) promised that no value below d->wmark is added again, and the count of a group was taken when its bit was
 * set, so the bits below are dead weight: the chunks of the windows wholly below the mark go to the free list and the directories slide up.
 * True when the base moved, so that the caller tries again before it gives the bitmap up.
 */
bool
dist_compact(Dist *d)
{
	int64		lo = (d->wmark >> DB_SHIFT) & ~(int64) (DB_NCD_MIN / 2 - 1);
	uint32		shift;

	if (d->mode != DD_BITS || d->ncd == 0 || lo <= d->cbase)
		return false;
	shift = (uint32) Min(lo - d->cbase, (int64) d->ncd);
	for (uint32 g = 0; g < d->ngd; g++)
	{
		DistChunk  *dir = d->gd[g];

		if (dir == NULL)
			continue;
		for (uint32 c = 0; c < shift; c++)
			if (dir[c])
			{
				((uint64 **) dir[c])[0] = d->freec;
				d->freec = dir[c];
				d->bytes -= DB_WORDS * sizeof(uint64);
			}
		memmove(dir, dir + shift, (d->ncd - shift) * sizeof(DistChunk));
		memset(dir + (d->ncd - shift), 0, shift * sizeof(DistChunk));
	}
	d->cbase += shift;
	return true;
}

/* the same for a hash set: the pairs below the mark are left out of a rebuilt table; false when they are too few to be worth it */
bool
dist_compact_hash(AggExec *ex, Dist *d)
{
	DSet	   *s = &d->ds;
	uint64	   *old = s->slot;
	uint64		dead = 0;

	if (d->wmark == PG_INT64_MIN || s->n == 0)
		return false;
	for (uint64 i = 0; i < s->cap; i++)
		dead += old[i] && dist_x(d, (uint32) old[i]) < d->wmark;
	if (dead < s->n / 8 + 1)
		return false;
	s->slot = MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(uint64));
	s->n = 0;
	for (uint64 i = 0; i < s->cap; i++)
		if (old[i] && dist_x(d, (uint32) old[i]) >= d->wmark)
			dset_insert_raw(s, old[i]);
	pfree(old);
	return true;
}
