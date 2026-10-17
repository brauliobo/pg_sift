/*
 * mdbq.c - one-connection statement runner for the tests and the benchmark (libmariadb).
 *
 *   mdbq SOCKET DB print FILE        runs the statements of FILE (one per line) and prints the rows, tab separated
 *   mdbq SOCKET DB time FILE [REPS]  runs them REPS times (default 2); one line per pass:
 *       pass N statements S completed C timeout T median_us M median_lb_us L qps Q
 *     median_us: median over the completed statements ("-" if none); median_lb_us: median with every timed out statement
 *     counted as the limit (a lower bound); qps: S over the wall clock of the pass.
 *
 * Every statement is fully fetched (mysql_store_result). Time mode runs under max_statement_time = $QUERY_TIMEOUT seconds
 * (default 10, 0 = no limit): the server aborts a longer statement (error 1969, or 188 from an InnoDB FULLTEXT search),
 * which is counted as a timeout. Any other error is reported on stderr and ends the run (in print mode it is printed as a
 * line "error TAB message").
 * Times are wall clock around mysql_real_query + fetch.
 */
#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STATEMENT_TIMEOUT 1969	/* ER_STATEMENT_TIMEOUT */
#define ABORTED_BY_USER 188		/* HA_ERR_ABORTED_BY_USER: InnoDB FULLTEXT search killed by the limit */

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

/* median of a[0..n) (sorted in place); negative when empty */
static double
median(double *a, size_t n)
{
	qsort(a, n, sizeof(double), cmp_d);
	return n ? a[n / 2] : -1;
}

static void
print_median(const char *name, double m)
{
	if (m < 0)
		printf(" %s -", name);
	else
		printf(" %s %.1f", name, m);
}

/* 0 done, 1 timed out (time mode only), -1 failed */
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
		else if (mysql_errno(c) == STATEMENT_TIMEOUT || mysql_errno(c) == ABORTED_BY_USER)
			return 1;
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

static MYSQL *
connect_to(const char *sock, const char *db, int print, double timeout)
{
	MYSQL	   *c = mysql_init(NULL);
	char		set[64];

	if (!mysql_real_connect(c, NULL, "root", NULL, db, 0, sock, CLIENT_MULTI_RESULTS))
	{
		fprintf(stderr, "connect: %s\n", mysql_error(c));
		exit(1);
	}
	mysql_set_character_set(c, "utf8mb4");
	snprintf(set, sizeof(set), "SET max_statement_time = %.6f", timeout);
	if (!print && mysql_query(c, set))
	{
		fprintf(stderr, "%s: %s\n", set, mysql_error(c));
		exit(1);
	}
	return c;
}

static size_t
read_statements(const char *path, char ***stmts, size_t **lens)
{
	FILE	   *in = fopen(path, "r");
	char	   *line = NULL;
	size_t		cap = 0,
				n = 0;

	if (in == NULL)
	{
		perror(path);
		exit(1);
	}
	for (ssize_t len; (len = getline(&line, &cap, in)) > 0;)
	{
		if (line[len - 1] == '\n')
			len--;
		if (len == 0)
			continue;
		*stmts = realloc(*stmts, (n + 1) * sizeof(char *));
		*lens = realloc(*lens, (n + 1) * sizeof(size_t));
		(*stmts)[n] = strndup(line, len);
		(*lens)[n++] = len;
	}
	fclose(in);
	return n;
}

/* one pass over the statements; prints the statistics line */
static void
timed_pass(MYSQL *c, char **stmts, size_t *lens, size_t n, int pass, double limit_us, double *done, double *bound)
{
	double		wall = now_us();
	size_t		completed = 0;

	for (size_t i = 0; i < n; i++)
	{
		double		t0 = now_us();
		int			rc = run_one(c, stmts[i], lens[i], 0);

		if (rc < 0)
			exit(1);
		bound[i] = rc ? limit_us : now_us() - t0;
		if (!rc)
			done[completed++] = bound[i];
	}
	wall = now_us() - wall;
	printf("pass %d statements %zu completed %zu timeout %zu", pass, n, completed, n - completed);
	print_median("median_us", median(done, completed));
	print_median("median_lb_us", median(bound, n));
	printf(" qps %.1f\n", n * 1e6 / wall);
}

int
main(int argc, char **argv)
{
	MYSQL	   *c;
	char	  **stmts = NULL;
	size_t	   *lens = NULL,
				n;
	int			print,
				reps = argc > 5 ? atoi(argv[5]) : 2;
	double		timeout = getenv("QUERY_TIMEOUT") ? atof(getenv("QUERY_TIMEOUT")) : 10,
			   *done,
			   *bound;

	if (argc < 5)
	{
		fprintf(stderr, "usage: mdbq SOCKET DB print|time FILE [REPS]   (time: QUERY_TIMEOUT seconds per statement, default 10)\n");
		return 2;
	}
	print = strcmp(argv[3], "print") == 0;
	c = connect_to(argv[1], argv[2], print, timeout);
	n = read_statements(argv[4], &stmts, &lens);
	if (print)
	{
		for (size_t i = 0; i < n; i++)
			run_one(c, stmts[i], lens[i], 1);
	}
	else
	{
		done = malloc(n * sizeof(double));
		bound = malloc(n * sizeof(double));
		for (int rep = 1; rep <= reps; rep++)
			timed_pass(c, stmts, lens, n, rep, timeout * 1e6, done, bound);
	}
	mysql_close(c);
	return 0;
}
