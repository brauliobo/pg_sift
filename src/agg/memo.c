/*
 * agg/memo.c - the rows of a correlated child by the values of its parameters, shared by the processes of a parallel scan
 *
 * Every process of a scan meets nearly all the distinct inputs of a correlated join (a sub-select run for each distinct combination of
 * the leaves it reads), and each ran the child for all of them: the child's index search was paid by every process, whatever the number of
 * workers. The first process to run the child for a set of parameter values stores its rows here (as minimal tuples) and the others read
 * them. The child is deterministic for the snapshot of the scan, so the rows are those the process would have got.
 *
 * One writer reserves room in the arena with an atomic add, fills its entry, and publishes it by storing its offset in a free slot of the
 * table; a reader that finds a slot set reads a complete entry. A full table or arena only stops the memoizing: the process runs the child.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "port/atomics.h"

#include "agg/agg.h"
#include "agg/agg_int.h"
#include "agg/memo.h"

#define MEMO_SLOTS	(1 << 17)
#define MEMO_ARENA	((Size) 32 << 20)

typedef struct ChildMemo
{
	pg_atomic_uint64 top;		/* the next free byte of the arena */
	pg_atomic_uint32 n;			/* entries published */
	pg_atomic_uint64 slot[MEMO_SLOTS];	/* 1 + the offset of the entry in the arena, 0: free */
} ChildMemo;

typedef struct MemoEnt
{
	uint32		hash;
	uint32		klen;
	uint32		nrows;
	uint32		pad;
} MemoEnt;

#define MEMO_ARENA_AT(m)	((char *) (m) + MAXALIGN(sizeof(ChildMemo)))

Size
memo_size(void)
{
	return MAXALIGN(sizeof(ChildMemo)) + MEMO_ARENA;
}

void
memo_init(ChildMemo *m)
{
	pg_atomic_init_u64(&m->top, 0);
	pg_atomic_init_u32(&m->n, 0);
	for (int i = 0; i < MEMO_SLOTS; i++)
		pg_atomic_init_u64(&m->slot[i], 0);
}

const char *
memo_find(ChildMemo *m, const char *key, uint32 klen, uint32 *nrows)
{
	uint32		hash = hash_bytes((const unsigned char *) key, klen);

	for (uint32 i = hash & (MEMO_SLOTS - 1);; i = (i + 1) & (MEMO_SLOTS - 1))
	{
		uint64		s = pg_atomic_read_u64(&m->slot[i]);
		const MemoEnt *e;

		if (s == 0)
			return NULL;
		pg_read_barrier();
		e = (const MemoEnt *) (MEMO_ARENA_AT(m) + s - 1);
		if (e->hash == hash && e->klen == klen && memcmp(e + 1, key, klen) == 0)
		{
			*nrows = e->nrows;
			return (const char *) e + sizeof(MemoEnt) + MAXALIGN(klen);
		}
	}
}

/* a row of an entry: its minimal tuple, after its length */
const char *
memo_row(const char *at, MinimalTuple *mt)
{
	uint32		len = *(const uint32 *) at;

	*mt = (MinimalTuple) (at + 8);
	return at + 8 + MAXALIGN(len);
}

void
memo_put(ChildMemo *m, const char *key, uint32 klen, int nrows, MinimalTuple *rows)
{
	uint32		hash = hash_bytes((const unsigned char *) key, klen);
	Size		size = sizeof(MemoEnt) + MAXALIGN(klen);
	uint64		off;
	char	   *at;
	MemoEnt    *e;

	for (int r = 0; r < nrows; r++)
		size += 8 + MAXALIGN(rows[r]->t_len);
	if (pg_atomic_read_u32(&m->n) >= MEMO_SLOTS / 2 || pg_atomic_read_u64(&m->top) + size > MEMO_ARENA)
		return;
	off = pg_atomic_fetch_add_u64(&m->top, size);
	if (off + size > MEMO_ARENA)
		return;
	e = (MemoEnt *) (MEMO_ARENA_AT(m) + off);
	e->hash = hash;
	e->klen = klen;
	e->nrows = nrows;
	memcpy(e + 1, key, klen);
	at = (char *) e + sizeof(MemoEnt) + MAXALIGN(klen);
	for (int r = 0; r < nrows; r++)
	{
		*(uint32 *) at = rows[r]->t_len;
		memcpy(at + 8, rows[r], rows[r]->t_len);
		at += 8 + MAXALIGN(rows[r]->t_len);
	}
	pg_write_barrier();
	for (uint32 i = hash & (MEMO_SLOTS - 1);; i = (i + 1) & (MEMO_SLOTS - 1))
	{
		uint64		expected = 0;

		if (pg_atomic_compare_exchange_u64(&m->slot[i], &expected, off + 1))
		{
			pg_atomic_fetch_add_u32(&m->n, 1);
			return;
		}
		pg_read_barrier();
		{
			const MemoEnt *o = (const MemoEnt *) (MEMO_ARENA_AT(m) + expected - 1);

			if (o->hash == hash && o->klen == klen && memcmp(o + 1, key, klen) == 0)
				return;			/* another process stored the same rows first */
		}
	}
}
