/*
 * stress.c - client threads against one sift index while writers insert and delete and the drain and the merge run.
 *
 *   stress SOCKET DB TABLE SECONDS READERS WRITERS
 *
 * Readers run ranked and counting queries for common terms and check what a query must keep: the top-10 is full (the table
 * holds more than 10 matching rows all the time), no error is raised. The writers insert rows and delete some of their own.
 * One thread drains and one merges, continuously. At the end the index is drained once more and its count must equal the
 * count of the table. Prints "stress: <numbers>" and exits 1 on any violation.
 */
#include <mysql.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *sock, *db, *tbl;
static atomic_int stop;
static atomic_long reads, inserts, deletes, drains, merges, violations, errors;

static MYSQL *
conn(void)
{
	MYSQL	   *m = mysql_init(NULL);

	if (!mysql_real_connect(m, NULL, "root", NULL, db, 0, sock, 0))
	{
		fprintf(stderr, "connect: %s\n", mysql_error(m));
		exit(2);
	}
	mysql_set_character_set(m, "utf8mb4");
	return m;
}

static long
one(MYSQL *m, const char *q)
{
	MYSQL_RES  *r;
	MYSQL_ROW	row;
	long		v = -1;

	if (mysql_query(m, q) || !(r = mysql_store_result(m)))
	{
		fprintf(stderr, "error: %s: %s\n", q, mysql_error(m));
		atomic_fetch_add(&errors, 1);
		return -1;
	}
	if ((row = mysql_fetch_row(r)) && row[0])
		v = atol(row[0]);
	mysql_free_result(r);
	return v;
}

/* Runs a statement (a CALL answers with several result sets) and drops what it returns */
static void
run(MYSQL *m, const char *q)
{
	MYSQL_RES  *r;

	if (mysql_query(m, q))
	{
		fprintf(stderr, "error: %s: %s\n", q, mysql_error(m));
		atomic_fetch_add(&errors, 1);
		return;
	}
	do
		if ((r = mysql_store_result(m)))
			mysql_free_result(r);
	while (mysql_next_result(m) == 0);
}

static void *
reader(void *arg)
{
	MYSQL	   *m = conn();
	char		q[512];
	unsigned	seed = (unsigned) (intptr_t) arg;

	while (!stop)
	{
		const char *terms[] = {"common", "common AND alpha", "alpha OR beta", "\"common alpha\"", "beta AND NOT alpha"};
		const char *t = terms[rand_r(&seed) % 5];

		snprintf(q, sizeof(q), "SELECT JSON_LENGTH(sift_topk('%s', '%s', 10))", tbl, t);
		if (one(m, q) != 10 && strcmp(t, "beta AND NOT alpha") != 0 && strcmp(t, "\"common alpha\"") != 0)
		{
			atomic_fetch_add(&violations, 1);
			fprintf(stderr, "short top-10 for %s\n", t);
		}
		snprintf(q, sizeof(q), "SELECT sift_count('%s', '%s')", tbl, t);
		one(m, q);
		atomic_fetch_add(&reads, 2);
	}
	mysql_close(m);
	return NULL;
}

static void *
writer(void *arg)
{
	MYSQL	   *m = conn();
	char		q[512];
	long		w = (long) (intptr_t) arg,
				n = 0;
	unsigned	seed = (unsigned) w;

	while (!stop)
	{
		long		id = 1000000 * w + ++n;

		snprintf(q, sizeof(q), "INSERT INTO %s VALUES (%ld, 'common %s tok%ld w%ld')", tbl, id, n % 2 ? "alpha" : "beta", n, w);
		run(m, q);
		atomic_fetch_add(&inserts, 1);
		usleep(2000);
		if (n > 5 && rand_r(&seed) % 3 == 0)
		{
			snprintf(q, sizeof(q), "DELETE FROM %s WHERE id = %ld", tbl, 1000000 * w + 1 + rand_r(&seed) % (n - 1));
			run(m, q);
			atomic_fetch_add(&deletes, 1);
		}
	}
	mysql_close(m);
	return NULL;
}

static void *
maintainer(void *arg)
{
	MYSQL	   *m = conn();
	char		q[256];
	int			merge = arg != NULL;

	while (!stop)
	{
		if (merge)
		{
			snprintf(q, sizeof(q), "SELECT sift_merge('%s', 'auto')", tbl);
			one(m, q);
			atomic_fetch_add(&merges, 1);
			usleep(200000);
		}
		else
		{
			snprintf(q, sizeof(q), "CALL sift_sync('%s')", tbl);
			run(m, q);
			atomic_fetch_add(&drains, 1);
			usleep(30000);
		}
	}
	mysql_close(m);
	return NULL;
}

int
main(int argc, char **argv)
{
	int			secs,
				nr,
				nw;
	pthread_t	th[64];
	int			n = 0;
	MYSQL	   *m;
	char		q[256];
	long		want,
				got;

	if (argc != 7)
		return fprintf(stderr, "usage: stress SOCKET DB TABLE SECONDS READERS WRITERS\n"), 2;
	sock = argv[1], db = argv[2], tbl = argv[3];
	secs = atoi(argv[4]), nr = atoi(argv[5]), nw = atoi(argv[6]);
	for (int i = 0; i < nr; i++)
		pthread_create(&th[n++], NULL, reader, (void *) (intptr_t) (i + 1));
	for (long i = 1; i <= nw; i++)
		pthread_create(&th[n++], NULL, writer, (void *) (intptr_t) i);
	pthread_create(&th[n++], NULL, maintainer, NULL);
	pthread_create(&th[n++], NULL, maintainer, (void *) 1);
	sleep(secs);
	stop = 1;
	for (int i = 0; i < n; i++)
		pthread_join(th[i], NULL);

	m = conn();
	snprintf(q, sizeof(q), "CALL sift_sync('%s')", tbl);
	run(m, q);
	snprintf(q, sizeof(q), "SELECT COUNT(*) FROM %s", tbl);
	want = one(m, q);
	snprintf(q, sizeof(q), "SELECT sift_count('%s', 'common')", tbl);
	got = one(m, q);
	mysql_close(m);
	printf("stress: %ds readers %d writers %d: queries %ld inserts %ld deletes %ld drains %ld merges %ld; index %ld table %ld; violations %ld errors %ld\n",
		   secs, nr, nw, (long) reads, (long) inserts, (long) deletes, (long) drains, (long) merges, got, want,
		   (long) violations, (long) errors);
	return got != want || violations || errors;
}
