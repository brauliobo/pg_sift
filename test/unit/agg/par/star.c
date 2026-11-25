/*
 * agg/par/star.c - agg/par/star.c on 1 to 4 threads (the trap shim flags the helpers: no palloc, no ereport): a star join of a fact table with dimensions that the leader read once, against the fact rows
 * joined one at a time by a linear look at the dimension. Inner and left joins, integer and text payload keys (short ones sit in their views, long ones in the table), NULL payloads, orphans that find no
 * dimension row, a dimension that holds a key more than once, a predicate that leaves a quarter of the rows; and what the threads must leave to the scan: a NULL in a summed column, a batch that expands past
 * the granule.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"

#include "agg/par/star.h"
#include "core/hash.h"
#include "core/par/pool.h"
#include "core/popcount.h"
#include "core/testrng.h"
#include "host.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define ROWS		(1u << 17)
#define GRAN		8192
#define SCRATCH		(1 << 20)
#define NDIM_ROWS	500

static int	nchecks;

/* the pipe of a compute thread never reaches the groups of the backend */
void
sift_v6_group_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	SIFT_CHECK(false);
}

#define PASS(cond) do { nchecks++; SIFT_CHECK(cond); } while (0)

typedef struct Fact
{
	int64	   *k[3];			/* the join key, a second integer, the measure */
	bool	   *isnull[3];
	uint64	   *alive;
} Fact;

typedef struct Dim
{
	int			n;
	int64		key[NDIM_ROWS * 4];
	bool		g_null[NDIM_ROWS * 4];
	int64		g[NDIM_ROWS * 4];
	bool		name_null[NDIM_ROWS * 4];
	char		name[NDIM_ROWS * 4][24];
	SiftStarDim table;
} Dim;

static uint64 rng_state = 11;

static uint64
rng(void)
{
	return sift_splitmix_next(&rng_state);
}

static Fact *
make_fact(bool null_measure)
{
	Fact	   *f = calloc(1, sizeof(Fact));

	for (int c = 0; c < 3; c++)
	{
		f->k[c] = malloc(sizeof(int64) * ROWS);
		f->isnull[c] = calloc(ROWS, 1);
	}
	f->alive = calloc(ROWS / 64, sizeof(uint64));
	for (uint32 i = 0; i < ROWS; i++)
	{
		const uint64 h = rng();

		f->k[0][i] = 1 + (int64) (h % (NDIM_ROWS + NDIM_ROWS / 5));	/* the last fifth of the keys has no dimension row */
		f->k[1][i] = (int64) ((h >> 20) % 9) - 4;
		f->k[2][i] = (int64) ((h >> 32) % 2001) - 1000;
		f->isnull[2][i] = null_measure && (h >> 50) % 4001 == 3;
		f->isnull[1][i] = (h >> 44) % 31 == 5;
		if ((h >> 3) % 4 == 0)
			f->alive[i / 64] |= UINT64CONST(1) << (i % 64);
	}
	return f;
}

static void
free_fact(Fact *f)
{
	for (int c = 0; c < 3; c++)
	{
		free(f->k[c]);
		free(f->isnull[c]);
	}
	free(f->alive);
	free(f);
}

/* A dimension of NDIM_ROWS keys, each `copies` times (copies > 1: a key more than once), with payload g (int, NULL for some) and name (text, short and long, NULL for some) */
static Dim *
make_dim(int copies, SiftV6JoinMode mode)
{
	Dim		   *d = calloc(1, sizeof(Dim));
	SiftStarShape sh = {.mode = mode,.nkeys = 1,.npay = 2,.budget = UINT64_MAX};

	sh.kcls[0] = SSC_INT;
	sh.pcls[0] = SSC_INT;
	sh.pcls[1] = SSC_STR;
	sift_star_dim_init(&d->table, &sh);
	for (int c = 0; c < copies; c++)
		for (int k = 1; k <= NDIM_ROWS; k++)
		{
			const int	i = d->n++;
			SiftStarRow row = {0};
			const uint64 h = sift_fmix64(k * 31 + c);

			d->key[i] = k;
			d->g_null[i] = k % 17 == 0;
			d->g[i] = (int64) (h % 40) - 5;
			d->name_null[i] = k % 23 == 0;
			snprintf(d->name[i], sizeof(d->name[i]), k % 3 == 0 ? "a-long-name-of-%d" : "n%d", (int) (h % 1000));
			row.key_i[0] = k;
			row.pay_i[0] = d->g[i];
			row.pay_null[0] = d->g_null[i];
			row.pay_p[1] = d->name[i];
			row.pay_len[1] = strlen(d->name[i]);
			row.pay_null[1] = d->name_null[i];
			SIFT_CHECK(sift_star_dim_add(&d->table, &row));
		}
	sift_star_dim_finish(&d->table);
	return d;
}

/* ---- the statement ---- */

typedef struct Stmt
{
	SiftV6Spec	spec;
	SiftV6Ext	ext;
	SiftV6Pipe *pipe;
	SiftParWordShape sh;
	bool		text_key;
} Stmt;

static const SiftV6Col fact_col[3] = {{.attno = 1,.typid = INT8OID}, {.attno = 2,.typid = INT4OID}, {.attno = 3,.typid = INT4OID}};

/* keys: the payload g of the dimension and the second fact column (or the text payload alone); aggregates: count(*), sum(measure), count(measure) */
static Stmt *
make_stmt(Dim *d, SiftV6JoinMode mode, bool text_key, bool with_count)
{
	Stmt	   *st = calloc(1, sizeof(Stmt));
	SiftV6Spec *sp = &st->spec;
	SiftVecType ftype[3] = {SVEC_I64, SVEC_I64, SVEC_I64};

	st->text_key = text_key;
	sp->ext = &st->ext;
	st->ext.ndims = 1;
	st->ext.dim[0] = (SiftV6XDim) {.mode = mode,.nkeys = 1,.fatt = {1},.ftype = {INT8OID},.ktype = {INT8OID},.npay = 2,.ptype = {INT4OID, TEXTOID}};
	sp->key = calloc(2, sizeof(SiftV6Col));
	sp->nkeys = text_key ? 1 : 2;
	sp->key[0] = text_key ? (SiftV6Col) {.src = SV6SRC_DIM,.dim = 0,.slot = 1,.typid = TEXTOID} : (SiftV6Col) {.src = SV6SRC_DIM,.dim = 0,.slot = 0,.typid = INT4OID};
	sp->key[1] = fact_col[1];
	sp->agg = calloc(3, sizeof(SiftV6Agg));
	sp->naggs = with_count ? 3 : 2;
	sp->agg[0] = (SiftV6Agg) {.kind = SV6_COUNT_STAR};
	sp->agg[1] = (SiftV6Agg) {.kind = SV6_SUM,.arg = fact_col[2]};
	if (with_count)
		sp->agg[2] = (SiftV6Agg) {.kind = SV6_COUNT,.arg = fact_col[2]};
	SIFT_CHECK(sift_par_star_shape(sp, &st->sh));
	st->pipe = sift_v6_pipe_new(CurrentMemoryContext, sp, NULL, NULL, 3, ftype);
	st->pipe->dim[0] = &d->table;
	st->pipe->nparts[0] = 1;
	st->pipe->fslot[0][0] = 0;
	st->pipe->key_slot[0] = sift_v6_pipe_dim_slot(st->pipe, 0, text_key ? 1 : 0);
	if (!text_key)
		st->pipe->key_slot[1] = 1;
	for (int v = 0; v < sift_v6_spec_nvec(sp); v++)
		st->pipe->arg_slot[v] = sift_v6_vec_col(sp, v) == NULL ? -1 : 2;
	return st;
}

static void
free_stmt(Stmt *st)
{
	free(st->spec.key);
	free(st->spec.agg);
	free(st);
}

/* ---- the reference: the rows joined by a look at the dimension ---- */

typedef struct Rec
{
	uint64		key[2];
	uint8		str[24];
	uint32		len;
	uint64		count;
	uint64		sum;
	uint64		nn;
} Rec;

typedef struct Vec
{
	Rec		   *r;
	uint32		n;
	uint32		cap;
} Vec;

static void
vec_add(Vec *v, const Rec *r)
{
	if (v->n == v->cap)
		v->r = realloc(v->r, sizeof(Rec) * (v->cap = v->cap ? v->cap * 2 : 1024));
	v->r[v->n++] = *r;
}

static int
cmp_rec(const void *a, const void *b)
{
	const Rec  *x = a,
			   *y = b;
	int			c;

	for (int j = 0; j < 2; j++)
		if (x->key[j] != y->key[j])
			return x->key[j] < y->key[j] ? -1 : 1;
	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	c = x->len == SIFT_STR_NULL ? 0 : memcmp(x->str, y->str, x->len);
	return c;
}

static Vec
merged(Vec *in, int n)
{
	Vec			all = {0},
				m = {0};

	for (int t = 0; t < n; t++)
		for (uint32 i = 0; i < in[t].n; i++)
			vec_add(&all, &in[t].r[i]);
	qsort(all.r, all.n, sizeof(Rec), cmp_rec);
	for (uint32 i = 0; i < all.n; i++)
	{
		if (m.n > 0 && cmp_rec(&m.r[m.n - 1], &all.r[i]) == 0)
		{
			m.r[m.n - 1].count += all.r[i].count;
			m.r[m.n - 1].sum += all.r[i].sum;
			m.r[m.n - 1].nn += all.r[i].nn;
		}
		else
			vec_add(&m, &all.r[i]);
	}
	free(all.r);
	return m;
}

#define NULLW	UINT64_MAX
#define SIGN	(UINT64CONST(1) << 63)

static Vec
reference(const Fact *f, const Dim *d, const Stmt *st, SiftV6JoinMode mode)
{
	Vec			v = {0},
				out;

	for (uint32 i = 0; i < ROWS; i++)
	{
		int			match[8];
		int			nmatch = 0;

		if (!((f->alive[i / 64] >> (i % 64)) & 1))
			continue;
		for (int j = 0; j < d->n && !f->isnull[0][i]; j++)
			if (d->key[j] == f->k[0][i])
				match[nmatch++] = j;
		if (nmatch == 0 && mode != SV6J_LEFT_EQUI)
			continue;
		for (int q = 0; q < Max(nmatch, 1); q++)
		{
			const int	j = q < nmatch ? match[q] : -1;
			Rec			r = {.count = 1,.sum = f->isnull[2][i] ? 0 : (uint64) f->k[2][i],.nn = !f->isnull[2][i]};

			if (st->text_key)
			{
				r.len = j >= 0 && !d->name_null[j] ? strlen(d->name[j]) : SIFT_STR_NULL;
				if (r.len != SIFT_STR_NULL)
					memcpy(r.str, d->name[j], r.len);
			}
			else
			{
				r.key[0] = j >= 0 && !d->g_null[j] ? (uint64) d->g[j] ^ SIGN : NULLW;
				r.key[1] = f->isnull[1][i] ? NULLW : (uint64) f->k[1][i] ^ SIGN;
			}
			vec_add(&v, &r);
		}
	}
	out = merged(&v, 1);
	free(v.r);
	return out;
}

/* ---- the run ---- */

typedef struct Ctx
{
	SiftParStar *st;
	const Fact *f;
	Vec			out[SIFT_PAR_MAX_THREADS];
	const SiftParWordShape *sh;
} Ctx;

static bool
fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	Ctx		   *c = arg;
	const uint32 at = lo * GRAN;
	SiftParCol	col[3];
	uint32		nact = 0;

	for (int k = 0; k < 3; k++)
		col[k] = (SiftParCol) {c->f->k[k] + at, c->f->isnull[k] + at};
	for (int w = 0; w < GRAN / 64; w++)
		nact += sift_pc64(c->f->alive[at / 64 + w]);
	if (nact == 0)
		return true;
	return sift_par_star_batch(c->st, worker, col, c->f->alive + at / 64, GRAN, err, out);
}

static void
emit(void *arg, int worker, const SiftParGroupRow *row)
{
	Ctx		   *c = arg;
	Rec			r = {.count = row->count,.sum = row->sum[c->sh->v_slot[1]],.nn = row->sum[c->sh->n_slot[1]]};

	if (c->sh->gs.strs)
	{
		r.len = row->str[0].len;
		if (r.len != SIFT_STR_NULL)
			memcpy(r.str, row->str[0].p, r.len);
	}
	else
		memcpy(r.key, row->key, sizeof(uint64) * 2);
	vec_add(&c->out[worker], &r);
}

static void
overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	SIFT_CHECK(false);
}

static bool
feed(void *arg, uint32 item)
{
	return item < ROWS / GRAN;
}

static void
release(void *arg, uint32 item)
{
}

static void
fallback(void *arg, uint32 item)
{
	SIFT_CHECK(false);
}

static Vec
run(const Fact *f, const Stmt *st, const Dim *d, uint32 workers)
{
	Ctx			c = {.f = f,.sh = &st->sh};
	SiftParGroupHooks hooks = {.fill = fill,.emit = emit,.overflow = overflow,.arg = &c};
	SiftParArenas *ar;
	SiftParGroup *g;
	SiftErr		err = {0};
	SiftParOps	ops = sift_par_group_ops;
	Vec			v;

	c.st = sift_par_star_new(CurrentMemoryContext, &st->sh, st->pipe, workers);
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, SCRATCH, 8 << 20, 8 << 20);
	g = sift_par_group_create(ar, &st->sh.gs, false, ROWS, &hooks, &err);
	SIFT_CHECK(g != NULL);
	ops.feed_slot = feed;
	ops.release_slot = release;
	ops.fallback_item = fallback;
	SIFT_CHECK(sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = workers}, &ops, g, ar, NULL));
	PASS(!sift_par_pool_busy());
	v = merged(c.out, SIFT_PAR_MAX_THREADS);
	for (int t = 0; t < SIFT_PAR_MAX_THREADS; t++)
		free(c.out[t].r);
	return v;
}

static bool
same(const Vec *a, const Vec *b)
{
	if (a->n != b->n)
		return false;
	for (uint32 i = 0; i < a->n; i++)
		if (cmp_rec(&a->r[i], &b->r[i]) != 0 || a->r[i].count != b->r[i].count || a->r[i].sum != b->r[i].sum || a->r[i].nn != b->r[i].nn)
			return false;
	return true;
}

static void
check_parity(SiftV6JoinMode mode, int copies, bool text_key, bool with_count, bool null_measure, const char *what)
{
	Fact	   *f = make_fact(null_measure);
	Dim		   *d = make_dim(copies, mode);
	Stmt	   *st = make_stmt(d, mode, text_key, with_count);
	Vec			want = reference(f, d, st, mode);

	PASS(want.n > 0);
	for (uint32 w = 1; w <= 4; w++)
	{
		Vec			got = run(f, st, d, w);

		if (!same(&got, &want))
			fprintf(stderr, "%s: %u threads: %u groups, want %u\n", what, w, got.n, want.n);
		PASS(same(&got, &want));
		free(got.r);
	}
	free(want.r);
	free_fact(f);
	free(d);
	free_stmt(st);
	pgshim_reset();
}

/* What the threads leave to the scan: one batch on one thread */
static void
check_scan(bool null_measure, int copies, int expect_code)
{
	Fact	   *f = make_fact(null_measure);
	Dim		   *d = make_dim(copies, SV6J_INNER_EQUI);
	Stmt	   *st = make_stmt(d, SV6J_INNER_EQUI, false, true);
	SiftParStar *star = sift_par_star_new(CurrentMemoryContext, &st->sh, st->pipe, 1);
	SiftParCol	col[3];
	SiftParGroupBatch out = {0};
	SiftErr		err = {0};
	uint64		active[GRAN / 64];
	bool		ok;

	for (int k = 0; k < 3; k++)
		col[k] = (SiftParCol) {f->k[k], f->isnull[k]};
	memset(active, 0xFF, sizeof(active));
	if (null_measure)
		f->isnull[2][5] = true;
	ok = sift_par_star_batch(star, 0, col, active, GRAN, &err, &out);
	PASS(ok == (expect_code == 0));
	PASS(ok || err.code == expect_code);
	free_fact(f);
	free(d);
	free_stmt(st);
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	check_parity(SV6J_INNER_EQUI, 1, false, false, false, "inner, int keys");
	check_parity(SV6J_INNER_EQUI, 1, false, true, false, "inner, int keys, count(col)");
	check_parity(SV6J_LEFT_EQUI, 1, false, true, false, "left, int keys");
	check_parity(SV6J_INNER_EQUI, 1, true, true, false, "inner, text key");
	check_parity(SV6J_LEFT_EQUI, 1, true, false, false, "left, text key");
	check_parity(SV6J_INNER_EQUI, 2, false, true, false, "inner, a key twice in the dimension");
	check_parity(SV6J_LEFT_EQUI, 3, true, true, false, "left, a key three times, text key");
	check_parity(SV6J_INNER_EQUI, 1, false, true, true, "inner, NULL measures counted and summed");
	check_parity(SV6J_LEFT_EQUI, 1, true, true, true, "left, text key, NULL measures counted and summed");
	check_scan(false, 1, 0);
	check_scan(true, 1, 0);
	check_scan(false, 4, SIFT_ERR_ARENA_FULL);
	printf("par_star: ok (%d checks)\n", nchecks);
	return 0;
}
