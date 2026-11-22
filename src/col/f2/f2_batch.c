/*
 * col/f2/f2_batch.c - the policy of a batched bulk insert and the string arena of a row buffer (col/f2/f2_batch.h).
 */
#include "core/host.h"

#include "col/f2/f2_batch.h"

struct SiftStrChunk
{
	SiftStrChunk *next;
};

#define ALIGN8(n)	(((n) + 7) & ~(Size) 7)
#define CHUNK_HDR	ALIGN8(sizeof(SiftStrChunk))

SiftBatchRoute
sift_batch_route(int nslots, bool published, bool row_triggers)
{
	if (published)
		return SIFT_BATCH_STAGED_PUBLISHED;
	if (row_triggers)
		return SIFT_BATCH_STAGED_TRIGGERS;
	return nslots < SIFT_BATCH_MIN_SLOTS ? SIFT_BATCH_STAGED_SMALL : SIFT_BATCH_DIRECT;
}

uint32
sift_batch_flush_rows(uint64 budget_bytes, uint64 row_bytes)
{
	uint64		rows = budget_bytes / Max(row_bytes, 1);

	rows -= rows % SIFT_BATCH_GRANULE_ROWS;
	return (uint32) Min(Max(rows, (uint64) SIFT_BATCH_MIN_ROWS), (uint64) SIFT_BATCH_MAX_ROWS);
}

uint64
sift_batch_row_bytes(int ncols, int nstr, uint32 string_bytes)
{
	return 60 + (uint64) ncols * 18 + (uint64) nstr * ((uint64) string_bytes + 1);
}

bool
sift_batch_reset_due(uint32 *since, uint32 nrows)
{
	*since += nrows;
	if (*since < SIFT_BATCH_RESET_ROWS)
		return false;
	*since = 0;
	return true;
}

uint64
sift_batch_vblocks(uint64 nrows, uint64 unit)
{
	uint64		blocks = (nrows + SIFT_BATCH_VBLOCK_ROWS - 1) / SIFT_BATCH_VBLOCK_ROWS;

	return (blocks + unit - 1) / unit * unit;
}

void
sift_str_arena_init(SiftStrArena *a, MemoryContext cx)
{
	*a = (SiftStrArena) {.cx = cx};
}

static char *
chunk_alloc(SiftStrArena *a, Size payload)
{
	SiftStrChunk *c = MemoryContextAlloc(a->cx, CHUNK_HDR + payload);

	c->next = a->chunks;
	a->chunks = c;
	a->nchunks++;
	return (char *) c + CHUNK_HDR;
}

char *
sift_str_arena_copy(SiftStrArena *a, const char *s, uint32 len)
{
	Size		need = (Size) len + 1;
	char	   *p;

	if (need > SIFT_BATCH_STR_CHUNK / 4)
		p = chunk_alloc(a, need);
	else
	{
		if (a->left < need)
		{
			a->at = chunk_alloc(a, SIFT_BATCH_STR_CHUNK);
			a->left = SIFT_BATCH_STR_CHUNK;
		}
		p = a->at;
		a->at += need;
		a->left -= need;
	}
	memcpy(p, s, len);
	p[len] = '\0';
	a->bytes += need;
	return p;
}

void
sift_str_arena_free(SiftStrArena *a)
{
	for (SiftStrChunk *c = a->chunks, *next; c != NULL; c = next)
	{
		next = c->next;
		pfree(c);
	}
	*a = (SiftStrArena) {.cx = a->cx};
}
