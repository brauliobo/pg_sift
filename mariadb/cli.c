/*
 * cli.c - sift_cli: the engine of engine.c without a server, for development, the correctness harness and the
 * microbenchmarks.
 *
 *   sift_cli BASE create NAME [DPP]
 *   sift_cli BASE load NAME FILE         FILE: id TAB text (COPY escapes); every row is an insert with seq = pk = id
 *   sift_cli BASE feed NAME FILE         FILE: seq TAB op TAB pk TAB text (a drain with updates and deletes)
 *   sift_cli BASE topk NAME K QUERY | count NAME QUERY | matches NAME QUERY | stats NAME | merge NAME | tokens NAME TEXT
 *   sift_cli BASE run NAME FILE K        FILE: id TAB query; one line per query: id TAB count TAB pk:score,...
 *   sift_cli BASE time NAME FILE K       FILE: id TAB query; prints the median and mean time of topk and of count (us)
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"

static char err[1024];

static void
die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, err);
	exit(1);
}

static double
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static SiftIdx *
open_or_die(const char *name)
{
	SiftIdx    *ix = sx_open(name, err, sizeof(err));

	if (!ix)
		die("open");
	return ix;
}

static size_t
unescape(char *s)
{
	char	   *w = s;

	for (char *r = s; *r && *r != '\n'; r++)
	{
		if (*r == '\\' && r[1])
		{
			r++;
			*r = *r == 'n' ? '\n' : *r == 'r' ? '\r' : *r == 't' ? '\t' : *r;
		}
		*w++ = *r;
	}
	*w = 0;
	return w - s;
}

static char *
field(char **line)
{
	char	   *s = *line,
			   *t = strchr(s, '\t');

	if (t)
		*t++ = 0;
	*line = t ? t : s + strlen(s);
	return s;
}

static void
load(const char *name, const char *file, bool events)
{
	SiftIdx    *ix = open_or_die(name);
	FILE	   *in = fopen(file, "r");
	char	   *line = NULL;
	size_t		cap = 0;
	int64_t		rows = 0,
				last = 0;
	SiftFeed   *f;

	if (!in)
	{
		perror(file);
		exit(1);
	}
	f = sx_feed_begin(ix, INT64_MAX, err, sizeof(err));
	if (!f)
		die("feed_begin");
	while (getline(&line, &cap, in) > 0)
	{
		char	   *p = line,
				   *a = field(&p);
		int64_t		seq = atoll(a),
					pk = seq;
		char		op = 'I';
		size_t		len;

		if (events)
		{
			op = *field(&p);
			pk = atoll(field(&p));
		}
		len = unescape(p);
		if (sx_feed_add(f, seq, op, pk, op == 'D' ? NULL : p, len, err, sizeof(err)) != 0)
			die("feed_add");
		last = seq;
	}
	if (sx_feed_end(f, &rows, err, sizeof(err)) != 0)
		die("feed_end");
	printf("fed %lld rows, up to %lld\n", (long long) rows, (long long) last);
}

static void
print_hits(const SxHit *h, int n)
{
	for (int i = 0; i < n; i++)
		printf("%s%lld:%.9g", i ? "," : "", (long long) h[i].pk, h[i].score);
}

static int
cmp_d(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return (x > y) - (x < y);
}

static void
run(const char *name, const char *file, int k, bool timing)
{
	SiftIdx    *ix = open_or_die(name);
	FILE	   *in = fopen(file, "r");
	char	   *line = NULL;
	size_t		cap = 0;
	SxHit	   *hits = malloc(sizeof(SxHit) * (k + 1));
	double	   *tt = malloc(sizeof(double) * 100000),
			   *tc = malloc(sizeof(double) * 100000);
	int			nq = 0;

	if (!in)
	{
		perror(file);
		exit(1);
	}
	while (getline(&line, &cap, in) > 0)
	{
		char	   *p = line,
				   *id = field(&p);
		size_t		len = unescape(p);
		int			n;
		int64_t		count;
		double		t0 = now_us(),
					t1,
					t2;

		if (sx_topk(ix, p, len, k, hits, &n, err, sizeof(err)) != 0)
		{
			if (!timing)
				printf("%s\terror\t%s\n", id, err);
			continue;
		}
		t1 = now_us();
		if (sx_count(ix, p, len, &count, err, sizeof(err)) != 0)
			die("count");
		t2 = now_us();
		if (timing)
		{
			tt[nq] = t1 - t0;
			tc[nq++] = t2 - t1;
			continue;
		}
		printf("%s\t%lld\t", id, (long long) count);
		print_hits(hits, n);
		printf("\n");
	}
	if (timing)
	{
		double		st = 0,
					sc = 0;

		for (int i = 0; i < nq; i++)
		{
			st += tt[i];
			sc += tc[i];
		}
		qsort(tt, nq, sizeof(double), cmp_d);
		qsort(tc, nq, sizeof(double), cmp_d);
		printf("queries %d  topk median %.1f us mean %.1f us p95 %.1f us  count median %.1f us mean %.1f us\n",
			   nq, tt[nq / 2], st / nq, tt[nq * 95 / 100], tc[nq / 2], sc / nq);
	}
}

int
main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 4)
	{
		fprintf(stderr, "usage: sift_cli BASE create|load|feed|topk|count|matches|stats|merge|tokens|run|time NAME ...\n");
		return 2;
	}
	sx_set_dirs(argv[1], getenv("SIFT_TMP") ? getenv("SIFT_TMP") : "/tmp");
	cmd = argv[2];
	if (!strcmp(cmd, "create"))
	{
		if (sx_create(argv[3], argc > 4 ? atoi(argv[4]) : 32, err, sizeof(err)) != 0)
			die("create");
	}
	else if (!strcmp(cmd, "load") || !strcmp(cmd, "feed"))
		load(argv[3], argv[4], !strcmp(cmd, "feed"));
	else if (!strcmp(cmd, "topk"))
	{
		int			k = atoi(argv[4]),
					n;
		SxHit	   *hits = malloc(sizeof(SxHit) * k);

		if (sx_topk(open_or_die(argv[3]), argv[5], strlen(argv[5]), k, hits, &n, err, sizeof(err)) != 0)
			die("topk");
		for (int i = 0; i < n; i++)
			printf("%lld\t%.6f\n", (long long) hits[i].pk, hits[i].score);
	}
	else if (!strcmp(cmd, "count"))
	{
		int64_t		c;

		if (sx_count(open_or_die(argv[3]), argv[4], strlen(argv[4]), &c, err, sizeof(err)) != 0)
			die("count");
		printf("%lld\n", (long long) c);
	}
	else if (!strcmp(cmd, "matches"))
	{
		int64_t    *pks,
					n;

		if (sx_matches(open_or_die(argv[3]), argv[4], strlen(argv[4]), &pks, &n, err, sizeof(err)) != 0)
			die("matches");
		for (int64_t i = 0; i < n; i++)
			printf("%lld\n", (long long) pks[i]);
	}
	else if (!strcmp(cmd, "stats"))
	{
		SiftIdxStats st;

		if (sx_stats(open_or_die(argv[3]), &st, err, sizeof(err)) != 0)
			die("stats");
		printf("segments %d docs %lld dead %lld live %lld drained %lld bytes %lld dpp %lld\n", st.nsegs,
			   (long long) st.ndocs, (long long) st.ndead, (long long) st.nlive, (long long) st.drained,
			   (long long) st.bytes, (long long) st.dpp);
	}
	else if (!strcmp(cmd, "merge"))
	{
		if (sx_merge(open_or_die(argv[3]), err, sizeof(err)) != 0)
			die("merge");
	}
	else if (!strcmp(cmd, "tokens"))
	{
		char	   *out;

		if (sx_tokens(open_or_die(argv[3]), argv[4], strlen(argv[4]), &out, err, sizeof(err)) != 0)
			die("tokens");
		printf("%s\n", out);
	}
	else if (!strcmp(cmd, "run") || !strcmp(cmd, "time"))
		run(argv[3], argv[4], atoi(argv[5]), !strcmp(cmd, "time"));
	else
	{
		fprintf(stderr, "unknown command %s\n", cmd);
		return 2;
	}
	return 0;
}
