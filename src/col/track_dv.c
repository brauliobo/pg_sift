/*
 * col/track_dv.c - the dead addresses of one statement, written to the tombstone log of a heap index with track_changes: as deletion records (core/tomb_dv.h) for the units that hold
 * enough of them, as marks (one 32-byte entry each) for the rest. The statement's addresses are held (8 bytes each, a million at most) until the statement is done with them, so that they
 * are looked up in the units once and not for each chunk of the probe of the table's unique index.
 *
 * What a statement pays to write records is the lookup of its addresses in the units: DV_ROW_INSTR for each row of the index (every unit is walked), which the marks' fold (col/maint/fold.c) pays
 * once for all the marks it clears. Records are written when the statement has at least as many addresses as pay for the lookup, rows * DV_ROW_INSTR / DV_GAIN_INSTR, and for the units where they
 * are smaller than the marks they replace.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/lmgr.h"
#include "utils/inval.h"
#include "utils/rel.h"

#include "bm25/build.h"
#include "col/am.h"
#include "col/build.h"
#include "col/seg.h"
#include "core/cmp.h"
#include "core/membudget.h"
#include "core/tomb_dv.h"

/*
 * 150,000 rows, statements of 150 to 75,000 scattered rows, the same statement with marks and with records (release build), and the fold of each: (A) 125 units of 1,200 rows, key-cut on a key that is not
 * correlated to the heap; (B) 42 units of 8,192 rows, not key-cut; (C) 32 units of 4,800 rows, key-cut on a key correlated to the heap. The statement with records costs more than with marks by a walk of the
 * pages of every unit against its addresses, 9.2 M instructions for 150 addresses (A: 61 for each row of the index), 1.3 M (B: 8.7) and 3.8 M (C: 25), and by 0.6 k to 9.7 k for each address found; the fold
 * of the marks it did not write costs 17 M (A), 3 M (B) and 3.5 M (C) for 750 marks and 1.1 k for each mark more, where the fold of the record costs 0.1 k for each row. Over the statement and its fold the two
 * cost the same at 750 addresses in each fixture (-1.3 M, +0.3 M, +1.0 M) and records cost less above: DV_GAIN_INSTR is what an address gains in that exchange at the break-even of 0.5% of the rows.
 */
#define DV_ROW_INSTR		31
#define DV_GAIN_INSTR		6200
#define DV_MIN_ROWS			64
#define DV_KEYS_MAX			(1 << 20)
#define MARKS_CHUNK			8192

struct SiftColDead
{
	Relation	index;
	SiftTombLog *log;
	MemoryContext cxt;
	TransactionId xid;
	CommandId	cid;
	uint64		index_rows;			/* the rows of the segments, which a lookup reads */
	uint64	   *keys;
	uint32		n;
	uint32		cap;
};

SiftColDead *
sift_col_dead_begin(Relation index, SiftTombLog *log)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift dead addresses", ALLOCSET_DEFAULT_SIZES);
	SiftColDead *d = MemoryContextAllocZero(cxt, sizeof(SiftColDead));
	SiftDirSnapshot *snap;

	d->index = index;
	d->log = log;
	d->cxt = cxt;
	d->xid = GetCurrentTransactionId();
	d->cid = sift_col_stmt_cid();
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
		d->index_rows += snap->segs[i].kind == SIFT_SEG_IMMUTABLE ? snap->segs[i].ndocs : 0;
	pfree(snap);
	return d;
}

static void
append_marks(SiftColDead *d, const uint64 *keys, uint32 n)
{
	SiftTomb   *ents = palloc(Min(n, MARKS_CHUNK) * sizeof(SiftTomb));

	for (uint32 done = 0; done < n;)
	{
		uint32		k = Min(n - done, MARKS_CHUNK);

		for (uint32 i = 0; i < k; i++)
			ents[i] = (SiftTomb) {.key = keys[done + i],.xid = d->xid,.cid = d->cid,.kind = SIFT_TOMB_DELETE};
		sift_tomb_append_many(d->index, d->log, ents, k);
		done += k;
	}
	pfree(ents);
}

typedef struct Found
{
	SiftColDead *d;
	MemoryContext cxt;
	uint64		gen;
	SiftTomb   *slots;
	uint32		nslots;
	uint32		capslots;
	uint64	   *resolved;
	uint32		nres;
} Found;

/* The rows of one unit: records when they take fewer slots than marks, else left for the marks */
static void
found_unit(uint32 u, const uint32 *rows, const SiftKey *at, uint32 nrows, void *arg)
{
	Found	   *f = arg;

	if (sift_dv_count(rows, nrows) >= nrows)
		return;
	(void) sift_dv_encode(&f->slots, &f->nslots, &f->capslots, f->cxt, f->gen, u, f->d->xid, f->d->cid, rows, nrows);
	memcpy(f->resolved + f->nres, at, nrows * sizeof(uint64));
	f->nres += nrows;
}

/* Does an immutable segment of the directory hold copies of the rows (a scope): a record would not end them */
static bool
has_scope(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_SCOPE))
			return true;
	return false;
}

static void
stamp_format(SiftColDead *d)
{
	const SiftSegEntry *e;

	sift_dir_reload(d->index, d->log->dir, d->log->cxt);
	if ((e = sift_tomb_find_seg(*d->log->dir)) != NULL && e->format < SIFT_COL_FORMAT_TOMBDV)
	{
		sift_tomb_set_format(d->index, SIFT_COL_FORMAT_TOMBDV);
		CacheInvalidateRelcache(d->index);
	}
}

/* The keys (ascending) of d that are not in the resolved keys (ascending): the ones left for marks */
static uint64 *
unresolved(const SiftColDead *d, const Found *f, uint32 *n)
{
	uint64	   *rest = MemoryContextAlloc(d->cxt, d->n * sizeof(uint64));
	uint32		j = 0;

	*n = 0;
	for (uint32 i = 0; i < d->n; i++)
	{
		while (j < f->nres && f->resolved[j] < d->keys[i])
			j++;
		if (j == f->nres || f->resolved[j] != d->keys[i])
			rest[(*n)++] = d->keys[i];
	}
	return rest;
}

static void
find_units(SiftColDead *d, const SiftDirSnapshot *snap, Found *f)
{
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(d->index, &snap->segs[i], f->cxt);
		f->gen = snap->segs[i].gen;
		sift_cseg_find_live(seg, (const SiftKey *) d->keys, d->n, found_unit, f);
		sift_cseg_close(seg);
	}
	SIFT_MEM_CHECKPOINT();
}

/*
 * The keys (ascending) that records now end are written; the rest (those of units where marks are smaller, and all of them for an index with a scope) are returned. The tombstone lock is
 * held shared from before the directory is read to find the units of the keys until the records are appended, and the step that replaces segments holds it exclusive from the carry of the
 * records to the switch of the directory (col/maint/dvfold.c): a record is never written for a segment that a switch already replaced, and a step that comes after finds the record and carries it
 * to marks. Held only around the append, the lock would let a delete that found a segment lose to a whole merge and then name the segment that is gone.
 */
static uint64 *
write_records(SiftColDead *d, uint32 *nrest)
{
	SiftDirSnapshot *snap;
	MemoryContext tmp = AllocSetContextCreate(d->cxt, "sift dead addresses lookup", ALLOCSET_DEFAULT_SIZES);
	Found		f = {.d = d,.cxt = tmp};
	uint64	   *rest = d->keys;

	*nrest = d->n;
	f.resolved = MemoryContextAlloc(tmp, d->n * sizeof(uint64));
	LockPage(d->index, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	snap = sift_dir_read(d->index);
	if (!has_scope(snap))
		find_units(d, snap, &f);
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_COL_GATE_DELETE);
#endif
	if (f.nslots > 0)
	{
		stamp_format(d);
		sift_tomb_append_many(d->index, d->log, f.slots, (int) f.nslots);
		sift_sort_u64(f.resolved, f.nres);
		rest = unresolved(d, &f, nrest);
	}
	UnlockPage(d->index, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	MemoryContextDelete(tmp);
	pfree(snap);
	return rest;
}

static uint32
unique_keys(uint64 *keys, uint32 n)
{
	uint32		k = 0;

	for (uint32 i = 0; i < n; i++)
		if (k == 0 || keys[k - 1] != keys[i])
			keys[k++] = keys[i];
	return k;
}

static uint64
threshold(const SiftColDead *d)
{
	return Max(DV_MIN_ROWS, d->index_rows * DV_ROW_INSTR / DV_GAIN_INSTR);
}

static void
dead_flush(SiftColDead *d)
{
	uint64	   *rest = d->keys;
	uint32		nrest = d->n;

	SIFT_MEM_SCOPE(col_dead_keys, d->cxt);
	if (d->n == 0)
		return;
	sift_sort_u64(d->keys, d->n);
	d->n = unique_keys(d->keys, d->n);
	if (d->n >= threshold(d))
		rest = write_records(d, &nrest);
	append_marks(d, rest, nrest);
	if (rest != d->keys)
		pfree(rest);
	d->n = 0;
	SIFT_MEM_CHECKPOINT();
}

void
sift_col_dead_add(SiftColDead *d, uint64 key)
{
	if (d->n == DV_KEYS_MAX)
		dead_flush(d);
	if (d->n == d->cap)
	{
		SIFT_MEM_SCOPE(col_dead_keys, d->cxt);
		d->cap = Max(1024, d->cap * 2);
		d->keys = d->keys == NULL ? MemoryContextAlloc(d->cxt, d->cap * sizeof(uint64)) : repalloc(d->keys, d->cap * sizeof(uint64));
		SIFT_MEM_CHECKPOINT();
	}
	d->keys[d->n++] = key;
}

void
sift_col_dead_end(SiftColDead *d)
{
	dead_flush(d);
	MemoryContextDelete(d->cxt);
}
