/*
 * agg/v6/gj_bits.c - the windowed bits of v6_gj_bits.h.
 */
#include "agg/v6/gj_bits.h"

#define GB_WINDOW_BYTES	(64 * 1024)
#define GB_WINDOW_BITS	((uint64) GB_WINDOW_BYTES * 8)

struct SiftGjBits
{
	BufFile    *file;
	uint8	   *window;
	uint64		cur;			/* the window in memory */
	uint64		nflushed;		/* the windows the file has: the others are clear */
	bool		dirty;
};

SiftGjBits *
sift_gj_bits_open(MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftGjBits *b = palloc0(sizeof(SiftGjBits));

	b->file = BufFileCreateTemp(false);
	b->window = palloc0(GB_WINDOW_BYTES);
	MemoryContextSwitchTo(old);
	return b;
}

static void
gb_flush(SiftGjBits *b)
{
	if (!b->dirty)
		return;
	SIFT_INVARIANT(BufFileSeek(b->file, 0, (pgoff_t) (b->cur * GB_WINDOW_BYTES), SEEK_SET) == 0, "hash join: could not seek the matched bits");
	BufFileWrite(b->file, b->window, GB_WINDOW_BYTES);
	b->nflushed = Max(b->nflushed, b->cur + 1);
	b->dirty = false;
}

static void
gb_window(SiftGjBits *b, uint64 ord)
{
	const uint64 w = ord / GB_WINDOW_BITS;

	if (w == b->cur)
		return;
	gb_flush(b);
	b->cur = w;
	if (w < b->nflushed)
	{
		SIFT_INVARIANT(BufFileSeek(b->file, 0, (pgoff_t) (w * GB_WINDOW_BYTES), SEEK_SET) == 0, "hash join: could not seek the matched bits");
		BufFileReadExact(b->file, b->window, GB_WINDOW_BYTES);
	}
	else
		memset(b->window, 0, GB_WINDOW_BYTES);
}

bool
sift_gj_bits_get(SiftGjBits *b, uint64 ord)
{
	gb_window(b, ord);
	return (b->window[(ord % GB_WINDOW_BITS) >> 3] >> (ord & 7)) & 1;
}

void
sift_gj_bits_set(SiftGjBits *b, uint64 ord)
{
	gb_window(b, ord);
	b->window[(ord % GB_WINDOW_BITS) >> 3] |= (uint8) (1 << (ord & 7));
	b->dirty = true;
}

void
sift_gj_bits_close(SiftGjBits *b)
{
	BufFileClose(b->file);
	pfree(b->window);
	pfree(b);
}
