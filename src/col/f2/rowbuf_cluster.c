/*
 * col/rowbuf_cluster.c - the order of the rows of a part of the warm and cold tiers among rows that the sort columns leave tied (col/rowbuf.h, sift_rowbuf_cluster).
 *
 * The sort columns (or none) fix which rows are near each other; inside a run of rows that tie on them, any order is a valid part (the key range of a part is the least and the greatest
 * key of its rows, and no reader assumes the addresses ascend inside a run), so the run is put in the order that makes the chunks of a few low-cardinality columns shortest: the columns
 * with the fewest distinct values first, each run of a value traversing the next column up and then down. With no sort column and no low-cardinality column, a Hilbert curve over two
 * columns keeps rows that are near in both near. Whatever the order costs the row addresses (a part appended in order holds them as a sequence) is counted: the order is kept only
 * when the first granule is at least half a byte a row smaller in the chunks of those columns and its addresses together.
 */
#include "core/host.h"

#include "common/hashfn.h"
#include "lib/stringinfo.h"

#include "col/granule/granule_int.h"
#include "col/f2/rowbuf.h"

#define SIFT_CLUSTER_MIN_ROWS	256
#define SIFT_CLUSTER_SAMPLE		1024
#define SIFT_CLUSTER_MAX_NDV	256
#define SIFT_CLUSTER_MAX_COLS	3
#define SIFT_CLUSTER_MIN_RUN	4.0
#define SIFT_CLUSTER_HASH_SLOTS	2048
#define SIFT_CLUSTER_RANKS		65536

typedef enum ClusterKind
{
	CLUSTER_SNAKE,
	CLUSTER_HILBERT
} ClusterKind;

typedef struct ClusterPlan
{
	const SiftRowBuf *rb;
	const SiftPartSpec *spec;
	ClusterKind kind;
	int			ncols;
	int			cols[SIFT_CLUSTER_MAX_COLS];
	const uint32 *hilbert;		/* CLUSTER_HILBERT: the position of each row on the curve */
} ClusterPlan;

static bool
same_cell(const SiftRowBuf *rb, int c, int r1, int r2)
{
	if (rb->isnull[c][r1] || rb->isnull[c][r2])
		return rb->isnull[c][r1] == rb->isnull[c][r2];
	return sift_rowbuf_cmp_val(rb, c, r1, r2) == 0;
}

static int
cell_cmp(const SiftRowBuf *rb, int c, int r1, int r2)
{
	if (rb->isnull[c][r1] || rb->isnull[c][r2])
		return rb->isnull[c][r1] == rb->isnull[c][r2] ? 0 : (rb->isnull[c][r1] ? 1 : -1);
	return sift_rowbuf_cmp_val(rb, c, r1, r2);
}

static bool
same_group(const SiftRowBuf *rb, const SiftPartSpec *spec, int r1, int r2)
{
	for (int k = 0; k < spec->nsort; k++)
		if (!same_cell(rb, spec->sort_cols[k], r1, r2))
			return false;
	return true;
}

static int
snake_cmp(const void *a, const void *b, void *arg)
{
	const ClusterPlan *p = arg;
	int			r1 = *(const int *) a,
				r2 = *(const int *) b;

	for (int k = 0; k < p->ncols; k++)
	{
		int			cmp = cell_cmp(p->rb, p->cols[k], r1, r2);

		if (cmp != 0)
			return cmp;
	}
	return p->rb->tid[r1] < p->rb->tid[r2] ? -1 : p->rb->tid[r1] > p->rb->tid[r2];
}

static int
hilbert_cmp(const void *a, const void *b, void *arg)
{
	const ClusterPlan *p = arg;
	int			r1 = *(const int *) a,
				r2 = *(const int *) b;

	if (p->hilbert[r1] != p->hilbert[r2])
		return p->hilbert[r1] < p->hilbert[r2] ? -1 : 1;
	return p->rb->tid[r1] < p->rb->tid[r2] ? -1 : p->rb->tid[r1] > p->rb->tid[r2];
}

static void
reverse_rows(int *perm, int lo, int hi)
{
	for (hi--; lo < hi; lo++, hi--)
	{
		int			t = perm[lo];

		perm[lo] = perm[hi];
		perm[hi] = t;
	}
}

/* The runs of equal values of column c inside [lo, hi) in the opposite order, the rows of each run as they were */
static void
reverse_runs(const ClusterPlan *p, int *perm, int lo, int hi, int c)
{
	reverse_rows(perm, lo, hi);
	for (int a = lo; a < hi;)
	{
		int			b = a + 1;

		while (b < hi && same_cell(p->rb, c, perm[a], perm[b]))
			b++;
		reverse_rows(perm, a, b);
		a = b;
	}
}

/* [lo, hi), ordered by the columns from `level`, as a snake: the runs of the value of each column are visited up and down in turn */
static void
snake(const ClusterPlan *p, int *perm, int lo, int hi, int level, bool flip)
{
	int			j = 0;

	if (flip)
		reverse_runs(p, perm, lo, hi, p->cols[level]);
	if (level + 1 >= p->ncols)
		return;
	for (int a = lo; a < hi; j++)
	{
		int			b = a + 1;

		while (b < hi && same_cell(p->rb, p->cols[level], perm[a], perm[b]))
			b++;
		snake(p, perm, a, b, level + 1, j & 1);
		a = b;
	}
}

/* perm, the identity of rows [0, limit), put in the order of the plan: inside each run of rows that tie on the sort columns */
static void
order_rows(const ClusterPlan *p, int *perm, int limit)
{
	int			g = 0;

	for (int lo = 0; lo < limit; g++)
	{
		int			hi = lo + 1;

		while (hi < limit && same_group(p->rb, p->spec, lo, hi))
			hi++;
		if (hi - lo >= 2)
		{
			qsort_arg(perm + lo, hi - lo, sizeof(int), p->kind == CLUSTER_HILBERT ? hilbert_cmp : snake_cmp, (void *) p);
			if (p->kind == CLUSTER_SNAKE)
				snake(p, perm, lo, hi, 0, g & 1);
		}
		lo = hi;
	}
}

static uint64
cell_hash(const SiftRowBuf *rb, int c, int r)
{
	uint64		v;

	if (rb->isnull[c][r])
		return UINT64CONST(0x9E3779B97F4A7C15);
	if (rb->kinds[c] == SIFT_CELL_INT)
		return hash_bytes_uint32_extended((uint32) rb->i[c][r], (uint64) (rb->i[c][r] >> 32)) | 1;
	if (rb->kinds[c] == SIFT_CELL_F8)
	{
		memcpy(&v, &rb->f[c][r], 8);
		return hash_bytes_uint32_extended((uint32) v, v >> 32) | 1;
	}
	return ((uint64) hash_bytes((const unsigned char *) rb->s[c][r], (int) rb->slen[c][r]) << 1) | 1;
}

/* The distinct values of column c in a sample of the rows, or SIFT_CLUSTER_MAX_NDV + 1 as soon as there are more */
static int
sample_ndv(const SiftRowBuf *rb, int c)
{
	uint64		seen[SIFT_CLUSTER_HASH_SLOTS] = {0};
	int			sn = Min(rb->n, SIFT_CLUSTER_SAMPLE),
				ndv = 0;

	for (int i = 0; i < sn && ndv <= SIFT_CLUSTER_MAX_NDV; i++)
	{
		uint64		h = cell_hash(rb, c, (int) ((int64) i * rb->n / sn));
		uint32		slot = (uint32) (h >> 7) % SIFT_CLUSTER_HASH_SLOTS;

		while (seen[slot] != 0 && seen[slot] != h)
			slot = (slot + 1) % SIFT_CLUSTER_HASH_SLOTS;
		if (seen[slot] == 0)
		{
			seen[slot] = h;
			ndv++;
		}
	}
	return ndv;
}

static bool
clusterable(const SiftRowBuf *rb, const SiftPartSpec *spec, int c)
{
	if (sift_rowbuf_is_child(rb, c) || sift_rowbuf_is_group(rb, c) || (spec->hints != NULL && (spec->hints[c] & ~SIFT_CHINT_ASTEXT) == SIFT_CHINT_UUID))
		return false;
	for (int k = 0; k < spec->nsort; k++)
		if (spec->sort_cols[k] == c)
			return false;
	return true;
}

/* The columns of the plan: those with the fewest distinct values (between 2 and ndv_cap), the fewest first; how many */
static int
low_cardinality_cols(const SiftRowBuf *rb, const SiftPartSpec *spec, int ndv_cap, int *cols)
{
	int			ndv[SIFT_CLUSTER_MAX_COLS],
				k = 0;

	for (int c = 0; c < rb->ncols; c++)
	{
		int			d,
					j;

		if (!clusterable(rb, spec, c))
			continue;
		d = sample_ndv(rb, c);
		if (d < 2 || d > ndv_cap || (k == SIFT_CLUSTER_MAX_COLS && d >= ndv[k - 1]))
			continue;
		for (j = k < SIFT_CLUSTER_MAX_COLS ? k++ : k - 1; j > 0 && ndv[j - 1] > d; j--)
		{
			ndv[j] = ndv[j - 1];
			cols[j] = cols[j - 1];
		}
		ndv[j] = d;
		cols[j] = c;
	}
	return k;
}

static int64
cell_rank_key(const SiftRowBuf *rb, int c, int r)
{
	return rb->kinds[c] == SIFT_CELL_INT ? rb->i[c][r] : sift_col_f8_key(rb->f[c][r]);
}

static int
key_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* The position of row r of column c among a sample of the column, as 0 .. SIFT_CLUSTER_RANKS - 1 (the nulls last) */
static uint32
sample_rank(const SiftRowBuf *rb, int c, int r, const int64 *sample, int sn)
{
	int			lo = 0,
				hi = sn;
	int64		v;

	if (rb->isnull[c][r])
		return SIFT_CLUSTER_RANKS - 1;
	v = cell_rank_key(rb, c, r);
	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (sample[mid] <= v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (uint32) ((int64) lo * (SIFT_CLUSTER_RANKS - 1) / Max(sn, 1));
}

/* The position on a Hilbert curve of 65536 x 65536 cells */
static uint32
hilbert_pos(uint32 x, uint32 y)
{
	uint64		d = 0;

	for (uint32 s = SIFT_CLUSTER_RANKS / 2; s > 0; s >>= 1)
	{
		uint32		rx = (x & s) != 0,
					ry = (y & s) != 0;

		d += (uint64) s * s * ((3 * rx) ^ ry);
		if (ry == 0)
		{
			if (rx == 1)
			{
				x = SIFT_CLUSTER_RANKS - 1 - x;
				y = SIFT_CLUSTER_RANKS - 1 - y;
			}
			rx = x;
			x = y;
			y = rx;
		}
	}
	return (uint32) d;
}

/* Two columns of many distinct values (the time column first) to lay the rows out by, or false: how many were found */
static bool
curve_cols(const SiftRowBuf *rb, const SiftPartSpec *spec, int *cols)
{
	int			k = 0;

	for (int pass = 0; pass < 2 && k < 2; pass++)
		for (int c = 0; c < rb->ncols && k < 2; c++)
		{
			bool		first = spec->time_col > 0 && c == spec->time_col - 1;

			if ((rb->kinds[c] != SIFT_CELL_INT && rb->kinds[c] != SIFT_CELL_F8) || !clusterable(rb, spec, c) || first != (pass == 0))
				continue;
			if (sample_ndv(rb, c) > SIFT_CLUSTER_MAX_NDV)
				cols[k++] = c;
		}
	return k == 2;
}

static uint32 *
curve_positions(const SiftRowBuf *rb, const int *cols)
{
	int			sn = Min(rb->n, SIFT_CLUSTER_SAMPLE);
	uint32	   *pos = palloc(Max(rb->n, 1) * sizeof(uint32));	/* check-memory: allow a key of the position of each row of a buffer the caller already holds */
	uint32	   *xy = palloc(Max(rb->n, 1) * sizeof(uint32));	/* check-memory: allow the same rows */
	int64	   *sample = palloc(sn * sizeof(int64));

	for (int k = 0; k < 2; k++)
	{
		int			ns = 0;

		for (int i = 0; i < sn; i++)
		{
			int			r = (int) ((int64) i * rb->n / sn);

			if (!rb->isnull[cols[k]][r])
				sample[ns++] = cell_rank_key(rb, cols[k], r);
		}
		qsort(sample, ns, sizeof(int64), key_cmp);
		for (int r = 0; r < rb->n; r++)
		{
			uint32		rank = sample_rank(rb, cols[k], r, sample, ns);

			if (k == 0)
				xy[r] = rank;
			else
				pos[r] = hilbert_pos(xy[r], rank);
		}
	}
	pfree(xy);
	pfree(sample);
	return pos;
}

/* The bytes of the chunks of the columns of the plan and of the row addresses of the first n rows, in the order perm gives them (NULL: as they are) */
static uint64
measure(const ClusterPlan *p, const int *perm, int n)
{
	const SiftRowBuf *rb = p->rb;
	SiftChunkOpts opt = {.tier = SIFT_TIER_WARM_L1,.alg = p->spec->compress_alg,.dict = NULL};
	StringInfoData s;
	SiftChunkRef ref = {0};
	uint64	   *tid = palloc(n * sizeof(uint64));
	int64	   *iv = palloc(n * sizeof(int64));
	double	   *fv = palloc(n * sizeof(double));
	const char **sv = palloc(n * sizeof(char *));
	uint32	   *slen = palloc(n * sizeof(uint32));
	bool	   *isnull = palloc(n);
	uint64		bytes;

	initStringInfo(&s);
	for (int i = 0; i < n; i++)
		tid[i] = rb->tid[perm == NULL ? i : perm[i]];
	sift_chunk_put_tids(&s, tid, n, &ref, &opt);
	for (int k = 0; k < p->ncols; k++)
	{
		int			c = p->cols[k];
		SiftColIn	col = {.kind = rb->kinds[c],.i = iv,.f = fv,.s = sv,.slen = slen,.isnull = isnull,.hint = p->spec->hints ? p->spec->hints[c] & ~SIFT_CHINT_ASTEXT : SIFT_CHINT_AUTO,
		.astext = p->spec->hints && (p->spec->hints[c] & SIFT_CHINT_ASTEXT),.key_leaf = p->spec->key_leaf && p->spec->key_leaf[c],.scale = p->spec->scales ? p->spec->scales[c] : 0};

		for (int i = 0; i < n; i++)
		{
			int			r = perm == NULL ? i : perm[i];

			isnull[i] = rb->isnull[c][r];
			if (rb->kinds[c] == SIFT_CELL_INT)
				iv[i] = rb->i[c][r];
			else if (rb->kinds[c] == SIFT_CELL_F8)
				fv[i] = rb->f[c][r];
			else
			{
				sv[i] = rb->s[c][r];
				slen[i] = rb->slen[c][r];
			}
		}
		ref = (SiftChunkRef) {0};
		sift_chunk_put(&s, &col, n, &ref, &opt);
	}
	bytes = s.len;
	pfree(s.data);
	pfree(tid);
	pfree(iv);
	pfree(fv);
	pfree(sv);
	pfree(slen);
	pfree(isnull);
	return bytes;
}

static int
count_groups(const SiftRowBuf *rb, const SiftPartSpec *spec)
{
	int			groups = 1;

	for (int r = 1; r < rb->n; r++)
		groups += !same_group(rb, spec, r - 1, r);
	return groups;
}

bool
sift_rowbuf_cluster(SiftRowBuf *rb, const SiftPartSpec *spec, SiftCompressTier tier)
{
	ClusterPlan p = {.rb = rb,.spec = spec,.kind = CLUSTER_SNAKE};
	double		e_avg;
	int			g0 = Min(rb->n, SIFT_GRANULE_ROWS);
	int		   *perm;
	bool		moved;

	if (!SIFT_TIER_IS_COLD(tier) || rb->n < SIFT_CLUSTER_MIN_ROWS || spec->ncols <= spec->nsort)
		return false;
	e_avg = (double) rb->n / count_groups(rb, spec);
	if (e_avg < SIFT_CLUSTER_MIN_RUN)
		return false;
	p.ncols = low_cardinality_cols(rb, spec, spec->nsort > 0 ? (int) Min((double) SIFT_CLUSTER_MAX_NDV, e_avg / 2.0) : SIFT_CLUSTER_MAX_NDV, p.cols);
	if (p.ncols == 0 && spec->nsort == 0 && curve_cols(rb, spec, p.cols))
	{
		p.kind = CLUSTER_HILBERT;
		p.ncols = 2;
		p.hilbert = curve_positions(rb, p.cols);
	}
	if (p.ncols == 0)
		return false;
	perm = palloc(rb->n * sizeof(int));	/* check-memory: allow a permutation of the rows of a buffer the caller already holds */
	for (int r = 0; r < rb->n; r++)
		perm[r] = r;
	order_rows(&p, perm, g0);
	moved = (int64) measure(&p, NULL, g0) - (int64) measure(&p, perm, g0) >= g0 / 2;
	if (moved)
	{
		for (int r = 0; r < rb->n; r++)
			perm[r] = r;
		order_rows(&p, perm, rb->n);
		sift_rowbuf_permute(rb, perm);
	}
	pfree(perm);
	if (p.hilbert != NULL)
		pfree((void *) p.hilbert);
	return moved;
}

