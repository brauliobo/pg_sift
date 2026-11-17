/*
 * col/autocut/spool.c - arrays of fixed-size records that keep to a budget of memory and put the rest on a temporary file
 *
 * The sample of the decision of key_cut, its sorted runs and the cells of its keys are written once, read front to back, and may be larger than the memory the
 * decision may take (maintenance_work_mem): a spool keeps its chunks in memory while the shared budget lasts and writes the following ones to a temporary file. A
 * reader that meets a chunk on the file reads it into a buffer of its own, so that many readers (one for each run of a merge) can walk one spool.
 */
#include "postgres.h"
#include "storage/buffile.h"
#include "col/autocut/autocut_int.h"

void
acut_spool_init(AcutSpool *s, AcutBudget *budget, uint32 recsz)
{
	*s = (AcutSpool) {.budget = budget, .recsz = recsz, .per = AUTOCUT_CHUNK / recsz};
	Assert(recsz > 0 && recsz <= AUTOCUT_CHUNK && recsz % sizeof(uint64) == 0);
}

void *
acut_spool_slot(AcutSpool *s)
{
	if (s->tail == NULL)
		s->tail = palloc(AUTOCUT_CHUNK);
	return s->tail + (size_t) s->tailn * s->recsz;
}

void
acut_spool_add(AcutSpool *s, const void *rec)
{
	memcpy(acut_spool_slot(s), rec, s->recsz);
	acut_spool_commit(s);
}

void
acut_spool_commit(AcutSpool *s)
{
	s->n++;
	if (++s->tailn < s->per)
		return;
	if (s->file == NULL && s->budget->left >= AUTOCUT_CHUNK_COST)
	{
		if (s->nchunk == s->capchunk)
		{
			s->capchunk = s->capchunk ? s->capchunk * 2 : 64;
			s->chunk = s->chunk ? repalloc_array(s->chunk, char *, s->capchunk) : palloc_array(char *, s->capchunk);
		}
		s->chunk[s->nchunk++] = s->tail;
		s->budget->left -= AUTOCUT_CHUNK_COST;
		s->tail = NULL;
	}
	else
	{
		if (s->file == NULL)
			s->file = BufFileCreateTemp(false);
		BufFileWrite(s->file, s->tail, AUTOCUT_CHUNK);
		s->nspill++;
	}
	s->tailn = 0;
}

void
acut_spool_free(AcutSpool *s)
{
	for (uint32 i = 0; i < s->nchunk; i++)
		pfree(s->chunk[i]);
	s->budget->left += (int64) s->nchunk * AUTOCUT_CHUNK_COST;
	if (s->chunk != NULL)
		pfree(s->chunk);
	if (s->tail != NULL)
		pfree(s->tail);
	if (s->file != NULL)
		BufFileClose(s->file);
	*s = (AcutSpool) {.budget = s->budget, .recsz = s->recsz, .per = s->per};
}

void
acut_cur_init(AcutCur *c, const AcutSpool *s, uint64 from, uint64 to)
{
	*c = (AcutCur) {.s = s, .pos = from, .end = to, .bufchunk = -1};
}

void
acut_cur_seek(AcutCur *c, uint64 from, uint64 to)
{
	c->pos = from;
	c->end = to;
	c->left = 0;
	c->nwin = 0;
}

void
acut_cur_free(AcutCur *c)
{
	if (c->buf != NULL)
		pfree(c->buf);
	c->buf = NULL;
}

/* the cursor read its window: the next one, the records from there to the end of the chunk or of the range, and the first of them; NULL past the range */
const void *
acut_cur_window(AcutCur *c)
{
	const AcutSpool *s = c->s;
	uint64		chunk,
				at;
	const char *base;

	c->pos += c->nwin;
	c->nwin = 0;
	if (c->pos >= c->end)
		return NULL;
	chunk = c->pos / s->per;
	at = c->pos % s->per;
	if (chunk < s->nchunk)
		base = s->chunk[chunk];
	else if (chunk < (uint64) s->nchunk + s->nspill)
	{
		if (c->buf == NULL)
			c->buf = palloc(AUTOCUT_CHUNK);
		if (c->bufchunk != (int64) chunk)
		{
			BufFileSeekBlock(s->file, (int64) (chunk - s->nchunk) * (AUTOCUT_CHUNK / BLCKSZ));
			BufFileReadExact(s->file, c->buf, AUTOCUT_CHUNK);
			c->bufchunk = chunk;
		}
		base = c->buf;
	}
	else
		base = s->tail;
	c->nwin = Min(s->per - at, c->end - c->pos);
	c->p = base + at * s->recsz;
	c->left = c->nwin;
	return acut_cur_next(c);
}

static inline int
merge_cmp(const AcutMerge *m, int a, int b)
{
	return m->cmp(m->head[a], m->head[b]);
}

static void
merge_down(AcutMerge *m, int i)
{
	int			x = m->heap[i];

	for (;;)
	{
		int			child = 2 * i + 1;

		if (child >= m->nheap)
			break;
		if (child + 1 < m->nheap && merge_cmp(m, m->heap[child + 1], m->heap[child]) < 0)
			child++;
		if (merge_cmp(m, m->heap[child], x) >= 0)
			break;
		m->heap[i] = m->heap[child];
		i = child;
	}
	m->heap[i] = x;
}

void
acut_merge_begin(AcutMerge *m, const AcutSpool *s, const uint64 *bound, int nruns, int (*cmp) (const void *, const void *))
{
	*m = (AcutMerge) {.cmp = cmp, .last = -1, .nheap = 0};
	m->cur = palloc_array(AcutCur, Max(nruns, 1));
	m->head = palloc_array(const void *, Max(nruns, 1));
	m->heap = palloc_array(int, Max(nruns, 1));
	for (int r = 0; r < nruns; r++)
	{
		acut_cur_init(&m->cur[r], s, bound[r], bound[r + 1]);
		m->head[r] = acut_cur_next(&m->cur[r]);
		if (m->head[r] != NULL)
			m->heap[m->nheap++] = r;
	}
	m->nruns = nruns;
	for (int i = m->nheap / 2; i-- > 0;)
		merge_down(m, i);
}

const void *
acut_merge_next(AcutMerge *m)
{
	if (m->nruns == 1)
		return acut_cur_next(&m->cur[0]);
	if (m->last >= 0)
	{
		m->head[m->last] = acut_cur_next(&m->cur[m->last]);
		if (m->head[m->last] == NULL)
			m->heap[0] = m->heap[--m->nheap];
		if (m->nheap > 0)
			merge_down(m, 0);
	}
	if (m->nheap == 0)
		return NULL;
	m->last = m->heap[0];
	return m->head[m->last];
}

void
acut_merge_end(AcutMerge *m)
{
	for (int r = 0; r < m->nruns; r++)
		acut_cur_free(&m->cur[r]);
	pfree(m->cur);
	pfree(m->head);
	pfree(m->heap);
}
