/*
 * mdbq.c - one-connection statement runner for the tests and the benchmark (libmariadb).
 *
 *   mdbq SOCKET DB print FILE        runs the statements of FILE (one per line) and prints the rows, tab separated
 *   mdbq SOCKET DB time FILE [REPS]  runs them REPS times (default 2) and reports the last pass: median, mean, p95, qps
 *
 * Every statement is fully fetched (mysql_store_result); a failing statement is reported on stderr and, in print mode,
 * as a line "error TAB message". Times are wall clock around mysql_real_query + fetch.
 */
#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int
cmp_d(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return (x > y) - (x < y);
}

static int
run_one(MYSQL *c, const char *sql, size_t len, int print)
{
	MYSQL_RES  *res;
	MYSQL_ROW	row;
	unsigned	n;

	if (mysql_real_query(c, sql, len) || ((res = mysql_store_result(c)) == NULL && mysql_errno(c)))
	{
		if (print)
			printf("error\t%s\n", mysql_error(c));
		else
			fprintf(stderr, "error: %s\n  %.200s\n", mysql_error(c), sql);
		return -1;
	}
	if (res == NULL)
		return 0;
	n = mysql_num_fields(res);
	while ((row = mysql_fetch_row(res)) != NULL)
	{
		if (print)
		{
			unsigned long *l = mysql_fetch_lengths(res);

			for (unsigned i = 0; i < n; i++)
			{
				if (i)
					putchar('\t');
				if (row[i])
					fwrite(row[i], 1, l[i], stdout);
				else
					fputs("NULL", stdout);
			}
			putchar('\n');
		}
	}
	mysql_free_result(res);
	return 0;
}

int
main(int argc, char **argv)
{
	MYSQL	   *c;
	FILE	   *in;
	char	   *line = NULL;
	size_t		cap = 0;
	char	  **stmts = NULL;
	size_t	   *lens = NULL,
				n = 0;
	int			print,
				reps = argc > 5 ? atoi(argv[5]) : 2;
	double	   *t;

	if (argc < 5)
	{
		fprintf(stderr, "usage: mdbq SOCKET DB print|time FILE [REPS]\n");
		return 2;
	}
	print = strcmp(argv[3], "print") == 0;
	c = mysql_init(NULL);
	if (!mysql_real_connect(c, NULL, "root", NULL, argv[2], 0, argv[1], CLIENT_MULTI_RESULTS))
	{
		fprintf(stderr, "connect: %s\n", mysql_error(c));
		return 1;
	}
	mysql_set_character_set(c, "utf8mb4");
	if ((in = fopen(argv[4], "r")) == NULL)
	{
		perror(argv[4]);
		return 1;
	}
	for (ssize_t len; (len = getline(&line, &cap, in)) > 0;)
	{
		if (line[len - 1] == '\n')
			len--;
		if (len == 0)
			continue;
		stmts = realloc(stmts, (n + 1) * sizeof(char *));
		lens = realloc(lens, (n + 1) * sizeof(size_t));
		stmts[n] = strndup(line, len);
		lens[n++] = len;
	}
	fclose(in);
	t = malloc(n * sizeof(double));
	for (int rep = 0; rep < (print ? 1 : reps); rep++)
	{
		double		wall = now_us(),
					sum = 0;
		size_t		failed = 0;

		for (size_t i = 0; i < n; i++)
		{
			double		t0 = now_us();

			failed += run_one(c, stmts[i], lens[i], print) != 0;
			t[i] = now_us() - t0;
			sum += t[i];
		}
		wall = now_us() - wall;
		if (!print && rep == reps - 1)
		{
			qsort(t, n, sizeof(double), cmp_d);
			printf("statements %zu failed %zu  median %.1f us  mean %.1f us  p95 %.1f us  qps %.0f\n", n, failed, t[n / 2],
				   sum / n, t[n * 95 / 100], n * 1e6 / wall);
		}
	}
	mysql_close(c);
	return 0;
}
