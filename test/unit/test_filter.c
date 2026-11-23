/*
 * test_filter.c - the filters of a granule on pool threads (col/granule/granule_filter_a.c): four workers filter every granule of the fixture by equality, IN, range, prefix, LIKE '%lit%', NOT
 * LIKE and the NULL tests, in an arena of their own that they rewind between granules, on zstd, LZ4, dictionary, FSST, view and integer chunks, at every SIMD level the CPU runs. The masks are
 * those of the backend path on the setup thread (and of a bytewise test of the decoded values), the strings the filter keeps are those of the decode, a damaged chunk is an error as data, and an
 * arena that is full is SIFT_ERR_ARENA_FULL, with the palloc/ereport trap of trapshim.h armed on every worker.
 */
#include "postgres.h"

#include "lib/stringinfo.h"

#include "col/codec.h"
#include "col/compress.h"
#include "col/granule/cfilt_cc.h"
#include "col/granule/cfilt_str_simd.h"
#include "col/granule/granule_filter.h"
#include "core/par/arena.h"
#include "core/cpu.h"
#include "granule_fixture.h"
#include "pgshim.h"
#include "unit.h"

#define WORKERS		4
#define GRANULES	10
#define ARENA_BYTES	(2 * 1024 * 1024)
#define SET_KEYS	12
#define ENC_BIT(e)	(UINT64CONST(1) << (e))
#define ENCS_REQUIRED (ENC_BIT(SIFT_CENC_INT_FOR) | ENC_BIT(SIFT_CENC_INT_DELTA) | ENC_BIT(SIFT_CENC_INT_CONST) | ENC_BIT(SIFT_CENC_DICT_CODE) | \
					   ENC_BIT(SIFT_CENC_STR_INLINE) | ENC_BIT(SIFT_CENC_BOOL_BITS) | ENC_BIT(SIFT_CENC_RLE) | ENC_BIT(SIFT_CENC_ALP) | \
					   ENC_BIT(SIFT_CENC_STR_VIEW) | ENC_BIT(SIFT_CENC_STR_FSST) | ENC_BIT(SIFT_CENC_STR_BLOCK))

typedef struct Job
{
	int			g;
	int			col;
	SiftColPred pred;
	SiftCfiltPred cp;
	const uint64 *live;
	const uint16 *lrows;
	uint16		nlrows;
	SiftCfiltResult want;
	uint64		mask[SIFT_CFILT_WORDS];
	int			memo_n;
} Job;

typedef struct Suite
{
	const GranuleFixture *fx;
	Job		  **jobs;
	int			njobs;
	SiftArena  *arena[WORKERS];
} Suite;

static uint64 rng_state = 0x9E3779B97F4A7C15ULL;
static uint64 live_sparse[SIFT_CFILT_WORDS],
			live_alt[SIFT_CFILT_WORDS];
static uint16 rows_sparse[FX_ROWS],
			rows_alt[FX_ROWS];
static uint16 nrows_sparse,
			nrows_alt;

static uint32
rnd(uint32 n)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32) ((rng_state >> 11) % n);
}

static void
live_make(uint64 *bits, uint16 *rows, uint16 *nrows, int stride)
{
	for (int r = 0; r < FX_ROWS; r += stride)
	{
		bits[r >> 6] |= UINT64CONST(1) << (r & 63);
		rows[(*nrows)++] = r;
	}
}

static int
str_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	return c != 0 ? c : (alen > blen) - (alen < blen);
}

static bool
str_equal(const SiftColValue *v, const char *s, uint32 len)
{
	return v->len == len && memcmp(v->v.s, s, len) == 0;
}

static bool
str_hit(const SiftColPred *p, const char *s, uint32 len)
{
	const SiftColValue *v = p->vals;
	bool		in = false;

	for (int i = 0; i < p->nvals; i++)
		in |= str_equal(&v[i], s, len);
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_IN:
			return in;
		case SIFT_COLOP_NE:
		case SIFT_COLOP_NOTIN:
			return !in;
		case SIFT_COLOP_LT:
			return str_cmp(s, len, v->v.s, v->len) < 0;
		case SIFT_COLOP_GE:
			return str_cmp(s, len, v->v.s, v->len) >= 0;
		case SIFT_COLOP_PREFIX:
			return len >= v->len && memcmp(s, v->v.s, v->len) == 0;
		case SIFT_COLOP_CONTAINS:
			return memmem(s, len, v->v.s, v->len) != NULL;
		default:
			return memmem(s, len, v->v.s, v->len) == NULL;
	}
}

static bool
num_hit(const SiftColPred *p, double f, int64 i, bool is_f8)
{
	const SiftColValue *v = p->vals;
	bool		in = false;

	for (int k = 0; k < p->nvals; k++)
		in |= is_f8 ? v[k].v.f == f : v[k].v.i == i;
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_IN:
			return in;
		case SIFT_COLOP_NE:
		case SIFT_COLOP_NOTIN:
			return !in;
		case SIFT_COLOP_LT:
			return is_f8 ? f < v->v.f : i < v->v.i;
		case SIFT_COLOP_LE:
			return is_f8 ? f <= v->v.f : i <= v->v.i;
		case SIFT_COLOP_GT:
			return is_f8 ? f > v->v.f : i > v->v.i;
		default:
			return is_f8 ? f >= v->v.f : i >= v->v.i;
	}
}

static bool
row_expected(const Job *j, const FxRef *ref, SiftCellKind kind, int r)
{
	if (j->pred.op == SIFT_COLOP_ISNULL || j->pred.op == SIFT_COLOP_NOTNULL)
		return ref->isnull[r] == (j->pred.op == SIFT_COLOP_ISNULL);
	if (ref->isnull[r])
		return false;
	if (kind == SIFT_CELL_STR)
		return str_hit(&j->pred, ref->s[r].p, ref->s[r].len);
	return num_hit(&j->pred, kind == SIFT_CELL_F8 ? ref->f[r] : 0, kind == SIFT_CELL_INT ? ref->i[r] : 0, kind == SIFT_CELL_F8);
}

static bool
row_live(const Job *j, int r)
{
	return j->live == NULL || ((j->live[r >> 6] >> (r & 63)) & 1);
}

static bool
row_passes(SiftCfiltResult res, const uint64 *mask, int r)
{
	return res == SIFT_CFILT_ALL_TRUE || (res == SIFT_CFILT_BITS && ((mask[r >> 6] >> (r & 63)) & 1));
}

static void
check_masks_equal(const Job *j, SiftCfiltResult res, const uint64 *mask)
{
	CHECK(res == j->want);
	if (res == SIFT_CFILT_BITS)
		for (int r = 0; r < FX_ROWS; r++)
			CHECK(!row_live(j, r) || row_passes(res, mask, r) == row_passes(j->want, j->mask, r));
}

static void
check_semantics(const GranuleFixture *fx, const Job *j)
{
	const FxRef *ref = &fx->g[j->g].ref[j->col];
	SiftCellKind kind = fx->cols[j->col].kind;

	if (j->want == SIFT_CFILT_UNSUPPORTED)
		return;
	for (int r = 0; r < FX_ROWS; r++)
		CHECK(!row_live(j, r) || row_passes(j->want, j->mask, r) == row_expected(j, ref, kind, r));
}

static SiftColValue *
values_alloc(int n)
{
	return palloc0(n * sizeof(SiftColValue));
}

static void
set_str(SiftColValue *v, const char *s, uint32 len)
{
	v->v.s = memcpy(palloc(len + 1), s, len);
	v->len = len;
}

static const SiftStrCell *
pick_str(const FxRef *ref)
{
	for (;;)
	{
		const SiftStrCell *c = &ref->s[rnd(FX_ROWS)];

		if (!ref->isnull[c - ref->s] && c->len >= 4)
			return c;
	}
}

static bool
has_value(const FxRef *ref, SiftCellKind kind)
{
	for (int r = 0; r < FX_ROWS; r++)
		if (!ref->isnull[r] && (kind != SIFT_CELL_STR || ref->s[r].len >= 4))
			return true;
	return false;
}

typedef struct Spec
{
	SiftColOp	op;
	int			nvals;
	int			pick;			/* what the constants are: 0 values of the column, 1 a piece of one, 2 bytes no value holds */
} Spec;

static const Spec str_specs[] = {
	{SIFT_COLOP_CONTAINS, 1, 1}, {SIFT_COLOP_CONTAINS, 1, 2}, {SIFT_COLOP_NOTCONTAINS, 1, 1}, {SIFT_COLOP_EQ, 1, 0}, {SIFT_COLOP_NE, 1, 0},
	{SIFT_COLOP_IN, 2, 0}, {SIFT_COLOP_IN, 9, 0}, {SIFT_COLOP_NOTIN, 3, 0}, {SIFT_COLOP_PREFIX, 1, 1}, {SIFT_COLOP_LT, 1, 0}, {SIFT_COLOP_GE, 1, 0},
	{SIFT_COLOP_ISNULL, 0, 0}, {SIFT_COLOP_NOTNULL, 0, 0}
};

static const Spec num_specs[] = {
	{SIFT_COLOP_EQ, 1, 0}, {SIFT_COLOP_NE, 1, 0}, {SIFT_COLOP_LT, 1, 0}, {SIFT_COLOP_LE, 1, 0}, {SIFT_COLOP_GT, 1, 0}, {SIFT_COLOP_GE, 1, 0},
	{SIFT_COLOP_IN, 2, 0}, {SIFT_COLOP_IN, SET_KEYS, 0}, {SIFT_COLOP_NOTIN, SET_KEYS, 0}, {SIFT_COLOP_ISNULL, 0, 0}, {SIFT_COLOP_NOTNULL, 0, 0}
};

static void
str_constants(const FxRef *ref, const Spec *sp, SiftColValue *vals)
{
	for (int k = 0; k < sp->nvals; k++)
	{
		const SiftStrCell *c = pick_str(ref);

		if (sp->pick == 0)
			set_str(&vals[k], c->p, c->len);
		else if (sp->pick == 1 && sp->op == SIFT_COLOP_PREFIX)
			set_str(&vals[k], c->p, 3);
		else if (sp->pick == 1)
			set_str(&vals[k], c->p + rnd(c->len - 3), 4);
		else
			set_str(&vals[k], "\x01\x02\x03\x01", 4);
	}
}

static void
num_constants(const FxRef *ref, SiftCellKind kind, const Spec *sp, SiftColValue *vals)
{
	for (int k = 0; k < sp->nvals; k++)
	{
		int			r = rnd(FX_ROWS);

		while (ref->isnull[r])
			r = rnd(FX_ROWS);
		if (kind == SIFT_CELL_F8)
			vals[k].v.f = ref->f[r];
		else
			vals[k].v.i = ref->i[r];
	}
}

static uint64 encs_filtered;

static uint16
chunk_enc(const GranuleFixture *fx, const Job *j)
{
	SiftChunk	chunk;
	uint16		enc;

	sift_chunk_open(&fx->g[j->g].view, j->col + 1, &chunk);
	enc = chunk.enc;
	sift_chunk_close(&chunk);
	return enc;
}

/* The chunks of strings that a filter decodes whole keep them for the view: a block for <>, LIKE and NOT LIKE, a FSST chunk for LIKE and NOT LIKE */
static int
expect_memo(const GranuleFixture *fx, const Job *j)
{
	const FxRef *ref = &fx->g[j->g].ref[j->col];
	SiftColOp	op = j->pred.op;
	uint16		enc = chunk_enc(fx, j);
	int			live_nn = 0;

	if (j->want == SIFT_CFILT_UNSUPPORTED)
		return 0;
	encs_filtered |= ENC_BIT(enc);
	for (int r = 0; r < FX_ROWS; r++)
		live_nn += row_live(j, r) && !ref->isnull[r];
	if (live_nn == 0)
		return 0;
	if (enc == SIFT_CENC_STR_BLOCK)
		return op == SIFT_COLOP_CONTAINS || op == SIFT_COLOP_NOTCONTAINS || op == SIFT_COLOP_NE;
	return enc == SIFT_CENC_STR_FSST && (op == SIFT_COLOP_CONTAINS || op == SIFT_COLOP_NOTCONTAINS);
}

static void
job_add(Suite *s, int *cap, int g, int col, const Spec *sp, int variant)
{
	const GranuleFixture *fx = s->fx;
	const FxRef *ref = &fx->g[g].ref[col];
	SiftCellKind kind = fx->cols[col].kind;
	SiftColValue *vals = values_alloc(Max(sp->nvals, 1));
	Job		   *j = palloc0(sizeof(Job));

	if (ref->n != FX_ROWS || (sp->nvals > 0 && !has_value(ref, kind)))
		return;
	if (kind == SIFT_CELL_STR)
		str_constants(ref, sp, vals);
	else
		num_constants(ref, kind, sp, vals);
	j->g = g;
	j->col = col;
	j->pred = (SiftColPred) {.col = col,.op = sp->op,.nvals = sp->nvals,.vals = vals};
	if (!sift_cfilt_compile(&j->cp, &fx->defs[col], &j->pred))
		return;
	j->live = variant == 1 ? live_sparse : variant == 2 ? live_alt : NULL;
	j->lrows = variant == 1 ? rows_sparse : variant == 2 ? rows_alt : NULL;
	j->nlrows = variant == 1 ? nrows_sparse : variant == 2 ? nrows_alt : FX_ROWS;
	j->want = sift_granule_filter_compressed_live(&fx->g[g].view, &j->cp, j->live, j->mask);
	check_semantics(fx, j);
	j->memo_n = expect_memo(fx, j);
	if (s->njobs == *cap)
	{
		*cap *= 2;
		s->jobs = repalloc(s->jobs, *cap * sizeof(Job *));
	}
	s->jobs[s->njobs++] = j;
}

static void
jobs_build(Suite *s)
{
	int			cap = 256;

	s->jobs = palloc(cap * sizeof(Job *));
	live_make(live_sparse, rows_sparse, &nrows_sparse, 17);
	live_make(live_alt, rows_alt, &nrows_alt, 2);
	for (int g = 0; g < s->fx->ngranules; g++)
		for (int c = 0; c < s->fx->ncols; c++)
		{
			SiftCellKind kind = s->fx->cols[c].kind;
			int			n = kind == SIFT_CELL_STR ? lengthof(str_specs) : lengthof(num_specs);

			for (int k = 0; k < n; k++)
				job_add(s, &cap, g, c, kind == SIFT_CELL_STR ? &str_specs[k] : &num_specs[k], (g + c + k) % 3);
		}
}

typedef struct MemoCheck
{
	const Suite *s;
	const Job  *j;
	const SiftStrMemo *memo;
	SiftStrCell *out;
	bool		found;
} MemoCheck;

static bool
memo_strs_a(SiftArena *a, SiftErr *err, MemoCheck *m)
{
	SIFT_ARENA_TRY(a, err)
		m->found = sift_cfstr_memo_strs_in(m->memo, &m->s->fx->g[m->j->g].view, m->j->col, m->j->lrows, m->j->nlrows, m->out);
	SIFT_ARENA_END
}

static void
check_memo(const Suite *s, const Job *j, SiftArena *a, const SiftStrMemo *memo)
{
	SiftStrCell out[FX_ROWS];
	MemoCheck	m = {s, j, memo, out};
	SiftErr		err;
	const FxRef *ref = &s->fx->g[j->g].ref[j->col];

	CHECK(memo->n == j->memo_n);
	if (memo->n == 0)
		return;
	CHECK(memo_strs_a(a, &err, &m) && m.found);
	for (int k = 0; k < j->nlrows; k++)
	{
		int			r = j->lrows != NULL ? j->lrows[k] : k;

		CHECK(out[k].isnull == ref->isnull[r]);
		CHECK(out[k].isnull || (out[k].len == ref->s[r].len && memcmp(out[k].p, ref->s[r].p, out[k].len) == 0));
	}
}

static void
filter_worker(void *arg, int w)
{
	Suite	   *s = arg;
	SiftArena  *a = s->arena[w];

	for (int k = 0; k < s->njobs; k++)
	{
		const Job  *j = s->jobs[(k + w * (s->njobs / WORKERS)) % s->njobs];
		SiftStrMemo memo = {0};
		SiftErr		err;
		SiftCfiltResult res;
		uint64		mask[SIFT_CFILT_WORDS];

		sift_arena_reset(a);
		CHECK(sift_granule_filter_compressed_live_a(a, &err, &memo, &s->fx->g[j->g].view, &j->cp, j->live, mask, &res));
		check_masks_equal(j, res, mask);
		check_memo(s, j, a, &memo);
	}
	sift_colz_thread_fini();
}

typedef struct Trial
{
	const Job  *job;
	SiftGranuleView view;
	bool		ok[WORKERS];
	SiftErr		err[WORKERS];
	SiftCfiltResult res[WORKERS];
	uint64		mask[WORKERS][SIFT_CFILT_WORDS];
	SiftCfiltResult pg_res;
	uint64		pg_mask[SIFT_CFILT_WORDS];
} Trial;

typedef struct Trials
{
	Trial	   *t;
	int			n;
	SiftArena  *arena[WORKERS];
} Trials;

static void
trial_worker(void *arg, int w)
{
	Trials	   *ts = arg;

	for (int i = 0; i < ts->n; i++)
	{
		Trial	   *t = &ts->t[i];
		SiftStrMemo memo = {0};

		sift_arena_reset(ts->arena[w]);
		t->ok[w] = sift_granule_filter_compressed_live_a(ts->arena[w], &t->err[w], &memo, &t->view, &t->job->cp, t->job->live, t->mask[w], &t->res[w]);
	}
	sift_colz_thread_fini();
}

static void
trial_backend(void *arg)
{
	Trial	   *t = arg;

	t->pg_res = sift_granule_filter_compressed_live(&t->view, &t->job->cp, t->job->live, t->pg_mask);
}

/* A copy of the record of the granule of the job with 16 bytes of the body of the column (or of its wrapper) overwritten */
static SiftGranuleView
damaged_view(const GranuleFixture *fx, const Job *j, int where)
{
	const FxGranule *g = &fx->g[j->g];
	const SiftChunkRef *ref = &g->view.hdr->chunks[j->col + 1];
	SiftGranuleView v = g->view;
	SiftChunk	c;
	char	   *rec = memcpy(palloc(g->len), g->rec, g->len);
	uint32		at,
				room;

	sift_chunk_open(&g->view, j->col + 1, &c);
	if (ref->flags & SIFT_CHUNK_COMPRESSED)
	{
		at = ref->off + (where ? ref->len / 2 : 8);
		room = ref->len - (where ? ref->len / 2 : 8);
	}
	else
	{
		at = ref->off + (uint32) (c.body - c.raw) + (where ? c.body_len / 2 : 0);
		room = c.body_len - (where ? c.body_len / 2 : 0);
	}
	sift_chunk_close(&c);
	memset(rec + at, 0xFF, Min(16, room));
	v.rec = rec;
	v.hdr = (const SiftGranuleHeader *) rec;
	return v;
}

static bool
damageable(const GranuleFixture *fx, const Job *j)
{
	uint16		enc = chunk_enc(fx, j);

	return j->want != SIFT_CFILT_UNSUPPORTED && j->pred.op != SIFT_COLOP_ISNULL && j->pred.op != SIFT_COLOP_NOTNULL && enc != SIFT_CENC_INT_CONST && enc != SIFT_CENC_BOOL_BITS &&
		enc != SIFT_CENC_ALL_NULL;
}

static Trials
trials_make(const Suite *s, bool damage)
{
	Trials		ts = {.t = palloc0(s->njobs * 2 * sizeof(Trial))};
	bool	   *seen = palloc0(s->fx->ngranules * s->fx->ncols * sizeof(bool));

	for (int i = 0; i < s->njobs; i++)
	{
		const Job  *j = s->jobs[i];

		if (seen[j->g * s->fx->ncols + j->col] || (damage && !damageable(s->fx, j)))
			continue;
		seen[j->g * s->fx->ncols + j->col] = true;
		for (int where = 0; where < (damage ? 2 : 1); where++)
			ts.t[ts.n++] = (Trial) {.job = j,.view = damage ? damaged_view(s->fx, j, where) : s->fx->g[j->g].view};
	}
	return ts;
}

static void
trials_run(Trials *ts, size_t arena_bytes)
{
	for (int w = 0; w < WORKERS; w++)
		ts->arena[w] = sift_arena_create(CurrentMemoryContext, arena_bytes);
	unit_run_workers(getenv("FILTER_DEV") ? 1 : WORKERS, trial_worker, ts);
}

static void
check_same_mask(const Job *j, SiftCfiltResult res, const uint64 *mask, SiftCfiltResult want, const uint64 *want_mask)
{
	CHECK(res == want);
	for (int r = 0; r < FX_ROWS && res == SIFT_CFILT_BITS; r++)
		CHECK(!row_live(j, r) || row_passes(res, mask, r) == row_passes(want, want_mask, r));
}

/* A damaged chunk is what the backend path makes of it: the same result, or the same error as data (a worker never raises it) */
static void
test_damage(const Suite *s)
{
	Trials		ts = trials_make(s, true);
	int			errors = 0;

	trials_run(&ts, ARENA_BYTES);
	for (int i = 0; i < ts.n; i++)
	{
		Trial	   *t = &ts.t[i];
		bool		backend_ok = pgshim_try(trial_backend, t);

		for (int w = 0; w < (getenv("FILTER_DEV") ? 1 : WORKERS); w++)
		{
			CHECK(t->ok[w] == backend_ok);
			if (!backend_ok)
				CHECK(t->err[w].code == SIFT_ERR_CORRUPT && strncmp(t->err[w].msg, pgshim_errmsg, sizeof(t->err[w].msg) - 1) == 0);
			else
				check_same_mask(t->job, t->res[w], t->mask[w], t->pg_res, t->pg_mask);
		}
		CHECK(backend_ok || pgshim_sqlstate == ERRCODE_DATA_CORRUPTED);
		errors += !backend_ok;
	}
	CHECK(errors > 0);
	printf("test_filter: %d damaged chunks, %d are errors as data\n", ts.n, errors);
}

/* An arena too small for what a filter allocates is SIFT_ERR_ARENA_FULL for the leader to take over, and the filters that allocate nothing still run */
static void
test_arena_full(const Suite *s)
{
	Trials		ts = trials_make(s, false);
	int			full = 0;

	trials_run(&ts, 256);
	for (int i = 0; i < ts.n; i++)
	{
		const Trial *t = &ts.t[i];

		for (int w = 0; w < (getenv("FILTER_DEV") ? 1 : WORKERS); w++)
		{
			if (t->ok[w])
				check_same_mask(t->job, t->res[w], t->mask[w], t->job->want, t->job->mask);
			else
				CHECK(t->err[w].code == SIFT_ERR_ARENA_FULL && t->err[w].arg0 > t->err[w].arg1);
		}
		full += !t->ok[0];
	}
	CHECK(full > 0);
	printf("test_filter: %d of %d (granule, column) filters need more than 256 bytes\n", full, ts.n);
}

/* The distinct levels the CPU runs, by the kernel tables they pick (the dispatch of tools/colcodec/ccshim.c) */
static int
levels(SiftSimdLevel lv[3])
{
	int			n = 0;

	for (int l = SIFT_SIMD_SCALAR; l <= (int) sift_cpu.detected; l++)
		if (!n || sift_cc_kernels_for(l) != sift_cc_kernels_for(lv[n - 1]) || sift_col_kernels_for(l) != sift_col_kernels_for(lv[n - 1]) ||
			(l >= SIFT_SIMD_AVX2) != (lv[n - 1] >= SIFT_SIMD_AVX2))
			lv[n++] = l;
	return n;
}

static void
level_use(SiftSimdLevel l)
{
	sift_cpu.active = l;
	sift_cck = sift_cc_kernels_for(l);
	sift_colk = sift_col_kernels_for(l);
}

/* ---- the kernels on the chunks of the codec: sets and ranges of keys, on every node the writer makes of five shapes of keys and six policies ---- */

#define CC_ROWS		3000
#define CC_TESTS	8

typedef struct CcCase
{
	SiftCc	   *cc;
	int64	   *keys;
	SiftCfTest	tests[CC_TESTS];
	bool		handled[CC_TESTS];
	uint64	   *want[CC_TESTS];
} CcCase;

typedef struct CcSuite
{
	CcCase	   *cases;
	int			n;
	SiftArena  *arena[WORKERS];
} CcSuite;

static void
cc_keys(int shape, int64 *k)
{
	for (int i = 0; i < CC_ROWS; i++)
		switch (shape)
		{
			case 0:
				k[i] = 7 + 3 * (int64) i;
				break;
			case 1:
				k[i] = rnd(1000);
				break;
			case 2:
				k[i] = 5000 + (i / 50) % 7 * 11;
				break;
			case 3:
				k[i] = ((int64) rnd(20) + 1) * 1000003 - 4000000000LL;
				break;
			default:
				k[i] = 42;
		}
}

static bool
key_in_set(const SiftCfTest *t, int64 key)
{
	for (uint32 i = 0; i < t->nset; i++)
		if (t->set[i] == key)
			return true;
	return false;
}

static bool
key_passes(const SiftCfTest *t, int64 key)
{
	bool		in = t->kind == SIFT_CFT_SET || t->kind == SIFT_CFT_NSET ? key_in_set(t, key) : key >= t->lo && key <= t->hi;

	return t->kind == SIFT_CFT_NSET || t->kind == SIFT_CFT_NRANGE ? !in : in;
}

static int
cmp_i64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

static int
unique_i64(int64 *v, int n)
{
	int			m = 0;

	for (int i = 0; i < n; i++)
		if (m == 0 || v[i] != v[m - 1])
			v[m++] = v[i];
	return m;
}

static void
cc_tests(CcCase *c)
{
	for (int t = 0; t < CC_TESTS; t++)
	{
		SiftCfTest *ct = &c->tests[t];
		int64	   *set = palloc(40 * sizeof(int64));
		int			n = 12 + rnd(28);

		for (int i = 0; i < n; i++)
			set[i] = i % 3 == 0 ? c->keys[rnd(CC_ROWS)] : c->keys[rnd(CC_ROWS)] + (int64) rnd(5) - 2;
		qsort(set, n, sizeof(int64), cmp_i64);
		n = unique_i64(set, n);
		ct->kind = (SiftCfKind[]) {SIFT_CFT_SET, SIFT_CFT_NSET, SIFT_CFT_RANGE, SIFT_CFT_NRANGE}[t % 4];
		ct->set = set;
		ct->nset = n;
		ct->lo = c->keys[rnd(CC_ROWS)] - rnd(3);
		ct->hi = ct->lo + (int64) rnd(300);
		c->want[t] = palloc0(SIFT_COL_NWORDS(CC_ROWS) * sizeof(uint64));
		c->handled[t] = sift_cf_cc_filter(c->cc, ct, c->want[t]);
		for (int i = 0; i < CC_ROWS && c->handled[t]; i++)
			CHECK(key_passes(ct, c->keys[i]) == ((c->want[t][i >> 6] >> (i & 63)) & 1));
	}
}

static void
cc_worker(void *arg, int w)
{
	CcSuite    *s = arg;
	SiftArena  *a = s->arena[w];
	for (int i = 0; i < s->n; i++)
		for (int t = 0; t < CC_TESTS; t++)
		{
			const CcCase *c = &s->cases[i];
			uint64		bits[SIFT_COL_NWORDS(CC_ROWS)];
			SiftErr		err;
			bool		handled;

			sift_arena_reset(a);
			CHECK(sift_cf_cc_filter_a(a, &err, c->cc, &c->tests[t], bits, &handled));
			CHECK(handled == c->handled[t] && (!handled || memcmp(bits, c->want[t], sizeof(bits)) == 0));
		}
	sift_colz_thread_fini();
}

static void
test_cc_kernels(void)
{
	static const char *const policies[] = {"auto", "for", "delta", "rle", "dict", "for+zstd"};
	CcSuite		s = {.cases = palloc0(5 * lengthof(policies) * sizeof(CcCase))};
	uint64		traps = sift_test_trap_count();
	SiftSimdLevel lv[3];
	int			nlv = levels(lv),
				handled = 0;

	level_use(lv[0]);
	for (int shape = 0; shape < 5; shape++)
		for (int p = 0; p < (int) lengthof(policies); p++)
		{
			CcCase	   *c = &s.cases[s.n];
			SiftCcPolicy pol;
			StringInfoData blob;

			CHECK(sift_cc_policy_parse(policies[p], &pol) == NULL);
			c->keys = palloc(CC_ROWS * sizeof(int64));
			cc_keys(shape, c->keys);
			initStringInfo(&blob);
			sift_cc_encode(&blob, SIFT_CC_INT, c->keys, CC_ROWS, NULL, &pol, NULL);
			c->cc = sift_cc_open(CurrentMemoryContext, (const uint8 *) blob.data, blob.len, true);
			cc_tests(c);
			for (int t = 0; t < CC_TESTS; t++)
				handled += c->handled[t];
			s.n++;
		}
	CHECK(handled > 0);
	for (int w = 0; w < WORKERS; w++)
		s.arena[w] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	for (int i = 0; i < nlv; i++)
	{
		level_use(lv[i]);
		unit_run_workers(getenv("FILTER_DEV") ? 1 : WORKERS, cc_worker, &s);
	}
	CHECK(sift_test_trap_count() == traps || getenv("FILTER_DEV"));
	printf("test_filter: %d chunks of the codec x %d tests, %d read by the kernels\n", s.n, CC_TESTS, handled);
}

/* ---- the string kernel on an opened chunk, with the strings it keeps ---- */

#define STR_CASES	160

typedef struct StrCase
{
	const Job  *job;
	SiftChunk	chunk;
	bool		handled;
	uint64		want[FX_ROWS / 64 + 8];
} StrCase;

typedef struct StrSuite
{
	StrCase	   *cases;
	int			n;
	SiftArena  *arena[WORKERS];
} StrSuite;

static bool
str_kernel_job(const GranuleFixture *fx, const Job *j)
{
	SiftColOp	op = j->pred.op;
	uint16		enc = chunk_enc(fx, j);

	return fx->cols[j->col].kind == SIFT_CELL_STR && (op == SIFT_COLOP_CONTAINS || op == SIFT_COLOP_NOTCONTAINS || op == SIFT_COLOP_NE) &&
		(enc == SIFT_CENC_STR_BLOCK || enc == SIFT_CENC_STR_FSST || enc == SIFT_CENC_STR_INLINE || enc == SIFT_CENC_STR_VIEW);
}

static void
str_worker(void *arg, int w)
{
	StrSuite   *ss = arg;

	for (int i = 0; i < ss->n; i++)
	{
		const StrCase *sc = &ss->cases[(i + w * (ss->n / WORKERS)) % ss->n];
		SiftStrMemo memo = {0};
		SiftErr		err;
		uint64		dense[FX_ROWS / 64 + 8];
		bool		handled;
		uint16		enc = sc->chunk.enc;

		sift_arena_reset(ss->arena[w]);
		CHECK(sift_cfstr_bits_live_a(ss->arena[w], &err, &memo, &sc->job->cp, &sc->chunk, NULL, 0, dense, &handled));
		CHECK(handled == sc->handled && (!handled || memcmp(dense, sc->want, (sc->chunk.nnonnull + 63) / 64 * 8) == 0));
		CHECK(memo.n == (handled && (enc == SIFT_CENC_STR_BLOCK || enc == SIFT_CENC_STR_FSST)));
	}
	sift_colz_thread_fini();
}

static void
test_str_kernel(const Suite *s)
{
	StrSuite	ss = {.cases = palloc0(STR_CASES * sizeof(StrCase))};
	uint64		traps = sift_test_trap_count();
	int			kept = 0;

	for (int i = 0; i < s->njobs && ss.n < STR_CASES; i++)
	{
		const Job  *j = s->jobs[i];
		StrCase    *sc = &ss.cases[ss.n];

		if (!str_kernel_job(s->fx, j) || j->want == SIFT_CFILT_UNSUPPORTED)
			continue;
		sift_chunk_open(&s->fx->g[j->g].view, j->col + 1, &sc->chunk);
		sc->job = j;
		sc->handled = sift_cfstr_bits_live(&j->cp, &sc->chunk, NULL, 0, sc->want);
		kept += sc->handled && (sc->chunk.enc == SIFT_CENC_STR_BLOCK || sc->chunk.enc == SIFT_CENC_STR_FSST);
		ss.n++;
	}
	CHECK(ss.n > 0 && kept > 0);
	for (int w = 0; w < WORKERS; w++)
		ss.arena[w] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	unit_run_workers(getenv("FILTER_DEV") ? 1 : WORKERS, str_worker, &ss);
	CHECK(sift_test_trap_count() == traps || getenv("FILTER_DEV"));
	printf("test_filter: %d string chunks on the kernel, %d keep their strings\n", ss.n, kept);
}

static void
test_levels(Suite *s)
{
	SiftSimdLevel lv[3];
	int			nlv = levels(lv);
	uint64		traps = sift_test_trap_count();

	for (int i = 0; i < nlv; i++)
	{
		level_use(lv[i]);
		unit_run_workers(getenv("FILTER_DEV") ? 1 : WORKERS, filter_worker, s);
		printf("test_filter: level %d: %d filters x %d workers\n", (int) lv[i], s->njobs, WORKERS);
	}
	CHECK(sift_test_trap_count() == traps || getenv("FILTER_DEV"));
}

int
main(void)
{
	Suite		s = {0};
	int			bits = 0,
				kept = 0;

	granule_fixture_init();
	sift_test_trap_soft = getenv("FILTER_DEV") != NULL;
	s.fx = granule_fixture_build(GRANULES, 7);
	for (int w = 0; w < WORKERS; w++)
		s.arena[w] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	jobs_build(&s);
	for (int i = 0; i < s.njobs; i++)
	{
		bits += s.jobs[i]->want == SIFT_CFILT_BITS;
		kept += s.jobs[i]->memo_n;
	}
	CHECK(bits > 0 && kept > 0);
	CHECK((encs_filtered & ENCS_REQUIRED) == ENCS_REQUIRED);
	printf("test_filter: %d filters (%d masks, %d keep strings)\n", s.njobs, bits, kept);
	test_levels(&s);
	test_cc_kernels();
	test_str_kernel(&s);
	if (!getenv("FILTER_DEV"))
		test_damage(&s);
	test_arena_full(&s);
	sift_colz_thread_fini();
	granule_fixture_free((GranuleFixture *) s.fx);
	puts("test_filter: ok");
	return 0;
}
