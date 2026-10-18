/*
 * bench.c - ratios and speeds of the chunk codec on the generated e-commerce columns (gen.h): bytes per row of every policy, and per SIMD
 * level the ns per value of opening (unwrapping), decoding, filtering and random access. Usage: bench [-n orders] [-c chunk rows] [-v]
 * (-v lists every column under the auto policy). CPU time, run under flock /tmp/sift-perf.lock (make bench does).
 */
#include "postgres.h"

#include <time.h>

#include "gen.h"

#include "../../src/sift_colcodec_int.h"
#include "harness.h"
#include "ccshim.h"

static u32	g_chunk = 16384;

static double
cpu_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

typedef struct Blob
{
	uint8	   *p;
	Size		len;
	uint32		n;
	SiftCc	   *cc;
} Blob;

typedef struct Set
{
	Blob	   *b;
	int			nb;
	uint64		rows;
	uint64		bytes[4];		/* by Kind */
	uint64		strbytes;
	double		enc_ns;
} Set;

static int64
key_at(const Col *c, u32 i)
{
	switch (c->kind)
	{
		case K_F32:
			return sift_col_f8_key((double) ((float *) c->d)[i]);
		case K_F64:
			return sift_col_f8_key(((double *) c->d)[i]);
		default:
			switch (c->width)
			{
				case 1:
					return c->sgn ? (int64) ((int8_t *) c->d)[i] : (int64) ((u8 *) c->d)[i];
				case 2:
					return c->sgn ? (int64) ((int16_t *) c->d)[i] : (int64) ((u16 *) c->d)[i];
				case 4:
					return c->sgn ? (int64) ((i32 *) c->d)[i] : (int64) ((u32 *) c->d)[i];
				default:
					return ((i64 *) c->d)[i];
			}
	}
}

static void
add_blob(Set *s, const Col *c, u32 i0, u32 n, const SiftCcPolicy *pol, bool verbose)
{
	int64	   *keys = malloc(sizeof(int64) * n);
	uint64	   *nulls = NULL;
	StringInfoData out;
	SiftCcInfo info;
	double		t0;
	Blob	   *b;

	for (u32 i = 0; i < n; i++)
		keys[i] = key_at(c, i0 + i);
	if (c->nul)
	{
		nulls = calloc((n + 63) / 64, 8);
		for (u32 i = 0; i < n; i++)
			if (c->nul[i0 + i])
				nulls[i / 64] |= UINT64CONST(1) << (i % 64);
	}
	initStringInfo(&out);
	t0 = cpu_ns();
	sift_cc_encode(&out, c->kind == K_F32 ? SIFT_CC_F4 : c->kind == K_F64 ? SIFT_CC_F8 : SIFT_CC_INT, keys, n, nulls, pol, &info);
	s->enc_ns += cpu_ns() - t0;
	b = &s->b[s->nb++];
	b->p = aligned_alloc(64, (out.len + 63) & ~(size_t) 63);
	memcpy(b->p, out.data, out.len);
	b->len = out.len;
	b->n = n;
	s->rows += n;
	s->bytes[c->kind] += out.len;
	pfree(out.data);
	free(keys);
	free(nulls);
}

static void
string_bytes(Set *s, const Col *c, u32 i0, u32 n, const SiftCcPolicy *pol)
{
	StringInfoData out;
	uint32	   *off = malloc(sizeof(uint32) * (n + 1));

	for (u32 i = 0; i <= n; i++)
		off[i] = c->off[i0 + i] - c->off[i0];
	initStringInfo(&out);
	sift_cc_encode_strs(&out, off, (const char *) c->d + c->off[i0], n, pol);
	s->strbytes += out.len;
	pfree(out.data);
	free(off);
}

static void
build(Set *s, const Table *t, const char *spec, bool verbose)
{
	SiftCcPolicy pol;

	memset(s, 0, sizeof(*s));
	sift_cc_policy_parse(spec, &pol);
	s->b = calloc(t->ncol * (t->n / g_chunk + 2), sizeof(Blob));
	for (int ci = 0; ci < t->ncol; ci++)
	{
		const Col  *c = &t->col[ci];
		uint64		before = 0,
					rawb = 0;
		int			nb0 = s->nb;

		for (u32 i0 = 0; i0 < c->n; i0 += g_chunk)
		{
			u32			n = c->n - i0 < g_chunk ? c->n - i0 : g_chunk;

			if (c->kind == K_STR)
				string_bytes(s, c, i0, n, &pol);
			else
				add_blob(s, c, i0, n, &pol, verbose);
		}
		if (verbose && c->kind != K_STR)
		{
			for (int b = nb0; b < s->nb; b++)
				before += s->b[b].len;
			rawb = (uint64) c->n * c->width;
			printf("  %-16s raw %6.2f B/row  %-8s %6.2f B/row  (%5.1fx)\n", c->name, (double) rawb / c->n, spec, (double) before / c->n,
				   (double) rawb / before);
		}
	}
}

static void
ratio_table(const Table *t, bool verbose)
{
	static const char *const specs[] = {"none", "for", "auto", "lz4", "zstd", "zstd:9"};
	uint64		raw = 0,
				rawstr = 0;

	for (int ci = 0; ci < t->ncol; ci++)
		if (t->col[ci].kind == K_STR)
			rawstr += t->col[ci].off[t->col[ci].n];
		else
			raw += (uint64) t->col[ci].n * t->col[ci].width;
	printf("\n%s: %u rows, %u-row chunks, numeric raw %.2f B/row, string bytes %.2f B/row (plain, offsets excluded)\n", t->name, t->n, g_chunk,
		   (double) raw / t->n, (double) rawstr / t->n);
	printf("  %-8s %8s %8s %8s %8s %8s %8s %9s\n", "policy", "int", "f32", "f64", "numeric", "ratio", "strings", "enc ns/v");
	for (size_t p = 0; p < lengthof(specs); p++)
	{
		Set			s;
		uint64		num;

		build(&s, t, specs[p], false);
		num = s.bytes[K_INT] + s.bytes[K_F32] + s.bytes[K_F64];
		printf("  %-8s %8.2f %8.2f %8.2f %8.2f %7.2fx %8.2f %9.2f\n", specs[p], (double) s.bytes[K_INT] / t->n, (double) s.bytes[K_F32] / t->n,
			   (double) s.bytes[K_F64] / t->n, (double) num / t->n, (double) raw / num, (double) s.strbytes / t->n, s.enc_ns / (double) s.rows);
		for (int i = 0; i < s.nb; i++)
			free(s.b[i].p);
		free(s.b);
	}
	if (verbose)
	{
		Set			s;

		build(&s, t, "auto", true);
	}
}

/* ---------- speeds ---------- */

/* ns per value of fn over all blobs: the best of three runs of at least 50 ms */
static double
timed(Set *s, void (*fn) (Blob *, void *), void *arg)
{
	double		best = 1e18;

	for (int rep = 0; rep < 3; rep++)
	{
		double		t0 = cpu_ns();
		uint64		vals = 0;

		do
		{
			for (int i = 0; i < s->nb; i++)
				fn(&s->b[i], arg), vals += s->b[i].n;
		} while (cpu_ns() - t0 < 5e7);
		best = Min(best, (cpu_ns() - t0) / (double) vals);
	}
	return best;
}

static int64 *g_dec;
static uint64 *g_bits;
static uint32 g_sink;

static void
op_open(Blob *b, void *arg)
{
	SiftCc	   *cc = sift_cc_open(CurrentMemoryContext, b->p, b->len, false);

	sift_cc_close(cc);
	pgshim_reset();
}

static void
op_decode(Blob *b, void *arg)
{
	sift_cc_decode(b->cc, g_dec);
	g_sink += (uint32) g_dec[b->n / 2];
}

static void
op_filter(Blob *b, void *arg)
{
	int64		lo = sift_cc_min(b->cc),
				span = sift_cc_max(b->cc) - lo;

	sift_cc_filter(b->cc, lo + span / 20 * 9, lo + span / 20 * 11, g_bits);
	g_sink += (uint32) g_bits[0];
}

static void
op_get(Blob *b, void *arg)
{
	uint32		x = (uint32) (uintptr_t) b->p;

	for (int k = 0; k < 64; k++)
	{
		x = x * 1664525 + 1013904223;
		g_sink += (uint32) sift_cc_get(b->cc, (x >> 8) % b->n);
	}
}

static void
speeds(const Table *t, const char *spec, const SiftSimdLevel lv[3], int nl)
{
	Set			s;
	double		open_ns;

	build(&s, t, spec, false);
	g_dec = malloc(sizeof(int64) * (g_chunk + 16));
	g_bits = malloc(8 * SIFT_COL_NWORDS(g_chunk));
	open_ns = timed(&s, op_open, NULL);
	for (int i = 0; i < s.nb; i++)
		s.b[i].cc = sift_cc_open(CurrentMemoryContext, s.b[i].p, s.b[i].len, false);
	for (int l = 0; l < nl; l++)
	{
		cc_use_level(lv[l]);
		printf("  %-8s %-7s open %7.3f  decode %6.3f  filter %6.3f  ns/value  get %6.1f ns\n", spec, kt_level_name[lv[l]], open_ns, timed(&s, op_decode, NULL),
			   timed(&s, op_filter, NULL), timed(&s, op_get, NULL) * (double) s.rows / s.nb / 64);
	}
	for (int i = 0; i < s.nb; i++)
	{
		sift_cc_close(s.b[i].cc);
		free(s.b[i].p);
	}
	pgshim_reset();
	free(s.b);
	free(g_dec);
	free(g_bits);
}

int
main(int argc, char **argv)
{
	u32			norders = 400000;
	bool		verbose = false;
	Table		ot = {"orders"},
				it = {"items"};
	SiftSimdLevel lv[3];
	int			nl;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-n") && i + 1 < argc)
			norders = (u32) atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			g_chunk = (u32) atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v"))
			verbose = true;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	cc_init();
	nl = cc_levels(lv);
	gen_tables(&ot, &it, norders, 0);
	ratio_table(&ot, verbose);
	ratio_table(&it, verbose);
	printf("\nspeeds on the items table (CPU time; open = unwrap + validate, decode/filter of the opened chunk, filter selects 10%% of the range)\n");
	{
		static const char *const specs[] = {"none", "auto", "lz4", "zstd"};

		for (size_t p = 0; p < lengthof(specs); p++)
			speeds(&it, specs[p], lv, nl);
	}
	printf("sink %u\n", g_sink);
	return 0;
}
