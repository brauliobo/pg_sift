/*
 * agg/spill.c - the partitioned spill files (v6_spill.h).
 */
#include "agg/spill.h"

#define SIFT_SPILL_CHECK_EVERY	1024

/*
 * A record starts with a byte: bit 0 says a tag follows (a record of tag 0 has none), the others are its length when it is below SPILL_LONG; at SPILL_LONG or more, the byte holds SPILL_LONG and the
 * length follows in 4 bytes, before the tag.
 */
#define SPILL_LONG		127
#define SPILL_HDR_MAX	(1 + sizeof(uint32) + sizeof(uint64))

struct SiftSpillSet
{
	MemoryContext cx;
	uint32		nparts;
	uint32		ops;			/* records put or got: a cancel is looked for once in SIFT_SPILL_CHECK_EVERY */
	BufFile   **file;
	uint64	   *nrec;
	uint64	   *nbytes;
};

typedef struct SpillWhere
{
	uint32		part;
	uint32		nparts;
} SpillWhere;

static void
spill_context(void *arg)
{
	const SpillWhere *w = arg;

	errcontext("partition %u of %u of the spill files of an aggregate scan; more work_mem spills less", w->part, w->nparts);
}

/* the work on a partition: a cancel is seen between its records, and an error of its temporary file names the partition */
static void
spill_enter(SiftSpillSet *s, uint32 part, SpillWhere *w, ErrorContextCallback *cb)
{
	if (++s->ops % SIFT_SPILL_CHECK_EVERY == 0)
		CHECK_FOR_INTERRUPTS();
	*w = (SpillWhere) {.part = part,.nparts = s->nparts};
	*cb = (ErrorContextCallback) {.callback = spill_context,.arg = w,.previous = error_context_stack};
	error_context_stack = cb;
}

uint32
sift_spill_nparts(uint64 expect_bytes, uint64 budget_bytes)
{
	uint64		want = budget_bytes ? expect_bytes / budget_bytes + 1 : SIFT_SPILL_MAX_PARTS;
	uint32		n = SIFT_SPILL_MIN_PARTS;

	while (n < SIFT_SPILL_MAX_PARTS && n < want * 2)
		n <<= 1;
	return n;
}

SiftSpillSet *
sift_spill_create(MemoryContext cx, uint32 nparts)
{
	SiftSpillSet *s;

	SIFT_INVARIANT(nparts >= 2 && (nparts & (nparts - 1)) == 0, "spill: %u partitions is no power of two", nparts);
	s = MemoryContextAllocZero(cx, sizeof(SiftSpillSet));
	s->cx = cx;
	s->nparts = nparts;
	s->file = MemoryContextAllocZero(cx, sizeof(BufFile *) * nparts);
	s->nrec = MemoryContextAllocZero(cx, sizeof(uint64) * nparts);
	s->nbytes = MemoryContextAllocZero(cx, sizeof(uint64) * nparts);
	return s;
}

uint32
sift_spill_nparts_of(const SiftSpillSet *s)
{
	return s->nparts;
}

static BufFile *
spill_file_create(SiftSpillSet *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	BufFile    *f = BufFileCreateTemp(false);

	MemoryContextSwitchTo(old);
	return f;
}

void
sift_spill_put(SiftSpillSet *s, uint32 part, uint64 tag, const void *bytes, uint32 len)
{
	uint8		hdr[SPILL_HDR_MAX];
	uint32		hn = 1;
	SpillWhere	where;
	ErrorContextCallback cb;

	hdr[0] = (uint8) ((tag != 0) | (Min(len, SPILL_LONG) << 1));
	if (len >= SPILL_LONG)
	{
		memcpy(hdr + hn, &len, sizeof(len));
		hn += sizeof(len);
	}
	if (tag != 0)
	{
		memcpy(hdr + hn, &tag, sizeof(tag));
		hn += sizeof(tag);
	}
	spill_enter(s, part, &where, &cb);
	if (s->file[part] == NULL)
		s->file[part] = spill_file_create(s);
	BufFileWrite(s->file[part], hdr, hn);
	if (len > 0)
		BufFileWrite(s->file[part], bytes, len);
	s->nrec[part]++;
	s->nbytes[part] += hn + len;
	error_context_stack = cb.previous;
}

void
sift_spill_rewind(SiftSpillSet *s, uint32 part)
{
	if (s->file[part] != NULL)
		SIFT_INVARIANT(BufFileSeek(s->file[part], 0, 0, SEEK_SET) == 0, "spill: could not rewind the temporary file of partition %u", part);
}

bool
sift_spill_get(SiftSpillSet *s, uint32 part, uint64 *tag, uint8 **buf, uint32 *cap, uint32 *len)
{
	uint8		first;
	uint32		n;
	SpillWhere	where;
	ErrorContextCallback cb;

	if (s->file[part] == NULL)
		return false;
	spill_enter(s, part, &where, &cb);
	if (BufFileReadMaybeEOF(s->file[part], &first, sizeof(first), true) == 0)
	{
		error_context_stack = cb.previous;
		return false;
	}
	n = first >> 1;
	if (n == SPILL_LONG)
		BufFileReadExact(s->file[part], &n, sizeof(n));
	*tag = 0;
	if (first & 1)
		BufFileReadExact(s->file[part], tag, sizeof(*tag));
	if (n > *cap)
	{
		uint32		ncap = Max(n, *cap * 2);

		if (*buf != NULL)
			pfree(*buf);
		*buf = MemoryContextAlloc(s->cx, ncap);
		*cap = ncap;
	}
	if (n > 0)
		BufFileReadExact(s->file[part], *buf, n);
	*len = n;
	error_context_stack = cb.previous;
	return true;
}

uint64
sift_spill_part_records(const SiftSpillSet *s, uint32 part)
{
	return s->nrec[part];
}

uint64
sift_spill_part_bytes(const SiftSpillSet *s, uint32 part)
{
	return s->nbytes[part];
}

void
sift_spill_drop(SiftSpillSet *s, uint32 part)
{
	if (s->file[part] != NULL)
		BufFileClose(s->file[part]);
	s->file[part] = NULL;
}

uint64
sift_spill_total_records(const SiftSpillSet *s)
{
	uint64		n = 0;

	for (uint32 i = 0; i < s->nparts; i++)
		n += s->nrec[i];
	return n;
}

uint64
sift_spill_total_bytes(const SiftSpillSet *s)
{
	uint64		n = 0;

	for (uint32 i = 0; i < s->nparts; i++)
		n += s->nbytes[i];
	return n;
}

void
sift_spill_free(SiftSpillSet *s)
{
	for (uint32 i = 0; i < s->nparts; i++)
		sift_spill_drop(s, i);
	pfree(s->file);
	pfree(s->nrec);
	pfree(s->nbytes);
	pfree(s);
}
