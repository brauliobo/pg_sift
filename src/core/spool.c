/*
 * core/spool.c - a byte spool in memory within its share of maintenance_work_mem, in a temporary file beyond it (core/spool.h)
 */
#include "core/host.h"

#include "core/memtrack.h"
#include "core/spool.h"

#define SPOOL_FIRST_BYTES	4096
#define SPOOL_SHARES		32

struct SiftSpool
{
	MemoryContext mcxt;
	BufFile    *file;
	char	   *buf;
	Size		cap;
	Size		max;
	Size		pos;
	uint64		len;
};

SiftSpool *
sift_spool_begin(void)
{
	SiftSpool  *sp = palloc0(sizeof(SiftSpool));

	sp->mcxt = CurrentMemoryContext;
	sp->max = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / SPOOL_SHARES;
	return sp;
}

static void
spill(SiftSpool *sp)
{
	MemoryContext old = MemoryContextSwitchTo(sp->mcxt);

	sp->file = BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	BufFileWrite(sp->file, sp->buf, sp->len);
	pfree(sp->buf);
	sp->buf = NULL;
	sp->cap = 0;
}

static void
grow(SiftSpool *sp, uint64 need)
{
	Size		cap = Max(sp->cap, SPOOL_FIRST_BYTES);

	while (cap < need)
		cap *= 2;
	cap = Min(cap, sp->max);
	sp->buf = sp->buf ? repalloc(sp->buf, cap) : MemoryContextAlloc(sp->mcxt, cap);
	sp->cap = cap;
}

void
sift_spool_write(SiftSpool *sp, const void *data, Size len)
{
	if (!sp->file && sp->len + len > sp->max)
		spill(sp);
	if (sp->file)
		BufFileWrite(sp->file, data, len);
	else
	{
		if (sp->len + len > sp->cap)
			grow(sp, sp->len + len);
		memcpy(sp->buf + sp->len, data, len);
	}
	sp->len += len;
}

uint64
sift_spool_len(const SiftSpool *sp)
{
	return sp->len;
}

void
sift_spool_rewind(SiftSpool *sp)
{
	sp->pos = 0;
	if (sp->file)
		SIFT_INVARIANT(BufFileSeek(sp->file, 0, 0, SEEK_SET) == 0, "sift: could not rewind a spool");
}

void
sift_spool_read(SiftSpool *sp, void *dst, Size len)
{
	if (sp->file)
		BufFileReadExact(sp->file, dst, len);
	else
	{
		SIFT_INVARIANT(sp->pos + len <= sp->len, "sift: a spool of %llu bytes read to %llu", (unsigned long long) sp->len,
					   (unsigned long long) (sp->pos + len));
		memcpy(dst, sp->buf + sp->pos, len);
		sp->pos += len;
	}
}

void
sift_spool_close(SiftSpool *sp)
{
	if (sp->file)
		BufFileClose(sp->file);
	if (sp->buf)
		pfree(sp->buf);
	pfree(sp);
}
