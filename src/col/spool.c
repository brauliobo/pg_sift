/*
 * col/spool.c - the entries of an order array on temp files (col/spool.h). Portable (core/host.h only).
 */
#include "col/spool.h"
#include "core/cmp.h"
#include "core/limits.h"
#include "core/membudget.h"
#include "core/store.h"

static inline int
ent_cmp(const SiftEnt *x, const SiftEnt *y)
{
	if (x->key != y->key)
		return SIFT_CMP3(x->key, y->key);
	return SIFT_CMP3(x->code, y->code);
}

#define ENT_LESS(x, y) (ent_cmp(x, y) < 0)
SIFT_DEFINE_SORT(ent_sort_cmp, SiftEnt, ENT_LESS)

/* a copy of the entries for the radix sort: none when it would pass the largest allocation (the comparison sort then) */
static void *
ent_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

#define ENT_KEY(x) ((x)->key)
#define ENT_CODE(x) ((x)->code)
#define ENT_ZERO(x) ((int64) 0)
#define ENT_CODE_LESS(x, y) ((x)->code < (y)->code)
#define ENT_CODE_SIGNED(x) ((int64) ((x)->code ^ UINT64CONST(0x8000000000000000)))	/* the order of the unsigned code as a signed key */
SIFT_DEFINE_RADIX_SORT(ent_sort_by_key, SiftEnt, ENT_KEY, ENT_ZERO, ENT_CODE, ent_copy, pfree, ent_sort_cmp)
SIFT_DEFINE_SORT(ent_code_cmp, SiftEnt, ENT_CODE_LESS)
SIFT_DEFINE_RADIX_SORT(ent_by_code, SiftEnt, ENT_CODE_SIGNED, ENT_ZERO, ENT_ZERO, ent_copy, pfree, ent_code_cmp)

/* The entries by (key, code). The radix sort by key keeps the order it finds, so they must come in the order of the code: those of a segment cut by a key do not, and are put in it by a radix sort first */
static void
ent_sort(SiftEnt *a, size_t n)
{
	for (size_t i = 1; i < n; i++)
		if (a[i].code < a[i - 1].code)
		{
			ent_by_code(a, n);
			break;
		}
	ent_sort_by_key(a, n);
}

void
sift_eset_init(SiftEntSet *s, MemoryContext cxt, bool presort)
{
	*s = (SiftEntSet) {.cxt = cxt, .presort = presort};
}

#define BLOCK_BYTES		((uint64) SIFT_ESET_BLOCK * sizeof(SiftEnt))

static BufFile *
eset_file(SiftEntSet *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cxt);
	BufFile    *f = BufFileCreateTemp(false);

	MemoryContextSwitchTo(old);
	return f;
}

/* A position of a file: the component of 1 GB and the offset in it, as BufFileSeek takes it */
static void
file_seek(BufFile *f, uint64 at)
{
	SIFT_INVARIANT(BufFileSeek(f, (int) (at / SIFT_ESET_COMPONENT), (long) (at % SIFT_ESET_COMPONENT), SEEK_SET) == 0, "sift: could not seek in a spool file");
}

static SiftEntSpill *
eset_spill(SiftEntSet *s)
{
	if (s->sp == NULL)
	{
		s->sp = MemoryContextAllocZero(s->cxt, sizeof(SiftEntSpill));
		s->sp->file = eset_file(s);
	}
	return s->sp;
}

/* A run being written at the end of the file of the set */
typedef struct RunOut
{
	SiftEntSpill *sp;
	SiftEntRun	run;
} RunOut;

static RunOut
runout_begin(SiftEntSet *s)
{
	SiftEntSpill *sp = eset_spill(s);

	file_seek(sp->file, sp->flen);
	return (RunOut) {sp, {.file = sp->file, .off = sp->flen}};
}

static void
runout_put(RunOut *o, const SiftEnt *e, uint32 m)
{
	file_seek(o->sp->file, o->sp->flen);
	BufFileWrite(o->sp->file, e, (size_t) m * sizeof(SiftEnt));
	o->sp->flen += (uint64) m * sizeof(SiftEnt);
	if (o->run.n == 0)
		o->run.first = e[0];
	o->run.last = e[m - 1];
	o->run.n += m;
}

/* The run ends on a block of the file: the next begins on another, and a reader's block is one read */
static SiftEntRun
runout_end(RunOut *o)
{
	static const char zero[BLOCK_BYTES];
	size_t		rest = (size_t) (-o->sp->flen % BLOCK_BYTES);

	file_seek(o->sp->file, o->sp->flen);
	BufFileWrite(o->sp->file, zero, rest);
	o->sp->flen += rest;
	return o->run;
}

static inline bool
ent_less(const SiftEnt *a, const SiftEnt *b)
{
	return a->key < b->key || (a->key == b->key && a->code < b->code);
}

/* No entry has this key and code: the head of a run with none left */
#define ENT_END		((SiftEnt) {INT64_MAX, UINT64_MAX})

/* The champion w of the tree, whose head changed, played again against the losers on its way to the root */
static inline void
ecur_replay(SiftEntCur *c, uint32 w)
{
	for (uint32 i = (c->nheap + w) >> 1; i >= 1; i >>= 1)
	{
		uint32		l = c->lose[i];

		if (ent_less(&c->head[l], &c->head[w]))
		{
			c->lose[i] = w;
			w = l;
		}
	}
	c->lose[0] = w;
}

static void
ecur_tree(SiftEntCur *c)
{
	uint32		k = c->nheap,
				win[2 * SIFT_ESET_FANIN];

	for (uint32 j = 0; j < k; j++)
	{
		c->head[j] = c->base[c->order[j]][0];
		win[k + j] = j;
	}
	for (uint32 i = k - 1; i >= 1; i--)
	{
		bool		left = ent_less(&c->head[win[2 * i]], &c->head[win[2 * i + 1]]);

		win[i] = left ? win[2 * i] : win[2 * i + 1];
		c->lose[i] = left ? win[2 * i + 1] : win[2 * i];
	}
	c->lose[0] = win[1];
	c->nlive = k;
}

static void
ecur_fill(SiftEntCur *c, uint32 i)
{
	c->cnt[i] = (uint32) Min(c->left[i], SIFT_ESET_BLOCK);
	c->pos[i] = 0;
	file_seek(c->run[i].file, c->at[i]);
	BufFileReadExact(c->run[i].file, c->base[i], (size_t) c->cnt[i] * sizeof(SiftEnt));
	c->left[i] -= c->cnt[i];
	c->at[i] += BLOCK_BYTES;
}

/* The runs by their first entries, and whether each ends before the next begins */
static bool
ecur_order(SiftEntCur *c)
{
	uint32		k = 0;
	bool		apart = true;

	for (uint32 i = 0; i < c->nrun; i++)
		if (c->run[i].n > 0)
		{
			uint32		j = k++;

			for (; j > 0 && ent_less(&c->run[i].first, &c->run[c->order[j - 1]].first); j--)
				c->order[j] = c->order[j - 1];
			c->order[j] = i;
		}
	for (uint32 j = 1; j < k; j++)
		apart = apart && ent_less(&c->run[c->order[j - 1]].last, &c->run[c->order[j]].first);
	c->nheap = k;
	return apart;
}

static void
ecur_open_runs(SiftEntCur *c, MemoryContext cxt, const SiftEntRun *run, uint32 nrun)
{
	uint32		nfile = 0;

	SIFT_INVARIANT(nrun <= SIFT_ESET_FANIN, "sift: a merge of more runs than its fan-in");
	*c = (SiftEntCur) {.run = run, .nrun = nrun};
	for (uint32 i = 0; i < nrun; i++)
		nfile += run[i].file != NULL && run[i].n > 0;
	c->blk = nfile > 0 ? MemoryContextAlloc(cxt, (size_t) nfile * SIFT_ESET_BLOCK * sizeof(SiftEnt)) : NULL;
	c->seq = ecur_order(c);
	nfile = 0;
	for (uint32 i = 0; i < nrun; i++)
	{
		if (run[i].n == 0)
			continue;
		if (run[i].file != NULL)
		{
			c->base[i] = c->blk + (size_t) nfile++ * SIFT_ESET_BLOCK;
			c->left[i] = run[i].n;
			c->at[i] = run[i].off;
			ecur_fill(c, i);
		}
		else
		{
			c->base[i] = run[i].mem;
			c->cnt[i] = (uint32) run[i].n;
		}
	}
	SIFT_MEM_CHECKPOINT();
	if (!c->seq && c->nheap > 0)
		ecur_tree(c);
}

void
sift_ecur_open(SiftEntCur *c, const SiftEntSet *s)
{
	SIFT_INVARIANT(s->sealed, "sift: the entries of an order array are read before they are sealed");
	ecur_open_runs(c, s->cxt, s->sealed_run, s->nsealed);
}

/* The entries of the runs that do not overlap, run after run */
static uint32
ecur_read_seq(SiftEntCur *c, SiftEnt *out, uint32 m)
{
	uint32		got = 0;

	while (got < m && c->cur < c->nheap)
	{
		uint32		i = c->order[c->cur],
					k = Min(m - got, c->cnt[i] - c->pos[i]);

		memcpy(out + got, c->base[i] + c->pos[i], (size_t) k * sizeof(SiftEnt));
		got += k;
		c->pos[i] += k;
		if (c->pos[i] < c->cnt[i])
			continue;
		if (c->left[i] > 0)
			ecur_fill(c, i);
		else
			c->cur++;
	}
	return got;
}

uint32
sift_ecur_read(SiftEntCur *c, SiftEnt *out, uint32 m)
{
	uint32		got = 0;

	if (c->seq)
		return ecur_read_seq(c, out, m);
	while (got < m && c->nlive > 0)
	{
		uint32		w = c->lose[0],
					i = c->order[w];

		out[got++] = c->head[w];
		if (++c->pos[i] == c->cnt[i] && c->left[i] > 0)
			ecur_fill(c, i);
		if (c->pos[i] < c->cnt[i])
			c->head[w] = c->base[i][c->pos[i]];
		else
		{
			c->head[w] = ENT_END;
			c->nlive--;
		}
		ecur_replay(c, w);
	}
	return got;
}

void
sift_ecur_close(SiftEntCur *c)
{
	if (c->blk)
		pfree(c->blk);
	*c = (SiftEntCur) {0};
}

/* The k runs of a level merged into one run at the end of the file */
static SiftEntRun
eset_merge_runs(SiftEntSet *s, const SiftEntRun *run, uint32 k)
{
	SiftEntCur	c;
	SiftEnt    *out = MemoryContextAlloc(s->cxt, SIFT_ESET_BLOCK * sizeof(SiftEnt));
	RunOut		o;

	ecur_open_runs(&c, s->cxt, run, k);
	o = runout_begin(s);
	for (uint32 m; (m = sift_ecur_read(&c, out, SIFT_ESET_BLOCK)) > 0;)
		runout_put(&o, out, m);
	sift_ecur_close(&c);
	pfree(out);
	return runout_end(&o);
}

/* A run on its file at a level; the level that holds FANIN of them is merged into a run of the next */
static void
eset_push(SiftEntSet *s, SiftEntRun run, uint32 level)
{
	SiftEntSpill *sp = s->sp;

	SIFT_INVARIANT(level < SIFT_ESET_LEVELS, "sift: the runs of an order array fill more levels than a set holds");
	sp->lev[level][sp->nlev[level]++] = run;
	if (sp->nlev[level] == SIFT_ESET_FANIN)
	{
		SiftEntRun	m = eset_merge_runs(s, sp->lev[level], SIFT_ESET_FANIN);

		sp->nlev[level] = 0;
		eset_push(s, m, level + 1);
	}
}

/* The buffer, sorted, as a run of its own */
static void
eset_write_run(SiftEntSet *s)
{
	RunOut		o = runout_begin(s);

	ent_sort(s->buf, s->n);
	runout_put(&o, s->buf, s->n);
	s->n = 0;
	eset_push(s, runout_end(&o), 0);
}

static void
eset_write_raw(SiftEntSet *s)
{
	if (s->raw == NULL)
		s->raw = eset_file(s);
	BufFileWrite(s->raw, s->buf, (size_t) s->n * sizeof(SiftEnt));
	s->nraw += s->n;
	s->n = 0;
}

void
sift_eset_grow(SiftEntSet *s)
{
	uint32		rows = (uint32) sift_limit(eset_rows, SIFT_ESET_ROWS);

	if (s->cap < rows)
	{
		s->cap = s->cap == 0 ? Min(1024, rows) : Min(s->cap * 2, rows);
		s->buf = s->buf ? repalloc(s->buf, s->cap * sizeof(SiftEnt)) : MemoryContextAlloc(s->cxt, s->cap * sizeof(SiftEnt));
	}
	else if (s->presort)
		eset_write_run(s);
	else
		eset_write_raw(s);
}

/* Without presort: the entries on the raw file, read back a chunk at a time with their keys mapped, as sorted runs */
static void
eset_runs_from_raw(SiftEntSet *s, const int64 *map)
{
	uint32		span = (uint32) sift_limit(eset_rows, SIFT_ESET_ROWS);
	SiftEnt    *chunk;

	pfree(s->buf);
	s->buf = NULL;
	s->cap = 0;
	chunk = MemoryContextAlloc(s->cxt, (size_t) span * sizeof(SiftEnt));
	file_seek(s->raw, 0);
	for (uint64 left = s->nraw; left > 0;)
	{
		uint32		m = (uint32) Min(left, span);
		RunOut		o;

		BufFileReadExact(s->raw, chunk, (size_t) m * sizeof(SiftEnt));
		for (uint32 i = 0; map != NULL && i < m; i++)
			chunk[i].key = map[chunk[i].key];
		ent_sort(chunk, m);
		o = runout_begin(s);
		runout_put(&o, chunk, m);
		eset_push(s, runout_end(&o), 0);
		left -= m;
	}
	pfree(chunk);
	BufFileClose(s->raw);
	s->raw = NULL;
	s->nraw = 0;
}

/* The runs of the levels, the buffer last (a run in memory), at most FANIN: the lowest level that holds more than one run is merged into one of the next while there are more */
static void
eset_flatten(SiftEntSet *s)
{
	SiftEntSpill *sp = s->sp;
	uint32		total = s->n > 0;
	uint32		k = 0;

	for (uint32 l = 0; l < SIFT_ESET_LEVELS; l++)
		total += sp->nlev[l];
	for (uint32 l = 0; total > SIFT_ESET_FANIN; l = 0)
	{
		while (sp->nlev[l] < 2)
			l++;
		SIFT_INVARIANT(l + 1 < SIFT_ESET_LEVELS, "sift: the runs of an order array fill more levels than a set holds");
		sp->lev[l + 1][sp->nlev[l + 1]++] = eset_merge_runs(s, sp->lev[l], sp->nlev[l]);
		total -= sp->nlev[l] - 1;
		sp->nlev[l] = 0;
	}
	for (uint32 l = 0; l < SIFT_ESET_LEVELS; l++)
		for (uint32 i = 0; i < sp->nlev[l]; i++)
			sp->flat[k++] = sp->lev[l][i];
	if (s->n > 0)
		sp->flat[k++] = (SiftEntRun) {.mem = s->buf, .n = s->n, .first = s->buf[0], .last = s->buf[s->n - 1]};
	s->sealed_run = sp->flat;
	s->nsealed = k;
}

void
sift_eset_seal(SiftEntSet *s, const int64 *map)
{
	bool		kept = s->n > 0;

	SIFT_INVARIANT(!s->sealed, "sift: a set of entries sealed twice");
	SIFT_INVARIANT(map == NULL || !s->presort, "sift: keys mapped at the seal of a set that sorted its runs as they came");
	if (s->raw != NULL)
	{
		eset_write_raw(s);
		eset_runs_from_raw(s, map);
	}
	else
	{
		for (uint32 i = 0; map != NULL && i < s->n; i++)
			s->buf[i].key = map[s->buf[i].key];
		ent_sort(s->buf, s->n);
	}
	if (s->sp != NULL)
		eset_flatten(s);
	else
	{
		s->solo = (SiftEntRun) {.mem = s->buf, .n = s->n};
		if (s->n > 0)
		{
			s->solo.first = s->buf[0];
			s->solo.last = s->buf[s->n - 1];
		}
		s->sealed_run = &s->solo;
		s->nsealed = s->n > 0;
	}
	if (!kept && s->buf)
		pfree(s->buf);
	s->buf = NULL;
	s->cap = 0;
	s->n = 0;
	s->sealed = true;
}

void
sift_eset_free(SiftEntSet *s)
{
	for (uint32 i = 0; i < s->nsealed; i++)
		if (s->sealed_run[i].mem)
			pfree(s->sealed_run[i].mem);
	if (s->sp)
	{
		BufFileClose(s->sp->file);
		pfree(s->sp);
	}
	if (s->raw)
		BufFileClose(s->raw);
	if (s->buf)
		pfree(s->buf);
	*s = (SiftEntSet) {.cxt = s->cxt, .presort = s->presort};
}

void
sift_esrc_open(SiftEntSrc *r, const SiftEntSet *vals, const SiftEntSet *nulls)
{
	*r = (SiftEntSrc) {.vals = vals, .nulls = nulls};
	sift_ecur_open(&r->cur, vals);
}

uint32
sift_esrc_read(SiftEntSrc *r, SiftEnt *out, uint32 m)
{
	uint32		got = sift_ecur_read(&r->cur, out, m);

	if (got < m && !r->innulls)
	{
		sift_ecur_close(&r->cur);
		sift_ecur_open(&r->cur, r->nulls);
		r->innulls = true;
		got += sift_ecur_read(&r->cur, out + got, m - got);
	}
	return got;
}

void
sift_esrc_close(SiftEntSrc *r)
{
	sift_ecur_close(&r->cur);
}

void
sift_blob_init(SiftBlob *b, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);

	*b = (SiftBlob) {.cxt = cxt};
	initStringInfo(&b->head);
	initStringInfo(&b->tail);
	MemoryContextSwitchTo(old);
}

void
sift_blob_free(SiftBlob *b)
{
	if (b->file)
		BufFileClose(b->file);
	if (b->head.data)
		pfree(b->head.data);
	if (b->tail.data)
		pfree(b->tail.data);
	*b = (SiftBlob) {0};
}
