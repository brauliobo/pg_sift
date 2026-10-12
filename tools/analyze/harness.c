/*
 * harness.c - standalone analyzer driver: stemmer validation, invariant
 * fuzzing, differential dumps and throughput benchmarks. See README.
 */
#include "postgres.h"

#include <time.h>

#include "mb/pg_wchar.h"

#include "sift_analyze.h"

extern int	stub_encoding;

static SiftAnalyzer *
analyzer(int mode, SiftStemmer stem)
{
	SiftAnalyzerConfig cfg;

	sift_analyzer_config_set(&cfg, mode & 1, mode & 2, stem);
	return sift_analyzer_create(&cfg, NULL);
}

static char *
slurp(const char *path, long *len)
{
	FILE	   *f = fopen(path, "rb");
	char	   *buf;

	if (!f)
	{
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	rewind(f);
	buf = malloc(*len + 1);
	if (fread(buf, 1, *len, f) != (size_t) *len)
		exit(1);
	buf[*len] = '\0';
	fclose(f);
	return buf;
}

/* stem VOC OUT: compares the English stemmer with Snowball's expected output */
static int
cmd_stem(const char *voc, const char *out)
{
	FILE	   *fv = fopen(voc, "r"),
			   *fo = fopen(out, "r");
	char		w[512],
				e[512];
	long		n = 0,
				bad = 0;

	if (!fv || !fo)
		return 1;
	while (fscanf(fv, "%511s", w) == 1 && fscanf(fo, "%511s", e) == 1)
	{
		int			len = sift_analyze_stem_en(w, strlen(w));

		n++;
		if (len != (int) strlen(e) || memcmp(w, e, len) != 0)
		{
			if (bad++ < 20)
				printf("mismatch: got %.*s want %s\n", len, w, e);
		}
	}
	printf("stem: %ld words, %ld mismatches\n", n, bad);
	return bad != 0;
}

/* dump: per input line of hex bytes, prints mode-specific tokens as hex */
static void
hex(const char *s, int n)
{
	for (int i = 0; i < n; i++)
		printf("%02x", (uint8) s[i]);
}

static int
cmd_dump(int mode, SiftStemmer stem)
{
	SiftAnalyzer *a = analyzer(mode, stem);
	static char line[1 << 21];
	static char bytes[1 << 20];

	while (fgets(line, sizeof(line), stdin))
	{
		int			n = 0,
					ntok;
		SiftToken  *t;

		for (char *p = line; p[0] && p[1] && p[0] != '\n'; p += 2)
			sscanf(p, "%2hhx", (unsigned char *) &bytes[n++]);
		ntok = sift_analyze_tokens(a, bytes, n, &t);
		for (int i = 0; i < ntok; i++)
		{
			printf(i ? " " : "");
			hex(t[i].str, t[i].len);
			printf(":%u:%u", t[i].start, t[i].end);
		}
		printf("\n");
	}
	return 0;
}

/* Fuzzing */

static uint64 rng_state;

static uint32
rnd(uint32 n)
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (uint32) (rng_state >> 33) % n;
}

static int
put_cp(char *p, uint32 cp)
{
	unsigned char *u = (unsigned char *) p;

	if (cp < 0x80)
		return u[0] = cp, 1;
	if (cp < 0x800)
		return u[0] = 0xC0 | cp >> 6, u[1] = 0x80 | (cp & 63), 2;
	if (cp < 0x10000)
		return u[0] = 0xE0 | cp >> 12, u[1] = 0x80 | ((cp >> 6) & 63), u[2] = 0x80 | (cp & 63), 3;
	return u[0] = 0xF0 | cp >> 18, u[1] = 0x80 | ((cp >> 12) & 63),
		u[2] = 0x80 | ((cp >> 6) & 63), u[3] = 0x80 | (cp & 63), 4;
}

/* Random text mixing every class the scanner distinguishes */
static int
gen_text(char *buf, int max)
{
	static const uint32 pool[][2] = {
		{'a', 'z'}, {'A', 'Z'}, {'0', '9'}, {' ', '/'}, {0xC0, 0x17F}, {0x370, 0x3FF},
		{0x400, 0x4FF}, {0x300, 0x36F}, {0x4E00, 0x4E40}, {0x3040, 0x30FF}, {0xAC00, 0xAC20},
		{0x0E00, 0x0E5B}, {0x0900, 0x097F}, {0x1F300, 0x1F64F}, {0x1F1E6, 0x1F1FF},
		{0x1F3FB, 0x1F3FF}, {0x200B, 0x200F}, {0xFE00, 0xFE0F}, {0xAD, 0xAD}, {0xFB00, 0xFB06},
		{0xFF01, 0xFF5E}, {0x1D400, 0x1D4FF}, {0x0590, 0x06FF}, {0x2190, 0x27BF}, {0, 0x10FFFF},
	};
	int			n = 0,
				target = rnd(max);

	while (n < target - 8)
	{
		uint32		k = rnd(lengthof(pool) + 3);

		if (k == lengthof(pool))
			buf[n++] = (char) (0x80 + rnd(0x80));	/* stray byte */
		else if (k == lengthof(pool) + 1)
		{
			int			run = rnd(300);	/* long run, crosses SIFT_MAX_TERM_BYTES */

			for (int i = 0; i < run && n < target - 8; i++)
				n += put_cp(buf + n, rnd(3) ? 'a' + rnd(26) : 0xE9);
		}
		else if (k == lengthof(pool) + 2)
			buf[n++] = ' ';
		else
		{
			uint32		cp = pool[k][0] + rnd(pool[k][1] - pool[k][0] + 1);

			if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))
				cp = 'x';
			n += put_cp(buf + n, cp);
		}
	}
	return n;
}

/* Whether a token analyzes to exactly itself */
static bool
self_analyzes(SiftAnalyzer *a, const char *tok, int n)
{
	char		copy[SIFT_MAX_TERM_BYTES];
	SiftToken  *r;

	memcpy(copy, tok, n);
	return sift_analyze_tokens(a, copy, n, &r) == 1 && r[0].len == n && memcmp(r[0].str, copy, n) == 0;
}

/*
 * invariant [VOC]: every token of every code point in several contexts, and
 * of every vocabulary word, analyzes to itself, in every configuration
 */
static int
cmd_invariant(const char *voc)
{
	static const uint32 ctx[][3] = {{0}, {0, 'a'}, {'a', 0}, {0, 0}, {0, 0x3099}, {0, 0x301}};
	long		checked = 0,
				bad = 0;

	for (int m = 0; m < 4; m++)
		for (int st = 0; st < 2; st++)
		{
			SiftAnalyzer *a = analyzer(m, st),
					   *b = analyzer(m, st);
			FILE	   *f = voc ? fopen(voc, "r") : NULL;
			char		in[512];

			for (uint32 cp = 1; cp < 0x110000; cp++)
			{
				if (cp >= 0xD800 && cp <= 0xDFFF)
					continue;
				for (int k = 0; k < lengthof(ctx); k++)
				{
					int			n = 0,
								ntok;
					SiftToken  *t;

					for (int j = 0; j < 3 && (j == 0 || ctx[k][j]); j++)
						n += put_cp(in + n, ctx[k][j] ? ctx[k][j] : cp);
					ntok = sift_analyze_tokens(a, in, n, &t);
					for (int i = 0; i < ntok; i++, checked++)
						if (!self_analyzes(b, t[i].str, t[i].len) && bad++ < 10)
							printf("mode %d stem %d U+%04X context %d: token %.*s\n", m, st, cp, k,
								   t[i].len, t[i].str);
				}
			}
			while (f && fscanf(f, "%511s", in) == 1)
				for (int v = 0; v < 2; v++)
				{
					SiftToken  *t;
					int			ntok;

					if (v)
						in[0] = pg_toupper((unsigned char) in[0]);
					ntok = sift_analyze_tokens(a, in, strlen(in), &t);
					for (int i = 0; i < ntok; i++, checked++)
						if (!self_analyzes(b, t[i].str, t[i].len) && bad++ < 10)
							printf("mode %d stem %d word %s: token %.*s\n", m, st, in, t[i].len,
								   t[i].str);
				}
			if (f)
				fclose(f);
		}
	printf("invariant: %ld tokens checked, %ld not analyzing to themselves\n", checked, bad);
	return bad != 0;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (seed %lu iter %ld)\n", \
	#c, seed, it); abort(); } } while (0)

static int
cmd_fuzz(long iters, unsigned long seed)
{
	enum
	{
	MAXLEN = 4096};
	char	   *buf = malloc(2 * MAXLEN + 1),
			   *copy = malloc(MAXLEN);
	SiftAnalyzer *an[2][4][2],
			   *a2[2][4][2];

	rng_state = seed;
	for (int e = 0; e < 2; e++)
	{
		stub_encoding = e ? PG_SQL_ASCII : PG_UTF8;
		for (int m = 0; m < 4; m++)
			for (int s = 0; s < 2; s++)
			{
				an[e][m][s] = analyzer(m, s);
				a2[e][m][s] = analyzer(m, s);
			}
	}
	for (long it = 0; it < iters; it++)
	{
		int			la = gen_text(buf, MAXLEN),
					lb,
					e = rnd(2),
					m = rnd(4),
					s = rnd(2);
		SiftAnalyzer *a = an[e][m][s],
				   *b = a2[e][m][s];
		SiftToken  *t,
				   *tb,
					tok;
		SiftTermFreq *tf;
		SiftTokenStream ts;
		int			ntok,
					nb,
					nt,
					total = 0;
		uint32		dl;
		char	   *f;

		/* the analyzer must not read past len: give it a private copy */
		memcpy(copy, buf, la);
		ntok = sift_analyze_tokens(a, copy, la, &t);
		for (int i = 0; i < ntok; i++)
		{
			CHECK(t[i].len > 0 && t[i].len <= SIFT_MAX_TERM_BYTES);
			CHECK(t[i].pos == (uint32) i);
			CHECK(t[i].start < t[i].end && t[i].end <= (uint32) la);
			CHECK(i == 0 || t[i].start >= t[i - 1].end);
			if (!e)
				CHECK(pg_encoding_verifymbstr(PG_UTF8, t[i].str, t[i].len) == t[i].len);
			CHECK(self_analyzes(b, t[i].str, t[i].len));
			if (!s)
			{
				/* tokens are already folded */
				int			fl = sift_analyze_fold(b, t[i].str, t[i].len, &f);

				CHECK(fl == t[i].len && memcmp(f, t[i].str, fl) == 0);
			}
		}

		/* streaming gives identical tokens */
		sift_analyze_begin(&ts, copy, la);
		for (int i = 0; i < ntok; i++)
		{
			CHECK(sift_analyze_next(b, &ts, &tok));
			CHECK(tok.len == t[i].len && memcmp(tok.str, t[i].str, tok.len) == 0);
			CHECK(tok.pos == t[i].pos && tok.start == t[i].start && tok.end == t[i].end);
		}
		CHECK(!sift_analyze_next(b, &ts, &tok));

		/* the bag is the multiset of tokens, sorted by (hash, bytes) */
		nt = sift_analyze_doc(b, copy, la, &tf, &dl);
		CHECK(dl == (uint32) Min(ntok, SIFT_MAX_DL));
		for (int i = 0; i < nt; i++)
		{
			int			cnt = 0;

			CHECK(tf[i].hash == sift_term_hash(tf[i].term, tf[i].len));
			CHECK(i == 0 || tf[i - 1].hash < tf[i].hash ||
				  (tf[i - 1].hash == tf[i].hash &&
				   (tf[i - 1].len != tf[i].len || memcmp(tf[i - 1].term, tf[i].term, tf[i].len))));
			for (int j = 0; j < ntok; j++)
				cnt += t[j].len == tf[i].len && memcmp(t[j].str, tf[i].term, tf[i].len) == 0;
			CHECK(cnt > 0 && tf[i].tf == Min(cnt, SIFT_MAX_TF));
			total += cnt;
		}
		CHECK(total == ntok);

		/* concatenation across a separator tokenizes each side independently */
		lb = gen_text(buf + la + 1, MAXLEN - 1);
		buf[la] = ' ';
		{
			SiftToken  *tc;
			int			nc = sift_analyze_tokens(b, buf, la + 1 + lb, &tc);

			ntok = sift_analyze_tokens(a, copy, la, &t);
			nb = 0;
			CHECK(nc >= ntok);
			for (int i = 0; i < ntok; i++)
				CHECK(tc[i].len == t[i].len && memcmp(tc[i].str, t[i].str, t[i].len) == 0 &&
					  tc[i].end == t[i].end);
			memcpy(copy, buf + la + 1, lb);
			nb = sift_analyze_tokens(a, copy, lb, &tb);
			CHECK(nc == ntok + nb);
			for (int i = 0; i < nb; i++)
				CHECK(tc[ntok + i].len == tb[i].len &&
					  memcmp(tc[ntok + i].str, tb[i].str, tb[i].len) == 0 &&
					  tc[ntok + i].start == tb[i].start + la + 1 && tc[ntok + i].pos == tb[i].pos + ntok);
		}
		if (it % 64 == 0)
			sift_analyzer_reset(a);
	}
	printf("fuzz: %ld iterations ok\n", iters);
	return 0;
}

/* Benchmark: one document per line (text after the first tab, if any) */

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int
cmd_bench(const char *path, int mode, SiftStemmer stem, int reps)
{
	long		len,
				ndocs = 0,
				ntok = 0;
	char	   *text = slurp(path, &len);
	int		   *starts = malloc(sizeof(int) * (len / 2 + 2)),
			   *lens = malloc(sizeof(int) * (len / 2 + 2));
	SiftAnalyzer *a = analyzer(mode, stem);
	double		best[3] = {1e9, 1e9, 1e9};

	for (char *p = text, *end = text + len; p < end;)
	{
		char	   *nl = memchr(p, '\n', end - p),
				   *tab;

		nl = nl ? nl : end;
		tab = memchr(p, '\t', nl - p);
		starts[ndocs] = (tab ? tab + 1 : p) - text;
		lens[ndocs++] = nl - (tab ? tab + 1 : p);
		p = nl + 1;
	}
	for (int r = 0; r < reps; r++)
	{
		double		t0 = now(),
					t1,
					t2;
		long		sink = 0;
		SiftToken  *t;
		SiftTermFreq *tf;
		uint32		dl;
		SiftTokenStream ts;
		SiftToken	tok;

		ntok = 0;
		for (long d = 0; d < ndocs; d++)
			ntok += sift_analyze_tokens(a, text + starts[d], lens[d], &t);
		t1 = now();
		for (long d = 0; d < ndocs; d++)
			sink += sift_analyze_doc(a, text + starts[d], lens[d], &tf, &dl);
		t2 = now();
		for (long d = 0; d < ndocs; d++)
			for (sift_analyze_begin(&ts, text + starts[d], lens[d]); sift_analyze_next(a, &ts, &tok);)
				sink += tok.len;
		best[0] = Min(best[0], t1 - t0);
		best[1] = Min(best[1], t2 - t1);
		best[2] = Min(best[2], now() - t2);
		if (sink == 42)
			printf(" ");
	}
	printf("%s mode=%d stem=%d: %.1f MB, %ld docs, %ld tokens\n", path, mode, stem, len / 1e6,
		   ndocs, ntok);
	printf("  tokens %.0f MB/s (%.2f ns/B)  doc %.0f MB/s (%.2f ns/B)  stream %.0f MB/s\n",
		   len / best[0] / 1e6, best[0] * 1e9 / len, len / best[1] / 1e6, best[1] * 1e9 / len,
		   len / best[2] / 1e6);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";

	if (strcmp(cmd, "stem") == 0 && argc == 4)
		return cmd_stem(argv[2], argv[3]);
	if (strcmp(cmd, "dump") == 0 && argc >= 3)
	{
		stub_encoding = argc > 4 && strcmp(argv[4], "sql_ascii") == 0 ? PG_SQL_ASCII : PG_UTF8;
		return cmd_dump(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 0);
	}
	if (strcmp(cmd, "invariant") == 0)
		return cmd_invariant(argc > 2 ? argv[2] : NULL);
	if (strcmp(cmd, "fuzz") == 0 && argc == 4)
		return cmd_fuzz(atol(argv[2]), strtoul(argv[3], NULL, 10));
	if (strcmp(cmd, "bench") == 0 && argc >= 3)
		return cmd_bench(argv[2], argc > 3 ? atoi(argv[3]) : 3, argc > 4 ? atoi(argv[4]) : 0,
						 argc > 5 ? atoi(argv[5]) : 3);
	if (strcmp(cmd, "hash") == 0 && argc == 3)
		return printf("%016llx\n", (unsigned long long) sift_term_hash64(argv[2], strlen(argv[2]))), 0;
	fprintf(stderr, "usage: harness stem VOC OUT | dump MODE [STEM [sql_ascii]] | fuzz N SEED |\n"
			"               bench FILE [MODE [STEM [REPS]]] | hash TERM | invariant [VOC]\n");
	return 2;
}
