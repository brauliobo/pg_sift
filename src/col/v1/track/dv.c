/*
 * col/v1/track/dv.c - the dead addresses of one statement, written to the tombstone log of a heap index with track_changes: as deletion records (core/v1_col/tomb/dv.h) for the units that hold
 * enough of them, as marks (one 32-byte entry each) for the rest. The statement's addresses are held (8 bytes each, a million at most) until the statement is done with them, so that they
 * are looked up in the units once and not for each chunk of the probe of the table's unique index.
 *
 * What a statement pays to write records is the lookup of its addresses in the units: DV_ROW_INSTR for each row of the index (every unit is walked), which the marks' fold (col/v1/maint/fold.c) pays
 * once for all the marks it clears. Records are written when the statement has at least as many addresses as pay for the lookup, rows * DV_ROW_INSTR / DV_GAIN_INSTR, and for the units where they
 * are smaller than the marks they replace.
 *
 * The lookup reads the units of the partitions that may hold a row of the statement and no others: the rows the statement noted (sift_col_dead_note_row) bound the first leaf of the key_cut of an index cut by an
 * integer, and the directory is read for the keys between its least and its greatest (the partitions tile the key, so the others hold none of them). An address a unit of another partition holds (the version of
 * a row that held another key) is no row of those units' partitions to look up: it stays a mark.
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/tuptable.h"
#include "storage/lmgr.h"
#include "utils/inval.h"
#include "utils/rel.h"

#include "bm25/build/build.h"
#include "col/am.h"
#include "col/v1/build.h"
#include "col/v1/seg/seg.h"
#include "core/cmp.h"
#include "core/memtrack.h"
#include "core/quarantine/quarantine.h"
#include "core/v1_col/tomb/dv.h"

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
#define MARKS_CHUNK			8192
#define DV_ROUTE_PROBE		256			/* the rows of a statement noted before its keys are held against the partitions: a statement that spans all of them stops noting its rows */

struct SiftColDead
{
	Relation	index;
	SiftTombLog *log;
	MemoryContext cxt;
	TransactionId xid;
	CommandId	cid;
	uint64	   *keys;
	uint32		n;
	uint32		cap;
	uint32		flush_at;		/* the keys held before they are flushed: a quarter of the memory of the statement, at most 4 MB */
	int			cut_leaf;		/* the integer leaf the rows are noted by: the first of the key_cut, -1 when it is not one */
	AttrNumber	cut_att;		/* its attribute in the rows of the statement */
	SiftColDef	cut_def;
	bool		cut_name;
	bool		cut_value;		/* a row had a key ... */
	bool		cut_null;		/* ... or none */
	uint32		cut_rows;
	int64		cut_lo;
	int64		cut_hi;
};

int64		sift_col_dead_segments;

SiftColDead *
sift_col_dead_begin(Relation index, SiftTombLog *log, Relation old_rel)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift dead addresses", ALLOCSET_DEFAULT_SIZES);
	SiftColDead *d = MemoryContextAllocZero(cxt, sizeof(SiftColDead));

	d->index = index;
	d->log = log;
	d->cxt = cxt;
	d->flush_at = (uint32) Max(Min(sift_mem_budget_bytes(SIFT_BUDGET_WORK_MEM, 1, 1) / 4, (Size) 4 * 1024 * 1024) / sizeof(uint64), DV_MIN_ROWS);
	d->xid = GetCurrentTransactionId();
	d->cid = sift_col_stmt_cid();
	d->cut_leaf = sift_col_int_cut_leaf(sift_col_rel(index));
	if (d->cut_leaf >= 0)
	{
		d->cut_def = sift_col_rel(index)->schema->cols[d->cut_leaf];
		d->cut_name = sift_col_rel(index)->name[d->cut_def.attno];
		d->cut_att = sift_col_index_attno_in(index, d->cut_def.attno, old_rel);
		if (d->cut_att == InvalidAttrNumber)
			d->cut_leaf = -1;
	}
	return d;
}

/* The keys of the first leaf of the cut that the noted rows hold: none noted bounds nothing, and a row without a key sorts after every key */
static bool
noted_keys(int leaf, const void *arg, int64 *lo, int64 *hi, bool *null)
{
	const SiftColDead *d = arg;

	if (leaf != d->cut_leaf || !(d->cut_value || d->cut_null))
		return false;
	*null = !d->cut_value;
	*lo = d->cut_value ? d->cut_lo : PG_INT64_MIN;
	*hi = d->cut_value && !d->cut_null ? d->cut_hi : PG_INT64_MAX;
	return true;
}

/* The directory less the partitions that hold no key of the noted rows: every unit the lookup reads is in it */
static SiftDirSnapshot *
dead_dir_read(const SiftColDead *d)
{
	SiftKeyRange range;

	return sift_col_key_range_by(d->index, noted_keys, d, &range) ? sift_dir_read_range(d->index, &range) : sift_dir_read(d->index);
}

void
sift_col_dead_note_row(SiftColDead *d, TupleTableSlot *old, AttrNumber upto)
{
	SiftColValue v;
	bool		isnull;
	Datum		key;

	if (d->cut_leaf < 0)
		return;
	slot_getsomeattrs(old, Max(upto, d->cut_att));
	key = slot_getattr(old, d->cut_att, &isnull);
	if (isnull)
		d->cut_null = true;
	else
	{
		sift_col_datum_values(&d->cut_def, key, d->cut_name, &v);
		d->cut_lo = d->cut_value ? Min(d->cut_lo, v.v.i) : v.v.i;
		d->cut_hi = d->cut_value ? Max(d->cut_hi, v.v.i) : v.v.i;
		d->cut_value = true;
	}
	if (++d->cut_rows == DV_ROUTE_PROBE)
	{
		SiftDirSnapshot *snap = dead_dir_read(d);

		if (!snap->partial)
			d->cut_leaf = -1;
		pfree(snap);
	}
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
	SiftAddrIdx *idx;
	uint64	   *resolved;			/* one bit for each key of the statement (SiftColDead.keys): set when a record ends it */
} Found;

/* The rows of one unit: records when they take fewer slots than marks, else left for the marks */
static void
found_unit(uint32 u, const uint32 *rows, const SiftKey *at, uint32 nrows, void *arg)
{
	Found	   *f = arg;

	if (sift_dv_count(rows, nrows) >= nrows)
		return;
	uint32		k = 0;

	(void) sift_dv_encode(&f->slots, &f->nslots, &f->capslots, f->cxt, f->gen, u, f->d->xid, f->d->cid, rows, nrows);
	for (uint32 i = 0; i < nrows; i++)
	{
		k = sift_u64_from(f->d->keys, k, f->d->n, at[i]);
		f->resolved[k / 64] |= UINT64CONST(1) << (k % 64);
	}
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

/* The keys (ascending) of d that no record ends: the ones left for marks */
static uint64 *
unresolved(const SiftColDead *d, const Found *f, uint32 *n)
{
	uint64	   *rest = MemoryContextAlloc(d->cxt, d->n * sizeof(uint64));

	*n = 0;
	for (uint32 i = 0; i < d->n; i++)
		if (!(f->resolved[i / 64] >> (i % 64) & 1))
			rest[(*n)++] = d->keys[i];
	return rest;
}

static void
find_units(SiftColDead *d, const SiftDirSnapshot *snap, Found *f)
{
	MemoryContext old = MemoryContextSwitchTo(f->cxt);

	f->idx = sift_addr_idx_make((const SiftKey *) d->keys, d->n);
	MemoryContextSwitchTo(old);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(d->index, &snap->segs[i], f->cxt);
		sift_col_dead_segments++;
		f->gen = snap->segs[i].gen;
		sift_cseg_find_live(seg, (const SiftKey *) d->keys, d->n, f->idx, found_unit, f);
		sift_cseg_close(seg);
	}
}

/*
 * The keys (ascending) that records now end are written; the rest (those of units where marks are smaller, and all of them for an index with a scope) are returned. The tombstone lock is
 * held shared from before the directory is read to find the units of the keys until the records are appended, and the step that replaces segments holds it exclusive from the carry of the
 * records to the switch of the directory (col/v1/maint/dvfold.c): a record is never written for a segment that a switch already replaced, and a step that comes after finds the record and carries it
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
	f.resolved = MemoryContextAllocZero(tmp, SIFT_COL_NWORDS(d->n) * sizeof(uint64));
	LockPage(d->index, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	snap = dead_dir_read(d);
	if (!has_scope(snap))
		find_units(d, snap, &f);
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_COL_GATE_DELETE);
#endif
	if (f.nslots > 0)
	{
		stamp_format(d);
		sift_tomb_append_many(d->index, d->log, f.slots, (int) f.nslots);
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

/* The addresses from which records pay: the lookup walks every row of the index, which the directory says */
static bool
records_pay(const SiftColDead *d)
{
	SiftDirSnapshot *snap;
	uint64		rows = 0;

	if (d->n < DV_MIN_ROWS)
		return false;
	snap = dead_dir_read(d);
	for (int i = 0; i < snap->nsegs; i++)
		rows += snap->segs[i].kind == SIFT_SEG_IMMUTABLE ? snap->segs[i].ndocs : 0;
	pfree(snap);
	return d->n >= rows * DV_ROW_INSTR / DV_GAIN_INSTR;
}

static void
dead_flush(SiftColDead *d)
{
	uint64	   *rest = d->keys;
	uint32		nrest = d->n;

	SiftMemTracker mt_col_dead_keys;

	sift_mem_init_sub(&mt_col_dead_keys, "col_dead_keys", d->cxt, false, sift_mem_budget_bytes(SIFT_BUDGET_WORK_MEM, 1, 1));
	if (d->n == 0)
		return;
	sift_sort_u64(d->keys, d->n);
	d->n = unique_keys(d->keys, d->n);
	if (records_pay(d))
		rest = write_records(d, &nrest);
	append_marks(d, rest, nrest);
	if (rest != d->keys)
		pfree(rest);
	d->n = 0;
	sift_mem_assert_within(&mt_col_dead_keys);
}

void
sift_col_dead_add(SiftColDead *d, uint64 key)
{
	if (d->n == d->flush_at)
		dead_flush(d);
	if (d->n == d->cap)
	{
		SiftMemTracker mt_col_dead_keys;

		sift_mem_init_sub(&mt_col_dead_keys, "col_dead_keys", d->cxt, false, sift_mem_budget_bytes(SIFT_BUDGET_WORK_MEM, 1, 1));
		d->cap = Min(Max(1024, d->cap * 2), d->flush_at);
		d->keys = d->keys == NULL ? MemoryContextAlloc(d->cxt, d->cap * sizeof(uint64)) : repalloc(d->keys, d->cap * sizeof(uint64));
		sift_mem_assert_within(&mt_col_dead_keys);
	}
	d->keys[d->n++] = key;
}

void
sift_col_dead_end(SiftColDead *d)
{
	dead_flush(d);
	MemoryContextDelete(d->cxt);
}
