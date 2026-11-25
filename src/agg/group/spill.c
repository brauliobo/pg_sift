/*
 * agg/group/spill.c - the spill of agg/group/spill.h.
 */
#include "core/host.h"

#include "miscadmin.h"
#include "storage/buffile.h"

#include "agg/group/spill.h"
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

struct SiftGroupSpill
{
	const SiftRadixShape *shape;
	MemoryContext cxt;
	SpillSet	top;
	SpillSet   *open[SIFT_SPILL_MAX_DEPTH + 1];	/* the levels below that a scan has files of, so that an error in it leaves none open */
	SiftRecBuf	rec;
	uint64		bytes;
	uint32		puts;
	uint32		depth;			/* the deepest level a scan has split to */
};

#define PART_BITS	4
StaticAssertDecl((1 << PART_BITS) == SIFT_SPILL_PARTS, "the partition bits are those of the partitions");

/* the bits of the hash a level partitions by: the top ones (those of SIFT_HASH_RADIX), then ones neither the slot probe (0..31), the salt (48..54) nor the radix (56..63) of the table use */
static const uint8 LEVEL_SHIFT[SIFT_SPILL_MAX_DEPTH + 1] = {64 - PART_BITS, 40, 32, 36};

static inline uint32
part_of(uint64 hash, uint32 depth)
{
	return (uint32) (hash >> LEVEL_SHIFT[depth]) & (SIFT_SPILL_PARTS - 1);
}

/* MEM[MEM-AGG-11]: SiftGroupSpill per-level stage and BufFiles (SIFT_SPILL_PARTS times 12KB is 192KB a level and 768KB at most; a sub-level partition file is closed when its merge is done, the top level stays for the next scan of the node; was held to the end of the scan) */
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

/* The next record of the file into buf (grown), false at the end */
static bool
read_record(BufFile *f, SiftRecBuf *buf)
{
	uint32		len;

	if (buf->cap < 8)
	{
		buf->cap = 1024;
		buf->p = buf->p ? repalloc(buf->p, buf->cap) : palloc(buf->cap);
	}
	if (BufFileReadMaybeEOF(f, buf->p, 8, true) == 0)
		return false;
	len = sift_rec_len(buf->p);
	if (len > buf->cap)
	{
		buf->cap = len;
		buf->p = repalloc(buf->p, buf->cap);
	}
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

static void
over_budget_error(const SiftGroupSpillIter *it)
{
	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the groups of a partition take more than %lu bytes", (unsigned long) it->budget),
					errdetail("One group, or the distinct values of one group, does not fit after %d levels of partitions.", SIFT_SPILL_MAX_DEPTH),
					errhint("Raise work_mem or hash_mem_multiplier.")));
}

static bool
over_budget(const SiftGroupSpillIter *it, const SiftRecMerge *m)
{
	const SiftRadixShape *sh = m->shape;

	return sift_ht_bytes(m->ht) + m->dist_bytes + (sh->held != NULL ? sh->held(sh) : 0) > it->budget;
}

/* The table into the next level: its groups leave it, and the records that follow go there */
static SpillSet *
split_table(SiftGroupSpillIter *it, uint32 depth, uint64 *bytes)
{
	SpillSet   *sub = palloc(sizeof(SpillSet));
	PutArg		a = {sub, bytes};

	if (depth >= SIFT_SPILL_MAX_DEPTH)
		over_budget_error(it);
	set_init(sub, depth + 1);
	it->sp->open[depth + 1] = sub;
	sift_rec_flush_ht(&it->ht, it->sp->shape, &it->rec, put_cb, &a);
	sift_ht_destroy(&it->ht);
	return sub;
}

/* The partition of the file into it->ht; one that takes more than the budget is split by the bits of the next level, which is then the next to read (false) */
static bool
iter_load(SiftGroupSpillIter *it, BufFile *f, uint32 depth)
{
	const SiftRadixShape *sh = it->sp->shape;
	SiftRecMerge m = {.shape = sh,.ht = &it->ht};
	SpillSet   *sub = NULL;
	uint64		bytes = 0;
	uint32		n = 0;

	if (sh->part_begin != NULL)
		sh->part_begin(sh);
	sift_ht_init_ex(&it->ht, SIFT_HT_MIN_SLOTS, sh->wide_key, sh->ext_stride, sh->ext_dist_off);
	if (BufFileSeek(f, 0, 0, SEEK_SET) != 0)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not rewind a spill file of the groups"),
						errhint("Check the free space of the temporary tablespace.")));
	while (read_record(f, &it->rec))
	{
		if (sub)
			set_put(sub, sift_rec_hash(it->rec.p), it->rec.p, sift_rec_len(it->rec.p), &bytes);
		else
		{
			sift_rec_merge(&m, it->rec.p);
			if (++n % 64 == 0)
			{
				CHECK_FOR_INTERRUPTS();
				if (over_budget(it, &m))
					sub = split_table(it, depth, &bytes);
			}
		}
	}
	if (!sub && over_budget(it, &m))
		sub = split_table(it, depth, &bytes);
	if (!sub)
		return true;
	it->sp->depth = Max(it->sp->depth, depth + 1);
	set_seal(sub, &bytes);
	it->stack[++it->top] = (Frame) {sub, 0};
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
	if (it->rec.p)
		pfree(it->rec.p);
	pfree(it);
}

void
sift_gspill_scan(SiftGroupSpill *sp, uint64 budget, SiftGroupSpillVisit visit, void *arg)
{
	SiftGroupSpillIter *it = sift_gspill_iter_begin(sp, budget, false);
	const SiftSaltedHT *ht;

	while ((ht = sift_gspill_iter_next(it)) != NULL)
		visit(arg, (SiftSaltedHT *) ht);
	sift_gspill_iter_end(it);
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

uint32
sift_gspill_depth(const SiftGroupSpill *sp)
{
	return sp->depth;
}

void
sift_gspill_free(SiftGroupSpill *sp)
{
	for (int d = 0; d <= SIFT_SPILL_MAX_DEPTH; d++)
		if (sp->open[d])
			set_free(sp->open[d]);
	set_free(&sp->top);
	MemoryContextDelete(sp->cxt);
}
