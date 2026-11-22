/*
 * test/unit/f2_delta_locate_test.c - the merge of the delta tail of a deletion vector (col/f2/delvec_delta.c) and the interval tree that locates a row among the parts (col/f2/f2_locate.c),
 * each compared with the plain algorithm it replaced on randomized input and timed at N and 8N (see test/unit/f2_scaling_test.c).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/unit/pg_shim.h"
#include "test/unit/harness.h"

#include "col/f2/delvec_delta.c"
#include "col/f2/f2_locate.c"

/* ---- [ON-F2-08] the delta tail of a deletion vector ---- */

static bool
ops_committed(void *ctx, TransactionId xmax)
{
	return true;
}

static FullTransactionId
ops_fxid(void *ctx, TransactionId xmax)
{
	return FullTransactionIdFromU64((uint64) xmax);
}

/* The rows a chunk adds to the granule, one chunk at a time as before: the walk over the granules, the copy of its rows (the first deleter of a row keeps it) */
static void
ref_add_rows(SiftDv *dv, uint32 gid, uint16 grows, const uint16 *rows, uint32 n, SiftDvXmaxEntry x, const SiftDvOps *ops)
{
	int			i = 0;
	SiftDvGran *g;
	uint16	   *nr;
	SiftDvXmaxEntry *nx;
	int			k = 0,
				a = 0,
				b = 0;

	while (i < dv->ndirty && dv->gran[i].granule_id < gid)
		i++;
	if (i == dv->ndirty || dv->gran[i].granule_id != gid)
	{
		dv->gran = realloc(dv->gran, (dv->ndirty + 1) * sizeof(SiftDvGran));
		memmove(&dv->gran[i + 1], &dv->gran[i], (dv->ndirty - i) * sizeof(SiftDvGran));
		dv->gran[i] = (SiftDvGran) {.granule_id = gid,.granule_rows = grows,.rows = malloc(sizeof(uint16)),.xmax = malloc(sizeof(SiftDvXmaxEntry))};
		dv->ndirty++;
	}
	g = &dv->gran[i];
	nr = malloc((g->ndead + n) * sizeof(uint16) + 1);
	nx = malloc((g->ndead + n) * sizeof(SiftDvXmaxEntry) + 1);
	while (a < g->ndead || b < (int) n)
	{
		if (b == (int) n || (a < g->ndead && g->rows[a] <= rows[b]))
		{
			if (b < (int) n && g->rows[a] == rows[b])
				b++;
			nr[k] = g->rows[a];
			nx[k++] = g->xmax[a++];
		}
		else
		{
			nr[k] = rows[b++];
			nx[k++] = x;
		}
	}
	free(g->rows);
	free(g->xmax);
	g->rows = nr;
	g->xmax = nx;
	g->ndead = k;
	if (TransactionIdIsValid(x.xmax))
	{
		FullTransactionId f = ops->fxid(ops->ctx, x.xmax);

		if (!FullTransactionIdIsValid(g->xid_max) || FullTransactionIdFollows(f, g->xid_max))
			g->xid_max = f;
	}
}

typedef struct TailFix
{
	char	   *bytes;
	uint32		len;
	SiftDv	   *base;
	uint16		grows[64];
	int			nchunks;
	struct
	{
		uint32		gid;
		int			nmarks;
		uint16		rows[48];
		TransactionId xmax;
	}		   *chunks;
} TailFix;

static SiftDv *
base_dv(int nbase)
{
	SiftDv	   *dv = calloc(1, sizeof(SiftDv));

	dv->ngranules = 64;
	dv->gran = calloc(64, sizeof(SiftDvGran));
	for (int g = 0; g < 64 && dv->ndirty * 4 < nbase; g += 3)
	{
		SiftDvGran *gr = &dv->gran[dv->ndirty++];
		int			n = 0;

		gr->granule_id = g;
		gr->granule_rows = 8192;
		gr->rows = malloc(nbase * sizeof(uint16) + 1);
		gr->xmax = malloc(nbase * sizeof(SiftDvXmaxEntry) + 1);
		for (int r = (int) (rnd() % 40); r < 8192 && n < nbase / 16 + 1; r += 1 + (int) (rnd() % 200))
		{
			gr->rows[n] = r;
			gr->xmax[n++] = (SiftDvXmaxEntry) {.xmax = 7,.cmax = 0};
		}
		gr->ndead = n;
	}
	return dv;
}

static SiftDv *
copy_dv(const SiftDv *a)
{
	SiftDv	   *dv = calloc(1, sizeof(SiftDv));

	*dv = *a;
	dv->gran = calloc(Max(a->ndirty, 1), sizeof(SiftDvGran));
	for (int i = 0; i < a->ndirty; i++)
	{
		dv->gran[i] = a->gran[i];
		dv->gran[i].rows = memcpy(malloc(a->gran[i].ndead * sizeof(uint16) + 1), a->gran[i].rows, a->gran[i].ndead * sizeof(uint16));
		dv->gran[i].xmax = memcpy(malloc(a->gran[i].ndead * sizeof(SiftDvXmaxEntry) + 1), a->gran[i].xmax, a->gran[i].ndead * sizeof(SiftDvXmaxEntry));
	}
	return dv;
}

static void
free_dv(SiftDv *dv)
{
	for (int i = 0; i < dv->ndirty; i++)
	{
		free(dv->gran[i].rows);
		free(dv->gran[i].xmax);
	}
	free(dv->gran);
	free(dv);
}

/* nchunks chunks over 64 granules, each of up to 48 ascending rows, the rows overlapping those of other chunks and of the base */
static void
tail_fix(TailFix *f, int nchunks, int nbase)
{
	uint32		pos = 0;
	uint32		cap = nchunks * 128 + 16;

	*f = (TailFix) {.nchunks = nchunks};
	f->bytes = calloc(1, cap);
	f->base = base_dv(nbase);
	for (int g = 0; g < 64; g++)
		f->grows[g] = 8192;
	for (int c = 0; c < nchunks; c++)
	{
		SiftDvDeltaChunkHdr h = {.magic = SIFT_DV_DELTA_MAGIC,.granule_id = (uint16) (rnd() % 64),.xmax = 100 + (TransactionId) (rnd() % 5),.cmax = (CommandId) (rnd() % 3)};
		uint16		rows[48];
		int			n = 0;

		for (int r = (int) (rnd() % 300); r < 8192 && n < 1 + (int) (rnd() % 48); r += 1 + (int) (rnd() % 300))
			rows[n++] = r;
		h.nmarks = n;
		memcpy(f->bytes + pos, &h, sizeof(h));
		memcpy(f->bytes + pos + sizeof(h), rows, n * sizeof(uint16));
		pos += SIFT_DV_CHUNK_BYTES(n);
	}
	f->len = pos;
}

static void
dv_same(const SiftDv *a, const SiftDv *b, const char *what)
{
	CHECK(a->ndirty == b->ndirty, "%s: %d granules, reference %d", what, a->ndirty, b->ndirty);
	for (int i = 0; i < Min(a->ndirty, b->ndirty); i++)
	{
		const SiftDvGran *x = &a->gran[i];
		const SiftDvGran *y = &b->gran[i];

		CHECK(x->granule_id == y->granule_id && x->ndead == y->ndead && x->xid_max.value == y->xid_max.value, "%s: granule %u differs", what, x->granule_id);
		if (x->ndead != y->ndead)
			continue;
		for (int k = 0; k < x->ndead; k++)
			CHECK(x->rows[k] == y->rows[k] && x->xmax[k].xmax == y->xmax[k].xmax && x->xmax[k].cmax == y->xmax[k].cmax, "%s: granule %u row %d", what, x->granule_id, k);
	}
}

static const SiftDvOps test_ops = {.committed = ops_committed,.fxid = ops_fxid};

static double
tail_run_fix(int nchunks, bool verify)
{
	TailFix		f;
	SiftDv	   *neu;
	double		t0;
	const char *why;

	tail_fix(&f, nchunks, 4000);
	neu = copy_dv(f.base);
	t0 = now_s();
	why = read_tail(neu, f.bytes, 0, f.len, f.grows, 64, &test_ops);
	t0 = now_s() - t0;
	CHECK(why == NULL, "read_tail: %s", why ? why : "");
	if (verify)
	{
		SiftDv	   *ref = copy_dv(f.base);

		for (uint32 pos = 0; pos < f.len;)
		{
			SiftDvDeltaChunkHdr h;

			memcpy(&h, f.bytes + pos, sizeof(h));
			ref_add_rows(ref, h.granule_id, 8192, (const uint16 *) (f.bytes + pos + sizeof(h)), h.nmarks, (SiftDvXmaxEntry) {h.xmax, h.cmax}, &test_ops);
			pos += SIFT_DV_CHUNK_BYTES(h.nmarks);
		}
		dv_same(neu, ref, "delta tail");
		free_dv(ref);
	}
	free_dv(neu);
	free_dv(f.base);
	free(f.bytes);
	return t0;
}

static double tail_run(int n) { return tail_run_fix(n, false); }

static void
test_delta_tail(void)
{
	const char *why;
	SiftDv	   *dv = base_dv(100);
	char		bad[32] = {0};
	uint16		grows[64];

	for (int i = 0; i < 64; i++)
		grows[i] = 8192;
	for (int n = 1; n < 400; n = n * 2 + 1)
		(void) tail_run_fix(n, true);
	(void) tail_run_fix(500, true);
	why = read_tail(dv, bad, 0, 16, grows, 64, &test_ops);
	CHECK(why != NULL, "a chunk with no magic is refused");
	free_dv(dv);
	report("delta tail merge", tail_run, 500, 20.0);
	for (int n = 500; n <= 4000; n *= 8)
	{
		TailFix		f;
		SiftDv	   *ref;
		double		t0;

		tail_fix(&f, n, 4000);
		ref = copy_dv(f.base);
		t0 = now_s();
		for (uint32 pos = 0; pos < f.len;)
		{
			SiftDvDeltaChunkHdr h;

			memcpy(&h, f.bytes + pos, sizeof(h));
			ref_add_rows(ref, h.granule_id, 8192, (const uint16 *) (f.bytes + pos + sizeof(h)), h.nmarks, (SiftDvXmaxEntry) {h.xmax, h.cmax}, &test_ops);
			pos += SIFT_DV_CHUNK_BYTES(h.nmarks);
		}
		printf("%-28s N=%-6d chunk by chunk (the code replaced) %9.3f ms\n", "delta tail reference", n, (now_s() - t0) * 1e3);
		free_dv(ref);
		free_dv(f.base);
		free(f.bytes);
	}
}

/* ---- [ON-F2-04] the intervals of the parts by address ---- */

typedef struct WalkRef
{
	int			seen[4096];
	int			n;
	int			want[4096];
	int			nwant;
}			WalkRef;

static bool
walk_hit(int i, void *arg)
{
	WalkRef    *w = arg;

	w->seen[w->n++] = i;
	return false;
}

static double
tree_run_fix(int parts, bool verify)
{
	uint64	   *maxes = malloc(parts * sizeof(uint64));
	uint64	   *mins = malloc(parts * sizeof(uint64));
	IntervalTree t;
	double		t0;
	uint64		sink = 0;

	for (int i = 0; i < parts; i++)
	{
		uint64		lo = (uint64) i * 1000 + rnd() % 400;

		mins[i] = i == 0 ? 0 : lo;
		maxes[i] = i == 0 ? UINT64_MAX : lo + 500 + rnd() % 300;
	}
	t = tree_build(CurrentMemoryContext, maxes, parts);
	t0 = now_s();
	for (int q = 0; q < 50000; q++)
	{
		uint64		tid = rnd() % ((uint64) parts * 1000 + 1000);
		int			first = 0,
					last = parts;
		static WalkRef w;

		while (first < last)
		{
			int			mid = first + (last - first) / 2;

			if (mins[mid] <= tid)
				first = mid + 1;
			else
				last = mid;
		}
		w.n = 0;
		(void) tree_walk(&t, first, tid, walk_hit, &w);
		sink += w.n;
		if (verify && w.n < 4096)
		{
			int			k = 0;

			for (int i = first - 1; i >= 0; i--)
				if (maxes[i] >= tid)
				{
					CHECK(k < w.n && w.seen[k] == i, "interval walk: place %d is %d, reference %d (tid %llu)", k, k < w.n ? w.seen[k] : -1, i, (unsigned long long) tid);
					k++;
				}
			CHECK(k == w.n, "interval walk: %d hits, reference %d", w.n, k);
		}
	}
	t0 = now_s() - t0;
	free(t.max);
	free(maxes);
	free(mins);
	return sink == UINT64_MAX ? 0 : t0;
}

static double tree_run(int n) { return tree_run_fix(n, false); }

static void
test_interval_tree(void)
{
	(void) tree_run_fix(5, true);
	(void) tree_run_fix(300, true);
	report("interval tree locate", tree_run, 2000, 3.0);
}

int
main(void)
{
	test_delta_tail();
	test_interval_tree();
	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures != 0;
}
