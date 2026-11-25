/*
 * tests/bench.c - the real device against the CPU on the same encoded chunks: ungrouped, dense, high-cardinality hash (two keys, about a
 * million groups), expression, and unsorted top N, at 10M and 50M rows; the device cold (the chunks cross the PCIe link and are decoded) and warm
 * (resident in VRAM). Every device result is compared with the CPU's. Two CPU baselines, all cores (worker processes: the codec's allocator is not
 * thread-safe):
 *   cpu-enc: each worker decodes its granules with the codec's decoder and aggregates (a scan of the stored column chunks)
 *   cpu-hot: the columns already decoded in RAM at their natural width; only the aggregation is timed (the best a CPU scan can do)
 *   vk_v6_bench [--rows 10,50] [--device SEL] [--reps N] [--cache-mb N] [--workers N]
 */
#include "postgres.h"

#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "col/codec/codec.h"
#include "ccshim.h"
#include "common.h"

#include "bench.h"

typedef struct Res
{
	int64_t		cnt, sum, mn, mx;
	double		fsum;
	uint64_t	groups, chk;
	int64_t		dcnt[100], dsum[100];
	double		dfsum[100];
	int64_t		top[100];
	uint32_t	ntop;
} Res;

static int	nworkers = 32,
			reps = 3;
static uint64_t cache_mb = 6144;

static double
now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void *
shm(size_t bytes)
{
	void	   *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
	{
		perror("mmap");
		exit(2);
	}
	return p;
}

/* The worker processes are forked once, before any data exists (a fork of a process that holds the data costs more than the scan), and take
 * their commands from a pipe; everything they read or write lives in shared memory mapped before the fork. */
typedef struct Disp
{
	void		(*fn) (int w, void *ctx);
	void	   *ctx;
} Disp;

static Disp *disp;
static int	cmd_fd[64],
			ack_fd[64];
static uint8_t *arena;
static size_t arena_used;

static void *
arena_alloc(size_t n)
{
	void	   *p = arena + arena_used;

	arena_used += (n + 63) & ~(size_t) 63;
	return p;
}

static void
pool_start(void)
{
	disp = shm(sizeof(*disp));
	for (int w = 0; w < nworkers; w++)
	{
		int			cmd[2],
					ack[2];

		if (pipe(cmd) || pipe(ack))
			exit(2);
		if (fork() == 0)
		{
			char		ch;

			while (read(cmd[0], &ch, 1) == 1 && ch)
			{
				disp->fn(w, disp->ctx);
				if (write(ack[1], "k", 1) != 1)
					_exit(2);
			}
			_exit(0);
		}
		cmd_fd[w] = cmd[1];
		ack_fd[w] = ack[0];
	}
}

static void
parallel(void (*fn) (int w, void *ctx), void *ctx)
{
	char		ch;

	disp->fn = fn;
	disp->ctx = ctx;
	for (int w = 0; w < nworkers; w++)
		if (write(cmd_fd[w], "g", 1) != 1)
			exit(2);
	for (int w = 0; w < nworkers; w++)
		if (read(ack_fd[w], &ch, 1) != 1)
		{
			fprintf(stderr, "a worker failed\n");
			exit(2);
		}
}

static void
pool_stop(void)
{
	for (int w = 0; w < nworkers; w++)
		if (write(cmd_fd[w], "\0", 1) != 1)
			exit(2);
	for (int w = 0; w < nworkers; w++)
		wait(NULL);
}

/* ---- the data ---- */
static void
build_bench(Bench *b, uint32_t ngran)
{
	TData		d;
	Rng			rng = {0xBE7C4};

	memset(b, 0, sizeof(*b));
	b->ngran = ngran;
	b->rows = (uint64_t) ngran * SIFT_VK_GRANULE_ROWS;
	b->vg = calloc(ngran, sizeof(SiftVkGranule));
	for (int c = 0; c < B_NCOLS; c++)
		b->chunk[c] = arena_alloc(ngran * sizeof(SiftVkChunk));
	td_init(&d, 1, B_NCOLS, SIFT_VK_GRANULE_ROWS);
	for (uint32_t gi = 0; gi < ngran; gi++)
	{
		TGran	   *g = &d.g[0];

		g->id = gi;
		for (uint32_t r = 0; r < SIFT_VK_GRANULE_ROWS; r++)
		{
			int64_t		vals[B_NCOLS] = {rng_range(&rng, 0, 99), rng_range(&rng, 0, 9999), rng_range(&rng, 0, 99), rng_range(&rng, 1, 50),
				f8_key((double) rng_range(&rng, 1, 99999) / 100.0), rng_range(&rng, 0, 5000000)};

			for (int c = 0; c < B_NCOLS; c++)
			{
				g->col[c].v[r] = vals[c];
				g->col[c].ok[r] = 1;
			}
		}
		for (int c = 0; c < B_NCOLS; c++)
		{
			ChunkSpec	sp = {.policy = c == B_PRICE ? "alp" : "for", .enc = c == B_PRICE ? SIFT_VK_ENC_ALP : SIFT_VK_ENC_INT_FOR, .nulls = NULLS_AUTO};

			if (!build_chunk(g, c, &sp, gi))
			{
				fprintf(stderr, "chunk: %s\n", sp.why);
				exit(2);
			}
			b->chunk[c][gi] = g->chunk[c];
			b->chunk[c][gi].key.part_gen = 77;
			b->chunk[c][gi].bytes = memcpy(arena_alloc(g->chunk[c].len + 8), g->buf[c], g->chunk[c].len);
			free(g->buf[c]);
			g->buf[c] = NULL;
			b->enc_bytes[c] += g->chunk[c].len;
		}
		b->vg[gi] = (SiftVkGranule) {.id = gi, .nrows = SIFT_VK_GRANULE_ROWS};
		for (int c = 0; c < B_NCOLS; c++)
			b->vg[gi].col[c] = &b->chunk[c][gi];
	}
}

static void
free_bench(Bench *b)
{
	free(b->vg);
	arena_used = 0;
}

static void
dec_chunk(const SiftVkChunk *ch, int64_t *vals)
{
	const uint8_t *body = (const uint8_t *) ch->bytes;
	uint32_t	len;
	SiftCc	   *cc;

	memcpy(&len, body, 4);
	cc = sift_cc_open_rows(CurrentMemoryContext, body + 8, len, true, ch->nrows);
	sift_cc_decode(cc, vals);
	sift_cc_close(cc);
}

/* ---- the CPU shapes: a worker's share of the granules ---- */
typedef struct Hot
{
	uint8_t    *ks, *kl, *qty;
	uint16_t   *km;
	uint32_t   *amount;
	double	   *price;
} Hot;

typedef struct HEnt
{
	uint32_t	key;
	int64_t		cnt, sum, mx;
} HEnt;

typedef struct Ctx
{
	const Bench *b;
	const Hot  *hot;
	int			shape;
	bool		decode;			/* from the chunks (else the hot arrays) */
	Res		   *part;			/* per worker */
	HEnt	   *ht;				/* per worker, 1 << 21 slots */
	HEnt	   *ents;			/* per worker, the groups it found, by merge bucket */
	uint32_t   *bucket_off;		/* per worker, nworkers + 1 */
	Res		   *merged;
	int			wbits,			/* the table of a worker, sized for its rows */
				mbits;			/* the table that merges one bucket of the groups */
} Ctx;

#define HT_BITS 21				/* the most slots of a worker's table */

static inline uint32_t
hash32(uint32_t k, int bits)
{
	return (k * 2654435761u) >> (32 - bits);
}

static int
log2_ceil(uint64_t v)
{
	int			b = 0;

	while (((uint64_t) 1 << b) < v)
		b++;
	return b;
}

static void
agg_s1(Res *r, const int64_t *qty, const int64_t *amount, const double *price, uint32_t n)
{
	int64_t		cnt = 0, sum = 0, mn = r->mn, mx = r->mx;
	double		fs = 0;

	for (uint32_t i = 0; i < n; i++)
		if (qty[i] > 10)
		{
			cnt++;
			sum += amount[i];
			mn = qty[i] < mn ? qty[i] : mn;
			mx = qty[i] > mx ? qty[i] : mx;
			fs += price[i];
		}
	r->cnt += cnt;
	r->sum += sum;
	r->mn = mn;
	r->mx = mx;
	r->fsum += fs;
}

static void
topn_push(int64_t *top, uint32_t *ntop, int64_t v)
{
	uint32_t	i;

	if (*ntop == 100 && v <= top[99])
		return;
	i = *ntop < 100 ? (*ntop)++ : 99;
	while (i > 0 && top[i - 1] < v)
	{
		top[i] = top[i - 1];
		i--;
	}
	top[i] = v;
}

static void
worker(int w, void *vctx)
{
	Ctx		   *c = vctx;
	const Bench *b = c->b;
	uint32_t	g0 = (uint32_t) ((uint64_t) b->ngran * w / nworkers),
				g1 = (uint32_t) ((uint64_t) b->ngran * (w + 1) / nworkers);
	Res		   *r = &c->part[w];
	int64_t    *col[B_NCOLS];
	double	   *pr = malloc(SIFT_VK_GRANULE_ROWS * 8);
	HEnt	   *ht = c->ht + ((size_t) w << HT_BITS);

	if (c->shape == 2)
		memset(ht, 0, sizeof(HEnt) << c->wbits);

	for (int i = 0; i < B_NCOLS; i++)
		col[i] = aligned_alloc(64, SIFT_VK_GRANULE_ROWS * 8);
	memset(r, 0, sizeof(*r));
	r->mn = INT64_MAX;
	r->mx = INT64_MIN;
	for (uint32_t g = g0; g < g1; g++)
	{
		uint64_t	base = (uint64_t) g * SIFT_VK_GRANULE_ROWS;
		uint32_t	n = SIFT_VK_GRANULE_ROWS;

		if (c->decode)
		{
			static const int need[5][4] = {{B_QTY, B_AMOUNT, B_PRICE, -1}, {B_KS, B_AMOUNT, B_PRICE, -1}, {B_KM, B_KL, B_QTY, B_AMOUNT},
				{B_QTY, B_AMOUNT, B_PRICE, -1}, {B_AMOUNT, -1, -1, -1}};

			for (int i = 0; i < 4 && need[c->shape][i] >= 0; i++)
				dec_chunk(&b->chunk[need[c->shape][i]][g], col[need[c->shape][i]]);
			if (c->shape != 4 && c->shape != 2)
				for (uint32_t i = 0; i < n; i++)
					pr[i] = f8_from_key(col[B_PRICE][i]);
		}
		else
		{
			const Hot  *h = c->hot;

			if (c->shape == 1)
				for (uint32_t i = 0; i < n; i++)
					col[B_KS][i] = h->ks[base + i];
			if (c->shape == 2)
				for (uint32_t i = 0; i < n; i++)
				{
					col[B_KM][i] = h->km[base + i];
					col[B_KL][i] = h->kl[base + i];
				}
			if (c->shape != 1 && c->shape != 4)
				for (uint32_t i = 0; i < n; i++)
					col[B_QTY][i] = h->qty[base + i];
			for (uint32_t i = 0; i < n; i++)
				col[B_AMOUNT][i] = h->amount[base + i];
			if (c->shape != 2 && c->shape != 4)
				memcpy(pr, h->price + base, n * 8);
		}
		if (c->shape == 0)
			agg_s1(r, col[B_QTY], col[B_AMOUNT], pr, n);
		else if (c->shape == 1)
			for (uint32_t i = 0; i < n; i++)
			{
				uint32_t	k = (uint32_t) col[B_KS][i];

				r->dcnt[k]++;
				r->dsum[k] += col[B_AMOUNT][i];
				r->dfsum[k] += pr[i];
			}
		else if (c->shape == 2)
			for (uint32_t i = 0; i < n; i++)
			{
				uint32_t	key = (uint32_t) (col[B_KM][i] * 100 + col[B_KL][i]),
							s = hash32(key, c->wbits);

				while (ht[s].cnt && ht[s].key != key)
					s = (s + 1) & ((1u << c->wbits) - 1);
				if (!ht[s].cnt)
					ht[s] = (HEnt) {key, 0, 0, INT64_MIN};
				ht[s].cnt++;
				ht[s].sum += col[B_AMOUNT][i];
				ht[s].mx = col[B_QTY][i] > ht[s].mx ? col[B_QTY][i] : ht[s].mx;
			}
		else if (c->shape == 3)
			for (uint32_t i = 0; i < n; i++)
			{
				int64_t		q = col[B_QTY][i],
							a = col[B_AMOUNT][i],
							v = a * q - a / (q + 1);

				r->cnt++;
				r->sum += v;
				r->mx = v > r->mx ? v : r->mx;
				r->fsum += pr[i] * (double) q * 1.0725;
			}
		else
			for (uint32_t i = 0; i < n; i++)
				topn_push(r->top, &r->ntop, col[B_AMOUNT][i]);
	}
	if (c->shape == 2)
	{
		uint32_t	cnt[64] = {0},
					off = 0;
		HEnt	   *out = c->ents + (size_t) w * (1 << 20);
		uint32_t   *bo = c->bucket_off + (size_t) w * (nworkers + 1);

		for (uint32_t s = 0; s < (1u << c->wbits); s++)
			if (ht[s].cnt)
				cnt[(ht[s].key * 2246822519u >> 8) % nworkers]++;
		for (int p = 0; p < nworkers; p++)
		{
			bo[p] = off;
			off += cnt[p];
			cnt[p] = bo[p];
		}
		bo[nworkers] = off;
		for (uint32_t s = 0; s < (1u << c->wbits); s++)
			if (ht[s].cnt)
				out[cnt[(ht[s].key * 2246822519u >> 8) % nworkers]++] = ht[s];
	}
	for (int i = 0; i < B_NCOLS; i++)
		free(col[i]);
	free(pr);
}

static uint64_t
mixk(uint64_t k)
{
	k *= 0x9E3779B97F4A7C15ull;
	return k ^ (k >> 29);
}

static void
merge_worker(int p, void *vctx)
{
	Ctx		   *c = vctx;
	uint32_t	slots = 1u << c->mbits;
	HEnt	   *t = calloc(slots, sizeof(HEnt));
	Res		   *m = &c->merged[p];

	memset(m, 0, sizeof(*m));
	for (int w = 0; w < nworkers; w++)
	{
		const HEnt *e = c->ents + (size_t) w * (1 << 20);
		const uint32_t *bo = c->bucket_off + (size_t) w * (nworkers + 1);

		for (uint32_t i = bo[p]; i < bo[p + 1]; i++)
		{
			uint32_t	s = hash32(e[i].key, c->mbits);

			while (t[s].cnt && t[s].key != e[i].key)
				s = (s + 1) & (slots - 1);
			if (!t[s].cnt)
			{
				t[s] = (HEnt) {e[i].key, 0, 0, INT64_MIN};
				m->groups++;
			}
			t[s].cnt += e[i].cnt;
			t[s].sum += e[i].sum;
			t[s].mx = e[i].mx > t[s].mx ? e[i].mx : t[s].mx;
		}
	}
	for (uint32_t s = 0; s < slots; s++)
		if (t[s].cnt)
			m->chk += mixk(t[s].key) * (uint64_t) (t[s].cnt + 31 * t[s].sum + 131 * t[s].mx);
	free(t);
}

/* one run of a shape on the CPU; the merged answer in *out */
static double
cpu_run(Ctx *c, int shape, Res *out)
{
	double		t0 = now_s();

	c->shape = shape;
	parallel(worker, c);
	memset(out, 0, sizeof(*out));
	out->mn = INT64_MAX;
	out->mx = INT64_MIN;
	if (shape == 2)
	{
		parallel(merge_worker, c);
		for (int p = 0; p < nworkers; p++)
		{
			out->groups += c->merged[p].groups;
			out->chk += c->merged[p].chk;
		}
	}
	for (int w = 0; w < nworkers; w++)
	{
		const Res  *r = &c->part[w];

		out->cnt += r->cnt;
		out->sum += r->sum;
		out->fsum += r->fsum;
		out->mn = r->mn < out->mn ? r->mn : out->mn;
		out->mx = r->mx > out->mx ? r->mx : out->mx;
		for (int k = 0; k < 100; k++)
		{
			out->dcnt[k] += r->dcnt[k];
			out->dsum[k] += r->dsum[k];
			out->dfsum[k] += r->dfsum[k];
		}
		for (uint32_t i = 0; i < r->ntop; i++)
			topn_push(out->top, &out->ntop, r->top[i]);
	}
	return (now_s() - t0) * 1000.0;
}

static void
hot_decode_worker(int w, void *vctx)
{
	Ctx		   *c = vctx;
	const Bench *b = c->b;
	const Hot  *h = c->hot;
	uint32_t	g0 = (uint32_t) ((uint64_t) b->ngran * w / nworkers),
				g1 = (uint32_t) ((uint64_t) b->ngran * (w + 1) / nworkers);
	int64_t    *v = aligned_alloc(64, SIFT_VK_GRANULE_ROWS * 8);

	for (uint32_t g = g0; g < g1; g++)
	{
		uint64_t	base = (uint64_t) g * SIFT_VK_GRANULE_ROWS;

		dec_chunk(&b->chunk[B_KS][g], v);
		for (uint32_t i = 0; i < SIFT_VK_GRANULE_ROWS; i++)
			h->ks[base + i] = (uint8_t) v[i];
		dec_chunk(&b->chunk[B_KM][g], v);
		for (uint32_t i = 0; i < SIFT_VK_GRANULE_ROWS; i++)
			h->km[base + i] = (uint16_t) v[i];
		dec_chunk(&b->chunk[B_KL][g], v);
		for (uint32_t i = 0; i < SIFT_VK_GRANULE_ROWS; i++)
			h->kl[base + i] = (uint8_t) v[i];
		dec_chunk(&b->chunk[B_QTY][g], v);
		for (uint32_t i = 0; i < SIFT_VK_GRANULE_ROWS; i++)
			h->qty[base + i] = (uint8_t) v[i];
		dec_chunk(&b->chunk[B_AMOUNT][g], v);
		for (uint32_t i = 0; i < SIFT_VK_GRANULE_ROWS; i++)
			h->amount[base + i] = (uint32_t) v[i];
		dec_chunk(&b->chunk[B_PRICE][g], v);
		for (uint32_t i = 0; i < SIFT_VK_GRANULE_ROWS; i++)
			h->price[base + i] = f8_from_key(v[i]);
	}
	free(v);
}

/* ---- the device jobs ---- */
/* the columns each shape reads: the granules of a job name only those, as the host does */
static const uint32_t need_mask[5] = {1u << B_QTY | 1u << B_AMOUNT | 1u << B_PRICE, 1u << B_KS | 1u << B_AMOUNT | 1u << B_PRICE,
	1u << B_KM | 1u << B_KL | 1u << B_QTY | 1u << B_AMOUNT, 1u << B_QTY | 1u << B_AMOUNT | 1u << B_PRICE, 1u << B_AMOUNT};

void
job_init(Job *j, const Bench *b, SiftVkMode m)
{
	memset(j, 0, sizeof(*j));
	j->j = (SiftVkJob) {.mode = m, .ncols = B_NCOLS, .ngran = b->ngran, .where_slot = 255};
}

void
jagg(Job *j, uint8_t kind, uint8_t vreg)
{
	j->j.agg[j->j.naggs++] = (SiftVkAgg) {kind, vreg, 255};
}

void
job_seal(Job *j)
{
	uint64_t	mask = 0;

	j->j.nops = j->p.n;
	j->j.ops = j->p.w;
	for (int c = 0; c < B_NCOLS; c++)
		mask |= (uint64_t) (j->vg[0].col[c] != NULL) << c;
	j->j.tab = (SiftVkTabKey) {.rel = 0xB3C4, .manifest_gen = 1, .col_mask = {mask}, .granules = j->j.ngran};
}

void
make_job(Job *j, const Bench *b, int shape)
{
	Prog	   *p = &j->p;

	job_init(j, b, shape == 1 ? SIFT_VK_DENSE : shape == 2 ? SIFT_VK_HASH : shape == 4 ? SIFT_VK_TOPN : SIFT_VK_UNGROUPED);
	if (shape == 0)
	{
		P_COL(p, 0, B_QTY, SIFT_VK_T_I64);
		P_COL(p, 1, B_AMOUNT, SIFT_VK_T_I64);
		P_COL(p, 2, B_PRICE, SIFT_VK_T_F64);
		P_CMPC(p, SIFT_VK_T_I64, 1, 0, 10, SIFT_VK_CMP_GT);
		j->j.where_slot = 1;
		jagg(j, SIFT_VK_AK_COUNT_STAR, 255);
		jagg(j, SIFT_VK_AK_SUM_I, 1);
		jagg(j, SIFT_VK_AK_MIN, 0);
		jagg(j, SIFT_VK_AK_MAX, 0);
		jagg(j, SIFT_VK_AK_SUM_F, 2);
	}
	else if (shape == 1)
	{
		P_COL(p, 0, B_KS, SIFT_VK_T_I64);
		P_COL(p, 1, B_AMOUNT, SIFT_VK_T_I64);
		P_COL(p, 2, B_PRICE, SIFT_VK_T_F64);
		j->j.nkeys = 1;
		j->j.dense_span[0] = 101;
		jagg(j, SIFT_VK_AK_COUNT_STAR, 255);
		jagg(j, SIFT_VK_AK_SUM_I, 1);
		jagg(j, SIFT_VK_AK_SUM_F, 2);
	}
	else if (shape == 2)
	{
		P_COL(p, 0, B_KM, SIFT_VK_T_I64);
		P_COL(p, 1, B_KL, SIFT_VK_T_I64);
		P_COL(p, 2, B_QTY, SIFT_VK_T_I64);
		P_COL(p, 3, B_AMOUNT, SIFT_VK_T_I64);
		j->j.nkeys = 2;
		j->j.key_reg[1] = 1;
		j->j.est_groups = 1000000;
		jagg(j, SIFT_VK_AK_COUNT_STAR, 255);
		jagg(j, SIFT_VK_AK_SUM_I, 3);
		jagg(j, SIFT_VK_AK_MAX, 2);
	}
	else if (shape == 3)
	{
		P_COL(p, 0, B_QTY, SIFT_VK_T_I64);
		P_COL(p, 1, B_AMOUNT, SIFT_VK_T_I64);
		P_COL(p, 2, B_PRICE, SIFT_VK_T_F64);
		P_BIN(p, SIFT_VK_OP_MUL, SIFT_VK_T_I64, 3, 1, 0, 0);
		P_BINC(p, SIFT_VK_OP_ADD, SIFT_VK_T_I64, 4, 0, 1, 0);
		P_BIN(p, SIFT_VK_OP_DIV, SIFT_VK_T_I64, 5, 1, 4, 0);
		P_BIN(p, SIFT_VK_OP_SUB, SIFT_VK_T_I64, 6, 3, 5, 0);
		p_op(p, SIFT_VK_OP_CAST, SIFT_VK_T_I64, SIFT_VK_T_F64, 0, 7, 0, 0, 0, 0, 0, 0, 0);
		P_BIN(p, SIFT_VK_OP_MUL, SIFT_VK_T_F64, 8, 2, 7, 0);
		{
			double		k = 1.0725;
			int64_t		bits;

			memcpy(&bits, &k, 8);
			P_BINC(p, SIFT_VK_OP_MUL, SIFT_VK_T_F64, 9, 8, bits, 0);
		}
		jagg(j, SIFT_VK_AK_COUNT_STAR, 255);
		jagg(j, SIFT_VK_AK_SUM_I, 6);
		jagg(j, SIFT_VK_AK_MAX, 6);
		jagg(j, SIFT_VK_AK_SUM_F, 9);
	}
	else
	{
		P_COL(p, 0, B_AMOUNT, SIFT_VK_T_I64);
		j->j.topn_desc = true;
		j->j.topn_k = 100;
	}
	j->vg = malloc(sizeof(SiftVkGranule) * b->ngran);
	for (uint32_t g = 0; g < b->ngran; g++)
	{
		j->vg[g] = b->vg[g];
		for (int c = 0; c < B_NCOLS; c++)
			if (!((need_mask[shape] >> c) & 1))
				j->vg[g].col[c] = NULL;
	}
	j->j.gran = j->vg;
	job_seal(j);
}

static bool
near(double a, double b)
{
	return fabs(a - b) <= 1e-9 * fmax(1.0, fmax(fabs(a), fabs(b)));
}

typedef struct Cand
{
	int64_t		key;
	uint64_t	tag;
} Cand;

static bool
gpu_matches(int shape, const SiftVkResult *g, const Res *c)
{
	if (shape == 0)
	{
		double		fs;

		memcpy(&fs, &g->acc[1 + 3 * 4], 8);
		return g->acc[0] == c->cnt && g->acc[1 + 3] + (g->acc[1 + 4] << 32) == c->sum && g->acc[1 + 6] == c->mn && g->acc[1 + 9] == c->mx &&
			near(fs, c->fsum);
	}
	if (shape == 1)
	{
		for (int k = 0; k < 100; k++)
		{
			const int64_t *w = g->acc + (size_t) k * 10;
			double		fs;

			memcpy(&fs, &w[7], 8);
			if (w[0] != c->dcnt[k] || w[4] + (w[5] << 32) != c->dsum[k] || !near(fs, c->dfsum[k]))
				return false;
		}
		return true;
	}
	if (shape == 2)
	{
		uint64_t	chk = 0;

		if (g->ngroups != c->groups)
			return false;
		for (uint32_t i = 0; i < g->ngroups; i++)
		{
			const int64_t *w = g->acc + (size_t) i * g->rec_words;
			uint32_t	key = (uint32_t) (w[0] * 100 + w[1]);
			int64_t		sum = w[7] + (w[8] << 32);

			chk += mixk(key) * (uint64_t) (w[3] + 31 * sum + 131 * w[10]);
		}
		return chk == c->chk;
	}
	if (shape == 3)
	{
		double		fs;

		memcpy(&fs, &g->acc[1 + 3 * 3], 8);
		return g->acc[0] == c->cnt && g->acc[1 + 3] + (g->acc[1 + 4] << 32) == c->sum && g->acc[1 + 6] == c->mx && near(fs, c->fsum);
	}
	if (g->ncand != c->ntop)
		return false;
	for (uint32_t i = 0; i < g->ncand; i++)
		if (((const Cand *) g->cand)[i].key != c->top[i])
			return false;
	return true;
}

static int
cmp_d(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return x < y ? -1 : x > y;
}

int
main(int argc, char **argv)
{
	static const char *names[] = {"ungrouped agg (WHERE qty>10)", "dense group-by (100 groups)", "hash group-by (2 keys, ~1M groups)",
		"expression agg (int + double math)", "unsorted top-100"};
	uint32_t	sizes[4] = {10, 50, 0, 0},
				max_rows = 0;
	int			nsizes = 2,
				mismatches = 0;
	char		err[256];
	const char *dev = "";
	Bench	   *bp;
	Ctx		   *cp;
	Hot		   *hp;

	nworkers = (int) sysconf(_SC_NPROCESSORS_ONLN);
	if (nworkers > 64)
		nworkers = 64;
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--rows") && i + 1 < argc)
		{
			nsizes = 0;
			for (char *t = strtok(argv[++i], ","); t && nsizes < 4; t = strtok(NULL, ","))
				sizes[nsizes++] = (uint32_t) atoi(t);
		}
		else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "--reps") && i + 1 < argc)
			reps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cache-mb") && i + 1 < argc)
			cache_mb = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--workers") && i + 1 < argc)
			nworkers = atoi(argv[++i]);
	}
	for (int si = 0; si < nsizes; si++)
		max_rows = sizes[si] > max_rows ? sizes[si] : max_rows;
	max_rows = (max_rows + 1) * 1000000u + SIFT_VK_GRANULE_ROWS;
	bp = shm(sizeof(Bench));
	cp = shm(sizeof(Ctx));
	hp = shm(sizeof(Hot));
	arena = shm((size_t) max_rows * 16 + ((size_t) 64 << 20));
	cp->part = shm(sizeof(Res) * nworkers);
	cp->merged = shm(sizeof(Res) * nworkers);
	cp->ht = shm(((size_t) sizeof(HEnt) << HT_BITS) * nworkers);
	cp->ents = shm((size_t) sizeof(HEnt) * (1 << 20) * nworkers);
	cp->bucket_off = shm(sizeof(uint32_t) * (nworkers + 1) * nworkers);
	hp->ks = shm(max_rows);
	hp->kl = shm(max_rows);
	hp->qty = shm(max_rows);
	hp->km = shm((size_t) max_rows * 2);
	hp->amount = shm((size_t) max_rows * 4);
	hp->price = shm((size_t) max_rows * 8);
	cp->b = bp;
	cp->hot = hp;
	codec_init();
	pool_start();
	printf("cpu workers: %d (persistent processes)   repetitions: %d (median)\n", nworkers, reps);
	for (int si = 0; si < nsizes; si++)
	{
		Res			cres[5];
		double		cpu_enc[5], cpu_hot[5];
		SiftVk	   *vk;
		uint32_t	ngran = (uint32_t) ((uint64_t) sizes[si] * 1000000 / SIFT_VK_GRANULE_ROWS) + 1;
		double		t0 = now_s();

		printf("\n== %u granules, %llu rows ==\n", ngran, (unsigned long long) ((uint64_t) ngran * SIFT_VK_GRANULE_ROWS));
		build_bench(bp, ngran);
		printf("data built in %.1f s; stored bytes per column (ks km kl qty price amount): ", now_s() - t0);
		for (int k = 0; k < B_NCOLS; k++)
			printf("%.0f MB ", bp->enc_bytes[k] / 1e6);
		printf("\n");
		cp->wbits = log2_ceil(2 * (bp->rows / nworkers < (1u << 20) ? bp->rows / nworkers : (1u << 20)));
		cp->wbits = cp->wbits < 12 ? 12 : cp->wbits > HT_BITS ? HT_BITS : cp->wbits;
		cp->mbits = log2_ceil(3 * (1000000 / nworkers + 1));
		cp->decode = false;
		parallel(hot_decode_worker, cp);
		for (int s = 0; s < 5; s++)
		{
			double		t[16];
			Res			r;

			for (int mode = 0; mode < 2; mode++)
			{
				cp->decode = mode == 0;
				for (int k = 0; k < reps; k++)
					t[k] = cpu_run(cp, s, &r);
				qsort(t, reps, sizeof(double), cmp_d);
				(mode == 0 ? cpu_enc : cpu_hot)[s] = t[reps / 2];
			}
			cres[s] = r;
		}
		t0 = now_s();
		vk = sift_vk_open(dev, false, cache_mb << 20, err, sizeof(err));
		if (!vk)
		{
			fprintf(stderr, "no device: %s\n", err);
			return 2;
		}
		printf("device open (device, cache, the general pipelines): %.0f ms\n", (now_s() - t0) * 1e3);
		printf("device %s, PCIe %.2f GB/s, cache %llu MiB\n\n", sift_vk_caps(vk)->name, sift_vk_caps(vk)->pcie_gbps, (unsigned long long) (sift_vk_caps(vk)->cache_bytes >> 20));
		printf("| shape | rows | cpu-enc ms | cpu-hot ms | GPU cold ms | GPU warm ms | warm kernel ms | uploaded MB | cold vs cpu-enc | warm vs cpu-enc | warm vs cpu-hot | first-use compile ms | match |\n");
		printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
		for (int s = 0; s < 5; s++)
		{
			Job		   *j = malloc(sizeof(Job));
			SiftVkResult res;
			double		cold,
						compile,
						up,
						warm[16],
						kern = 0,
						host = 0;
			bool		ok;
			SiftVkStatus st;

			make_job(j, bp, s);
			j->j.ngran = 2;
			sift_vk_run(vk, &j->j, NULL, &res);
			compile = res.times.compile_ms;
			j->j.ngran = bp->ngran;
			sift_vk_set_cache(vk, cache_mb << 20);
			st = sift_vk_run(vk, &j->j, NULL, &res);
			if (st != SIFT_VK_OK)
			{
				printf("| %s | status %d: %s |\n", names[s], st, sift_vk_error(vk));
				mismatches++;
				free(j->vg);
				free(j);
				continue;
			}
			cold = res.times.wall_ms;
			up = res.bytes_uploaded / 1e6;
			ok = gpu_matches(s, &res, &cres[s]);
			for (int k = 0; k < reps; k++)
			{
				st = sift_vk_run(vk, &j->j, NULL, &res);
				warm[k] = res.times.wall_ms;
				kern += res.times.kernel_ms;
				host += res.times.stage_ms;
				ok = ok && st == SIFT_VK_OK && gpu_matches(s, &res, &cres[s]);
			}
			qsort(warm, reps, sizeof(double), cmp_d);
			if (getenv("VKB_VERBOSE"))
				fprintf(stderr, "  shape %d warm: table %s, batches %u, host build %.3f ms, kernel %.2f ms, wall %.2f ms\n", s, res.tab_hit ? "hit" : "miss", res.batches, host / reps, kern / reps, warm[reps / 2]);
			printf("| %s | %llu | %.1f | %.1f | %.1f | %.1f | %.2f | %.0f | %.1fx | %.1fx | %.1fx | %.0f | %s |\n", names[s], (unsigned long long) bp->rows, cpu_enc[s], cpu_hot[s],
				   cold, warm[reps / 2], kern / reps, up, cpu_enc[s] / cold, cpu_enc[s] / warm[reps / 2], cpu_hot[s] / warm[reps / 2], compile, ok ? "exact" : "MISMATCH");
			mismatches += !ok;
			free(j->vg);
			free(j);
		}
		bench_probes(vk, bp);
		sift_vk_close(vk);
		free_bench(bp);
	}
	pool_stop();
	printf("\nmismatches: %d\n", mismatches);
	return mismatches != 0;
}
