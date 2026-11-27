/*
 * agg/ht/spill.c - the spill of agg/ht/spill.h.
 */
#include "core/host.h"

#include "access/tupdesc.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/tuplesort.h"

#include "agg/ht/spill.h"
#include "core/hash.h"
#include "core/memtrack.h"

/* one level of partitions: the files of the records whose hash has the same SIFT_SPILL_PARTS bits at the level's shift */
typedef struct SpillSet
{
	uint8		depth;
	BufFile    *files[SIFT_SPILL_PARTS];
	uint8	   *stage;			/* SIFT_SPILL_PARTS * SIFT_SPILL_STAGE_BYTES */
	uint32		used[SIFT_SPILL_PARTS];
	bool		sealed;
} SpillSet;

/* the records of a partition that the levels cannot make smaller, sorted by hash: they come back a run of one hash at a time, in the memory of the budget */
typedef struct SortRun
{
	Tuplesortstate *sort;
	TupleDesc	desc;
	TupleTableSlot *put;
	TupleTableSlot *get;
	SiftRecBuf	datum;			/* the record as the bytea that is sorted */
	bool		pending;		/* the record in the iterator's buffer is the first of the next table */
} SortRun;

struct SiftGroupSpill
{
	const SiftRadixShape *shape;
	MemoryContext cxt;
	SpillSet	top;
	SpillSet   *open[SIFT_SPILL_MAX_DEPTH + 1];	/* the levels below that a scan has files of, so that an error in it leaves none open */
	SortRun    *run;			/* the sort of the partition in hand, if the levels did not make it fit */
	SiftRecBuf	rec;
	uint64		bytes;
	uint32		puts;
};

#define PART_BITS	5
StaticAssertDecl((1 << PART_BITS) == SIFT_SPILL_PARTS, "the partition bits are those of the partitions");

/* the bits of the hash the first levels partition by: the top ones (those of SIFT_HASH_RADIX), then ones neither the slot probe (0..31), the salt (48..54) nor the radix (56..63) of the table use; the levels below mix the hash again */
static const uint8 LEVEL_SHIFT[] = {64 - PART_BITS, 37, 32, 42};

static inline uint32
part_of(uint64 hash, uint32 depth)
{
	const uint64 bits = depth < lengthof(LEVEL_SHIFT) ? hash >> LEVEL_SHIFT[depth] : sift_fmix64(hash + depth * SIFT_FIB64);

	return (uint32) bits & (SIFT_SPILL_PARTS - 1);
}

/* MEM[MEM-AGG-11]: SiftGroupSpill per-level stage and BufFiles (SIFT_SPILL_PARTS times 10KB is 320KB a level and 1.3MB at most; a sub-level partition file is closed when its merge is done, the top level stays for the next scan of the node; was held to the end of the scan) */
static void
set_init(SpillSet *ss, uint32 depth)
{
	memset(ss, 0, sizeof(*ss));
	ss->depth = (uint8) depth;
	ss->stage = palloc((Size) SIFT_SPILL_PARTS * SIFT_SPILL_STAGE_BYTES);
}

static void
stage_flush(SpillSet *ss, uint32 p, uint64 *bytes)
{
	if (ss->used[p] == 0)
		return;
	if (ss->files[p] == NULL)
		ss->files[p] = BufFileCreateTemp(false);
	BufFileWrite(ss->files[p], ss->stage + (Size) p * SIFT_SPILL_STAGE_BYTES, ss->used[p]);
	*bytes += ss->used[p];
	ss->used[p] = 0;
}

static void
set_put(SpillSet *ss, uint64 hash, const uint8 *rec, uint32 len, uint64 *bytes)
{
	const uint32 p = part_of(hash, ss->depth);

	Assert(!ss->sealed);
	if (len > SIFT_SPILL_STAGE_BYTES)
	{
		stage_flush(ss, p, bytes);
		if (ss->files[p] == NULL)
			ss->files[p] = BufFileCreateTemp(false);
		BufFileWrite(ss->files[p], rec, len);
		*bytes += len;
		return;
	}
	if (ss->used[p] + len > SIFT_SPILL_STAGE_BYTES)
		stage_flush(ss, p, bytes);
	memcpy(ss->stage + (Size) p * SIFT_SPILL_STAGE_BYTES + ss->used[p], rec, len);
	ss->used[p] += len;
}

static void
set_seal(SpillSet *ss, uint64 *bytes)
{
	for (uint32 p = 0; p < SIFT_SPILL_PARTS; p++)
		stage_flush(ss, p, bytes);
	ss->sealed = true;
}

static void
set_free(SpillSet *ss)
{
	for (uint32 p = 0; p < SIFT_SPILL_PARTS; p++)
		if (ss->files[p])
			BufFileClose(ss->files[p]);
	pfree(ss->stage);
}

SiftGroupSpill *
sift_gspill_new(const SiftRadixShape *shape)
{
	MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "group spill");
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftGroupSpill *sp = palloc0(sizeof(SiftGroupSpill));

	sp->shape = shape;
	sp->cxt = cxt;
	set_init(&sp->top, 0);
	MemoryContextSwitchTo(old);
	return sp;
}

typedef struct PutArg
{
	SpillSet   *ss;
	uint64	   *bytes;
} PutArg;

static void
put_cb(void *arg, uint64 hash, const uint8 *rec, uint32 len)
{
	PutArg	   *a = arg;

	set_put(a->ss, hash, rec, len, a->bytes);
}

void
sift_gspill_put_ht(SiftGroupSpill *sp, const SiftSaltedHT *ht)
{
	PutArg		a = {&sp->top, &sp->bytes};
	MemoryContext old = MemoryContextSwitchTo(sp->cxt);

	sift_rec_flush_ht(ht, sp->shape, &sp->rec, put_cb, &a);
	MemoryContextSwitchTo(old);
	sp->puts++;
}

static void
buf_fit(SiftRecBuf *buf, uint32 len)
{
	if (len <= buf->cap)
		return;
	buf->cap = Max(len, 1024);
	buf->p = buf->p ? repalloc(buf->p, buf->cap) : palloc(buf->cap);
}

/* The next record of the file into buf (grown), false at the end */
static bool
read_record(BufFile *f, SiftRecBuf *buf)
{
	uint32		len;

	buf_fit(buf, 8);
	if (BufFileReadMaybeEOF(f, buf->p, 8, true) == 0)
		return false;
	len = sift_rec_len(buf->p);
	buf_fit(buf, len);
	BufFileReadExact(f, buf->p + 8, len - 8);
	return true;
}

typedef struct Frame
{
	SpillSet   *set;
	uint32		next;
} Frame;

/* the partitions of the levels a scan is in: the explicit stack of the recursion into the levels below */
struct SiftGroupSpillIter
{
	SiftGroupSpill *sp;
	uint64		budget;
	bool		consume;
	SiftRecBuf	rec;
	Frame		stack[SIFT_SPILL_MAX_DEPTH + 1];
	int			top;
	SiftSaltedHT ht;
	bool		live;
};

static bool
over_budget(const SiftGroupSpillIter *it, const SiftRecMerge *m)
{
	const SiftRadixShape *sh = m->shape;

	return sift_ht_bytes(m->ht) + m->dist_bytes + (sh->held != NULL ? sh->held(sh) : 0) > it->budget;
}

static void
table_init(SiftGroupSpillIter *it)
{
	const SiftRadixShape *sh = it->sp->shape;

	if (sh->part_begin != NULL)
		sh->part_begin(sh);
	sift_ht_init_ex(&it->ht, SIFT_HT_MIN_SLOTS, sh->wide_key, sh->ext_stride, sh->ext_dist_off);
}

/* The table into the next level: its groups leave it, and the records that follow go there */
static SpillSet *
split_table(SiftGroupSpillIter *it, uint32 depth, uint64 *bytes)
{
	SpillSet   *sub = palloc(sizeof(SpillSet));
	PutArg		a = {sub, bytes};

	set_init(sub, depth + 1);
	it->sp->open[depth + 1] = sub;
	sift_rec_flush_ht(&it->ht, it->sp->shape, &it->rec, put_cb, &a);
	sift_ht_destroy(&it->ht);
	return sub;
}

/* the sort of the records by their hash: the partition has no level left to be split by, and the groups of one hash are the most a table must hold */
static void
sort_put(SortRun *run, uint64 hash, const uint8 *rec, uint32 len)
{
	buf_fit(&run->datum, VARHDRSZ + len);
	SET_VARSIZE(run->datum.p, VARHDRSZ + len);
	memcpy(VARDATA(run->datum.p), rec, len);
	ExecClearTuple(run->put);
	run->put->tts_values[0] = Int64GetDatum((int64) hash);
	run->put->tts_values[1] = PointerGetDatum(run->datum.p);
	run->put->tts_isnull[0] = run->put->tts_isnull[1] = false;
	ExecStoreVirtualTuple(run->put);
	tuplesort_puttupleslot(run->sort, run->put);
}

static void
sort_put_cb(void *arg, uint64 hash, const uint8 *rec, uint32 len)
{
	sort_put(arg, hash, rec, len);
}

/* The groups of the table go to a sort in the memory of the budget, and the records that follow go there */
static void
sort_begin(SiftGroupSpillIter *it)
{
	SiftGroupSpill *sp = it->sp;
	SortRun    *run = palloc0(sizeof(SortRun));
	AttrNumber	col = 1;
	Oid			op = Int8LessOperator,
				coll = InvalidOid;
	bool		nulls_first = false;

	run->desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(run->desc, 1, "hash", INT8OID, -1, 0);
	TupleDescInitEntry(run->desc, 2, "rec", BYTEAOID, -1, 0);
	sift_tupdesc_finalize(run->desc);
	run->sort = tuplesort_begin_heap(run->desc, 1, &col, &op, &coll, &nulls_first, (int) Max(it->budget / 1024, 64), NULL, TUPLESORT_NONE);
	run->put = MakeSingleTupleTableSlot(run->desc, &TTSOpsVirtual);
	run->get = MakeSingleTupleTableSlot(run->desc, &TTSOpsMinimalTuple);
	sp->run = run;
	sift_rec_flush_ht(&it->ht, sp->shape, &it->rec, sort_put_cb, run);
	sift_ht_destroy(&it->ht);
}

static void
sort_end(SiftGroupSpill *sp)
{
	SortRun    *run = sp->run;

	tuplesort_end(run->sort);
	ExecDropSingleTupleTableSlot(run->put);
	ExecDropSingleTupleTableSlot(run->get);
	FreeTupleDesc(run->desc);
	if (run->datum.p)
		pfree(run->datum.p);
	pfree(run);
	sp->run = NULL;
}

/* The next record of the sort into buf (grown), false at the end */
static bool
sort_read(SortRun *run, SiftRecBuf *buf)
{
	bool		isnull;
	bytea	   *rec;

	if (!tuplesort_gettupleslot(run->sort, true, false, run->get, NULL))
		return false;
	rec = DatumGetByteaPP(slot_getattr(run->get, 2, &isnull));
	buf_fit(buf, VARSIZE_ANY_EXHDR(rec));
	memcpy(buf->p, VARDATA_ANY(rec), VARSIZE_ANY_EXHDR(rec));
	return true;
}

/* The next table of the sorted records: the groups of one hash at a time into it->ht until it takes more than the budget, whose records are the same group wherever they are; false at the end of the sort */
static bool
sort_load(SiftGroupSpillIter *it)
{
	SortRun    *run = it->sp->run;
	SiftRecMerge m = {.shape = it->sp->shape,.ht = &it->ht};
	uint64		hash = 0;
	uint32		n = 0;

	table_init(it);
	while (run->pending || sort_read(run, &it->rec))
	{
		const uint64 next = sift_rec_hash(it->rec.p);

		if (n > 0 && next != hash && over_budget(it, &m))
		{
			run->pending = true;
			return true;
		}
		run->pending = false;
		hash = next;
		sift_rec_merge(&m, it->rec.p);
		if (++n % 64 == 0)
			CHECK_FOR_INTERRUPTS();
	}
	if (n > 0)
		return true;
	sift_ht_destroy(&it->ht);
	sort_end(it->sp);
	return false;
}

/* The table over its budget: its groups go to the next level of partitions, and from the last level to the sort */
static void
spill_table(SiftGroupSpillIter *it, uint32 depth, SpillSet **sub, uint64 *bytes)
{
	if (depth < SIFT_SPILL_MAX_DEPTH)
		*sub = split_table(it, depth, bytes);
	else
		sort_begin(it);
}

/* The partition of the file into it->ht; one that takes more than the budget is split by the bits of the next level, which is then the next to read (false), or sorted at the last level */
static bool
iter_load(SiftGroupSpillIter *it, BufFile *f, uint32 depth)
{
	SiftRecMerge m = {.shape = it->sp->shape,.ht = &it->ht};
	SpillSet   *sub = NULL;
	uint64		bytes = 0;
	uint32		n = 0;

	table_init(it);
	if (BufFileSeek(f, 0, 0, SEEK_SET) != 0)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not rewind a spill file of the groups"),
						errhint("Check the free space of the temporary tablespace.")));
	while (read_record(f, &it->rec))
	{
		if (++n % 64 == 0)
			CHECK_FOR_INTERRUPTS();
		if (it->sp->run)
			sort_put(it->sp->run, sift_rec_hash(it->rec.p), it->rec.p, sift_rec_len(it->rec.p));
		else if (sub)
			set_put(sub, sift_rec_hash(it->rec.p), it->rec.p, sift_rec_len(it->rec.p), &bytes);
		else
		{
			sift_rec_merge(&m, it->rec.p);
			if (n % 64 == 0 && over_budget(it, &m))
				spill_table(it, depth, &sub, &bytes);
		}
	}
	if (!sub && !it->sp->run && over_budget(it, &m))
		spill_table(it, depth, &sub, &bytes);
	if (!sub && !it->sp->run)
		return true;
	if (it->sp->run)
		tuplesort_performsort(it->sp->run->sort);
	else
	{
		set_seal(sub, &bytes);
		it->stack[++it->top] = (Frame) {sub, 0};
	}
	return false;
}

SiftGroupSpillIter *
sift_gspill_iter_begin(SiftGroupSpill *sp, uint64 budget, bool consume)
{
	MemoryContext old = MemoryContextSwitchTo(sp->cxt);
	SiftGroupSpillIter *it = palloc0(sizeof(SiftGroupSpillIter));

	it->sp = sp;
	it->budget = budget;
	it->consume = consume;
	set_seal(&sp->top, &sp->bytes);
	it->stack[0] = (Frame) {&sp->top, 0};
	MemoryContextSwitchTo(old);
	return it;
}

const SiftSaltedHT *
sift_gspill_iter_next(SiftGroupSpillIter *it)
{
	SiftGroupSpill *sp = it->sp;
	MemoryContext old = MemoryContextSwitchTo(sp->cxt);
	const SiftSaltedHT *out = NULL;

	if (it->live)
		sift_ht_destroy(&it->ht);
	it->live = false;
	while (it->top >= 0 && out == NULL)
	{
		const int	at = it->top;
		SpillSet   *set = it->stack[at].set;
		uint32		p = it->stack[at].next;

		if (sp->run)
		{
			it->live = sort_load(it);
			out = it->live ? &it->ht : NULL;
			continue;
		}

		while (p < SIFT_SPILL_PARTS && set->files[p] == NULL)
			p++;
		if (p == SIFT_SPILL_PARTS)
		{
			if (at > 0)
			{
				sp->open[set->depth] = NULL;
				set_free(set);
				pfree(set);
			}
			it->top--;
			continue;
		}
		it->stack[at].next = p + 1;
		it->live = iter_load(it, set->files[p], set->depth);
		if (it->consume || set->depth > 0)
		{
			BufFileClose(set->files[p]);
			set->files[p] = NULL;
		}
		if (it->live)
			out = &it->ht;
	}
	MemoryContextSwitchTo(old);
	return out;
}

void
sift_gspill_iter_end(SiftGroupSpillIter *it)
{
	if (it->live)
		sift_ht_destroy(&it->ht);
	if (it->sp->run)
		sort_end(it->sp);
	if (it->rec.p)
		pfree(it->rec.p);
	pfree(it);
}

uint64
sift_gspill_bytes(const SiftGroupSpill *sp)
{
	return sp->bytes;
}

uint32
sift_gspill_puts(const SiftGroupSpill *sp)
{
	return sp->puts;
}

void
sift_gspill_free(SiftGroupSpill *sp)
{
	if (sp->run)
		sort_end(sp);
	for (int d = 0; d <= SIFT_SPILL_MAX_DEPTH; d++)
		if (sp->open[d])
			set_free(sp->open[d]);
	set_free(&sp->top);
	MemoryContextDelete(sp->cxt);
}
