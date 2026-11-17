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

/* the record a run is at, as a match of the tree sees it */
static MergeEnt
merge_ent(const AcutMerge *m, int run)
{
	const int64 *x = m->head[run];

	if (x == NULL)
		return (MergeEnt) {.run = run, .end = true};
	return (MergeEnt) {.k = {x[0], m->nkeys > 1 ? x[1] : 0}, .run = run};
}

/* a is read before b: a run past its end after all, then the smaller keys, and those of equal keys in the order of the runs */
static inline bool
merge_before(const MergeEnt *a, const MergeEnt *b)
{
	if (a->end != b->end)
		return b->end;
	if (a->k[0] != b->k[0])
		return a->k[0] < b->k[0];
	if (a->k[1] != b->k[1])
		return a->k[1] < b->k[1];
	return a->run < b->run;
}

/* the run that was read replays its matches up the tree: at each node the loser stays and the winner goes on */
static void
merge_replay(AcutMerge *m, int run)
{
	MergeEnt	x = merge_ent(m, run);

	for (int node = (m->width + run) >> 1; node >= 1; node >>= 1)
		if (merge_before(&m->tree[node], &x))
		{
			MergeEnt	t = m->tree[node];

			m->tree[node] = x;
			x = t;
		}
	m->tree[0] = x;
}

void
acut_merge_begin(AcutMerge *m, const AcutSpool *s, const uint64 *bound, int nruns, int nkeys)
{
	int			width = 1;
	MergeEnt   *winner;

	while (width < nruns)
		width *= 2;
	*m = (AcutMerge) {.nruns = nruns, .nkeys = nkeys, .width = width, .last = -1};
	m->cur = palloc_array(AcutCur, Max(nruns, 1));
	m->head = palloc0_array(const void *, width);
	m->tree = palloc_array(MergeEnt, width);
	winner = palloc_array(MergeEnt, (Size) 2 * width);
	for (int r = 0; r < nruns; r++)
	{
		acut_cur_init(&m->cur[r], s, bound[r], bound[r + 1]);
		m->head[r] = acut_cur_next(&m->cur[r]);
	}
	for (int r = 0; r < width; r++)
		winner[width + r] = merge_ent(m, r);
	for (int node = width - 1; node >= 1; node--)
	{
		bool		left = merge_before(&winner[2 * node], &winner[2 * node + 1]);

		winner[node] = winner[2 * node + !left];
		m->tree[node] = winner[2 * node + left];
	}
	m->tree[0] = winner[1];
	pfree(winner);
}

const void *
acut_merge_next(AcutMerge *m)
{
	if (m->nruns == 1)
		return acut_cur_next(&m->cur[0]);
	if (m->nruns == 0)
		return NULL;
	if (m->last >= 0)
	{
		m->head[m->last] = acut_cur_next(&m->cur[m->last]);
		merge_replay(m, m->last);
	}
	if (m->tree[0].end)
		return NULL;
	m->last = m->tree[0].run;
	return m->head[m->last];
}

void
acut_merge_end(AcutMerge *m)
{
	for (int r = 0; r < m->nruns; r++)
		acut_cur_free(&m->cur[r]);
	pfree(m->cur);
	pfree(m->head);
	pfree(m->tree);
}
