/*
 * col/f2/stats.c - the writer, the reader and the priors of SEC_COL_STATS (col/f2/stats.h).
 */
#include "core/host.h"

#include <math.h>

#include "col/f2/stats.h"

#define ENC_TRACK		16

uint64
sift_col_stats_size(uint16 ncols, bool with_nulls)
{
	return sizeof(SiftColStatsHdr) + (uint64) ncols * sizeof(SiftPartColStat) + (with_nulls ? (uint64) ncols * sizeof(uint64) : 0);
}

const char *
sift_col_stats_open(const void *blob, uint64 len, uint16 ncols, uint32 ngranules, SiftColStats *cs)
{
	SiftColStatsHdr hdr;

	if (len < sizeof(hdr))
		return "the column statistics are shorter than their header";
	memcpy(&hdr, blob, sizeof(hdr));
	if (hdr.magic != SIFT_COL_STATS_MAGIC || hdr.version != SIFT_COL_STATS_VERSION)
		return "the column statistics have another magic or version";
	if (hdr.flags & ~SIFT_COLSTATS_HAS_NULLS)
		return "the column statistics have flags this build does not know";
	if (hdr.ncols != ncols || hdr.ngranules != ngranules)
		return "the column statistics are of another schema or part than the one read";
	if (len != sift_col_stats_size(hdr.ncols, (hdr.flags & SIFT_COLSTATS_HAS_NULLS) != 0))
		return "the column statistics are not the size their header says";
	cs->hdr = blob;
	cs->recs = (const uint8 *) blob + sizeof(hdr);
	cs->nulls = (hdr.flags & SIFT_COLSTATS_HAS_NULLS) ? cs->recs + (size_t) ncols * sizeof(SiftPartColStat) : NULL;
	return NULL;
}

SiftPartColStat
sift_col_stats_get(const SiftColStats *cs, int c)
{
	SiftPartColStat st;

	memcpy(&st, cs->recs + (size_t) c * sizeof(st), sizeof(st));
	return st;
}

uint64
sift_col_stats_nulls(const SiftColStats *cs, int c)
{
	uint64		n = 0;

	if (cs->nulls != NULL)
		memcpy(&n, cs->nulls + (size_t) c * sizeof(n), sizeof(n));
	return n;
}

/* ---- the sketch ---- */

static uint64
mix64(uint64 x)
{
	x += UINT64CONST(0x9E3779B97F4A7C15);
	x = (x ^ (x >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	x = (x ^ (x >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return x ^ (x >> 31);
}

void
sift_ndv_add_hash(SiftNdvSketch *s, uint64 h)
{
	uint32		idx = (uint32) (h >> 54);
	uint8		rank = (uint8) (__builtin_clzll((h << 10) | (UINT64CONST(1) << 9)) + 1);

	if (rank > s->reg[idx])
		s->reg[idx] = rank;
}

void
sift_ndv_add_i64(SiftNdvSketch *s, int64 v)
{
	sift_ndv_add_hash(s, mix64((uint64) v));
}

void
sift_ndv_add_bytes(SiftNdvSketch *s, const void *p, uint32 len)
{
	const uint8 *b = p;
	uint64		h = UINT64CONST(0xCBF29CE484222325);

	for (uint32 i = 0; i < len; i++)
		h = (h ^ b[i]) * UINT64CONST(0x100000001B3);
	sift_ndv_add_hash(s, mix64(h ^ len));
}

void
sift_ndv_merge(SiftNdvSketch *into, const SiftNdvSketch *from)
{
	for (int i = 0; i < SIFT_NDV_REGISTERS; i++)
		if (from->reg[i] > into->reg[i])
			into->reg[i] = from->reg[i];
}

uint64
sift_ndv_estimate(const SiftNdvSketch *s)
{
	const double m = SIFT_NDV_REGISTERS;
	double		sum = 0,
				e;
	int			zeros = 0;

	for (int i = 0; i < SIFT_NDV_REGISTERS; i++)
	{
		sum += ldexp(1.0, -s->reg[i]);
		zeros += s->reg[i] == 0;
	}
	e = (0.7213 / (1.0 + 1.079 / m)) * m * m / sum;
	if (e <= 2.5 * m && zeros > 0)
		e = m * log(m / zeros);
	return (uint64) (e + 0.5);
}

/* ---- the writer ---- */

typedef struct EncBytes
{
	uint16		enc;
	uint64		bytes;
} EncBytes;

typedef struct ColAcc
{
	uint64		rows;
	uint64		nulls;
	uint64		bytes;
	uint32		exact_ndv;
	bool		exact;
	EncBytes	enc[ENC_TRACK];
	int			nenc;
	SiftNdvSketch ndv;
} ColAcc;

struct SiftColStatsBuilder
{
	uint16		ncols;
	ColAcc	   *col;
};

SiftColStatsBuilder *
sift_col_stats_begin(uint16 ncols)
{
	SiftColStatsBuilder *b = palloc0(sizeof(SiftColStatsBuilder));

	b->ncols = ncols;
	b->col = palloc0(sizeof(ColAcc) * (ncols ? ncols : 1));
	return b;
}

SiftNdvSketch *
sift_col_stats_ndv(SiftColStatsBuilder *b, int c)
{
	return &b->col[c].ndv;
}

void
sift_col_stats_set_ndv_exact(SiftColStatsBuilder *b, int c, uint32 ndv)
{
	b->col[c].exact = true;
	b->col[c].exact_ndv = ndv;
}

/* The encodings by the bytes they took: the table keeps the heaviest ENC_TRACK, and a new one beyond them takes the place of the lightest */
static void
enc_add(ColAcc *a, uint16 enc, uint32 bytes)
{
	int			lightest = 0;

	for (int i = 0; i < a->nenc; i++)
	{
		if (a->enc[i].enc == enc)
		{
			a->enc[i].bytes += bytes;
			return;
		}
		if (a->enc[i].bytes < a->enc[lightest].bytes)
			lightest = i;
	}
	if (a->nenc < ENC_TRACK)
		a->enc[a->nenc++] = (EncBytes) {enc, bytes};
	else
		a->enc[lightest] = (EncBytes) {enc, a->enc[lightest].bytes + bytes};
}

void
sift_col_stats_granule(SiftColStatsBuilder *b, int c, uint32 nrows, uint32 nnull, uint32 chunk_bytes, uint16 enc)
{
	ColAcc	   *a = &b->col[c];

	a->rows += nrows;
	a->nulls += nnull;
	a->bytes += chunk_bytes;
	enc_add(a, enc, chunk_bytes);
}

void *
sift_col_stats_finish(SiftColStatsBuilder *b, uint32 ngranules, uint64 *len)
{
	bool		has_nulls = false;
	uint64		size;
	uint8	   *out;
	SiftColStatsHdr hdr = {.magic = SIFT_COL_STATS_MAGIC,.version = SIFT_COL_STATS_VERSION,.ncols = b->ncols,.ngranules = ngranules};
	uint8	   *recs,
			   *nulls;

	for (int c = 0; c < b->ncols; c++)
		has_nulls |= b->col[c].nulls > 0;
	/* STOR[STOR-COL-09]: the per-column null counts are written only when a column has a null (8*ncols B saved) */
	hdr.flags = has_nulls ? SIFT_COLSTATS_HAS_NULLS : 0;
	size = sift_col_stats_size(b->ncols, has_nulls);
	out = palloc0(size);
	recs = out + sizeof(hdr);
	nulls = recs + (size_t) b->ncols * sizeof(SiftPartColStat);

	memcpy(out, &hdr, sizeof(hdr));
	for (int c = 0; c < b->ncols; c++)
	{
		const ColAcc *a = &b->col[c];
		uint64		nonnull = a->rows - a->nulls,
					ndv = a->exact ? a->exact_ndv : sift_ndv_estimate(&a->ndv);
		SiftPartColStat st = {.total_bytes = a->bytes};
		int			best = 0;

		for (int i = 1; i < a->nenc; i++)
			if (a->enc[i].bytes > a->enc[best].bytes)
				best = i;
		st.dominant_enc = a->nenc > 0 ? a->enc[best].enc : 0;
		st.est_ndv = (uint32) Min(Max(ndv, nonnull > 0 ? 1 : 0), Min(nonnull, PG_UINT32_MAX));
		st.flags = (a->rows > 0 && a->nulls == a->rows ? SIFT_COLSTAT_ALL_NULL : 0) | (a->exact ? SIFT_COLSTAT_NDV_EXACT : 0);
		memcpy(recs + (size_t) c * sizeof(st), &st, sizeof(st));
		if (has_nulls)
			memcpy(nulls + (size_t) c * sizeof(uint64), &a->nulls, sizeof(uint64));
	}
	*len = size;
	pfree(b->col);
	pfree(b);
	return out;
}

static bool
open_any(const void *blob, uint64 len, uint16 ncols, SiftColStats *cs)
{
	SiftColStatsHdr hdr;

	if (len < sizeof(hdr))
		return false;
	memcpy(&hdr, blob, sizeof(hdr));
	return sift_col_stats_open(blob, len, ncols, hdr.ngranules, cs) == NULL;
}

void *
sift_col_stats_merge(const void *const *blobs, const uint64 *lens, int n, uint64 *len)
{
	SiftColStatsHdr first;
	uint16		ncols;
	uint32		ng = 0;
	uint8	   *out;
	uint64		size;

	if (n == 0 || lens[0] < sizeof(first))
		return NULL;
	memcpy(&first, blobs[0], sizeof(first));
	ncols = first.ncols;
	size = sift_col_stats_size(ncols, true);
	out = palloc0(size);
	for (int i = 0; i < n; i++)
	{
		SiftColStats cs;

		if (!open_any(blobs[i], lens[i], ncols, &cs))
		{
			pfree(out);
			return NULL;
		}
		ng += cs.hdr->ngranules;
		for (int c = 0; c < ncols; c++)
		{
			SiftPartColStat st = sift_col_stats_get(&cs, c),
						acc;
			uint64		nulls = sift_col_stats_nulls(&cs, c);
			uint8	   *rec = out + sizeof(SiftColStatsHdr) + (size_t) c * sizeof(SiftPartColStat);
			uint8	   *nl = out + sizeof(SiftColStatsHdr) + (size_t) ncols * sizeof(SiftPartColStat) + (size_t) c * sizeof(uint64);
			uint64		acc_nulls;

			memcpy(&acc, rec, sizeof(acc));
			memcpy(&acc_nulls, nl, sizeof(acc_nulls));
			if (i == 0 || st.total_bytes > acc.total_bytes)
				acc.dominant_enc = st.dominant_enc;
			acc.flags = i == 0 ? (st.flags & SIFT_COLSTAT_ALL_NULL) : (acc.flags & st.flags & SIFT_COLSTAT_ALL_NULL);
			acc.total_bytes += st.total_bytes;
			acc.est_ndv = Max(acc.est_ndv, st.est_ndv);
			acc_nulls += nulls;
			memcpy(rec, &acc, sizeof(acc));
			memcpy(nl, &acc_nulls, sizeof(acc_nulls));
		}
	}
	{
		SiftColStatsHdr hdr = {.magic = SIFT_COL_STATS_MAGIC,.version = SIFT_COL_STATS_VERSION,.ncols = ncols,.ngranules = ng,.flags = SIFT_COLSTATS_HAS_NULLS};

		memcpy(out, &hdr, sizeof(hdr));
	}
	*len = size;
	return out;
}

/* ---- the priors ---- */

static double
clamp_pass(double p)
{
	return Max(0.001, Min(0.99, p));
}

double
sift_col_stats_pass_prior(const SiftPartColStat *st, uint64 nrows, uint64 nnull, SiftPredShape shape, int nvals)
{
	double		null_frac = nrows > 0 ? (double) nnull / nrows : 0.0,
				ndv = Max(1.0, (double) st->est_ndv);

	if (st->flags & SIFT_COLSTAT_ALL_NULL)
		return shape == SIFT_PS_ISNULL ? 0.99 : 0.001;
	switch (shape)
	{
		case SIFT_PS_EQ:
			return clamp_pass(Max(nvals, 1) / ndv * (1.0 - null_frac));
		case SIFT_PS_RANGE:
			return clamp_pass((1.0 / 3.0) * (1.0 - null_frac));
		case SIFT_PS_ISNULL:
			return clamp_pass(null_frac);
		case SIFT_PS_NOTNULL:
			return clamp_pass(1.0 - null_frac);
		case SIFT_PS_SUBSTRING:
			return clamp_pass(0.1 * (1.0 - null_frac));
		default:
			return 0.5;
	}
}

double
sift_col_stats_bytes_per_row(const SiftPartColStat *st, uint64 nrows)
{
	return nrows > 0 ? (double) st->total_bytes / nrows : 0.0;
}
