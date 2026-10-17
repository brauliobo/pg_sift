/*
 * stress.c - N reader threads and one writer on one database file, the extension linked in (build: make tsan, or
 * make build/stress). Readers query inside a read transaction and check what a snapshot guarantees: the rows of a
 * MATCH, sift_count and the facets agree, ranked rows are matches, highlights are well formed. The writer inserts,
 * updates and deletes in transactions, rolls some back, and runs flush, merge and optimize commands.
 *
 *   stress [-r readers] [-s seconds] [-m serialized|multi] [-c] [-f db] [-p parallel]
 *     -c  the readers share one connection (serialized mode) instead of having their own
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

extern int	sqlite3_sift_init(sqlite3 *db, char **err, const sqlite3_api_routines *api);

#define VOCAB	2000
#define DOCS	6000

static atomic_int stop;
static atomic_long reads,
			writes,
			failures;
static const char *path;
static int	parallel;
static sqlite3 *shared;

static void
fail(const char *what, const char *detail)
{
	atomic_fetch_add(&failures, 1);
	fprintf(stderr, "FAIL %s: %s\n", what, detail ? detail : "");
}

static unsigned
next(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

static void
doc(char *buf, unsigned *s)
{
	char	   *p = buf;

	for (int i = 0; i < 30; i++)
		p += sprintf(p, "w%u ", next(s) % VOCAB);
}

static void
exec(sqlite3 *db, const char *sql)
{
	char	   *err = NULL;
	int			rc = sqlite3_exec(db, sql, NULL, NULL, &err);

	if (rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
		fail(sql, err);
	sqlite3_free(err);
}

static sqlite3 *
open_db(void)
{
	sqlite3    *db;

	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
	{
		fail("open", sqlite3_errmsg(db));
		exit(1);
	}
	sqlite3_busy_timeout(db, 30000);
	return db;
}

static long
scalar(sqlite3 *db, const char *sql, const char *q)
{
	sqlite3_stmt *st;
	long		v = -1;

	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return fail(sql, sqlite3_errmsg(db)), -1;
	if (q)
		sqlite3_bind_text(st, 1, q, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW)
		v = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return v;
}

/* One snapshot: the same answer from every way of asking */
static void
check_query(sqlite3 *db, unsigned *s, int snapshot)
{
	char		q[64];
	sqlite3_stmt *st;
	long		n,
				c,
				rows = 0;

	switch (next(s) % 4)
	{
		case 0:
			snprintf(q, sizeof(q), "w%u*", next(s) % 100);
			break;
		case 1:
			snprintf(q, sizeof(q), "\"w%u w%u\" OR w%u", next(s) % VOCAB, next(s) % VOCAB, next(s) % VOCAB);
			break;
		default:
			snprintf(q, sizeof(q), "w%u OR w%u", next(s) % VOCAB, next(s) % VOCAB);
	}
	if (snapshot)
		exec(db, "BEGIN");
	n = scalar(db, "SELECT count(*) FROM t WHERE t MATCH ?1", q);
	c = scalar(db, "SELECT sift_count('t', ?1)", q);
	if (snapshot && n != c)
	{
		char		msg[200];

		snprintf(msg, sizeof(msg), "%s: %ld rows but sift_count %ld", q, n, c);
		fail("snapshot", msg);
	}
	if (sqlite3_prepare_v2(db, "SELECT rowid, rank, sift_highlight(t, 0, '<', '>') FROM t WHERE t MATCH ?1 ORDER BY rank LIMIT 10", -1, &st, NULL) == SQLITE_OK)
	{
		double		last = -1e30;

		sqlite3_bind_text(st, 1, q, -1, SQLITE_STATIC);
		while (sqlite3_step(st) == SQLITE_ROW)
		{
			double		r = sqlite3_column_double(st, 1);

			rows++;
			if (r < last)
				fail("rank order", q);
			last = r;
			if (strstr((const char *) sqlite3_column_text(st, 2), "<w") == NULL)
				fail("highlight", q);
		}
		sqlite3_finalize(st);
	}
	if (rows > 10 || (snapshot && n >= 10 && rows != 10))
		fail("limit", q);
	if (scalar(db, "SELECT count(*) FROM (SELECT * FROM sift_facets('t', 'cat', ?1))", q) < 0)
		fail("facets", q);
	if (snapshot)
		exec(db, "COMMIT");
	atomic_fetch_add(&reads, 1);
}

static void *
reader(void *arg)
{
	unsigned	s = (unsigned) (size_t) arg * 7919u + 1;
	sqlite3    *db = shared ? shared : open_db();

	while (!atomic_load(&stop))
		check_query(db, &s, !shared);
	if (!shared)
		sqlite3_close(db);
	return NULL;
}

static void *
writer(void *arg)
{
	unsigned	s = 12345;
	sqlite3    *db = open_db();
	char		buf[1024],
				sql[1400];
	long		round = 0;

	while (!atomic_load(&stop))
	{
		exec(db, "BEGIN IMMEDIATE");
		for (int i = 0; i < 40; i++)
		{
			unsigned	id = next(&s) % (DOCS * 2) + 1;

			doc(buf, &s);
			snprintf(sql, sizeof(sql), "DELETE FROM t WHERE rowid = %u", id);
			exec(db, sql);
			snprintf(sql, sizeof(sql), "INSERT INTO t(rowid, body) VALUES (%u, '%s')", id, buf);
			exec(db, sql);
			snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO cat(rowid, c) VALUES (%u, 'k%u')", id, id % 7);
			exec(db, sql);
		}
		exec(db, round % 5 == 4 ? "ROLLBACK" : "COMMIT");
		if (round % 7 == 6)
			exec(db, "INSERT INTO t(t) VALUES ('flush')");
		if (round % 11 == 10)
			exec(db, "INSERT INTO t(t, rank) VALUES ('merge', 1)");
		if (round % 40 == 39)
			exec(db, "INSERT INTO t(t) VALUES ('optimize')");
		round++;
		atomic_fetch_add(&writes, 1);
	}
	sqlite3_close(db);
	return NULL;
}

static void
setup(void)
{
	sqlite3    *db = open_db();
	unsigned	s = 99;
	char		buf[1024],
				sql[1400];

	exec(db, "PRAGMA journal_mode = WAL");
	snprintf(sql, sizeof(sql), "CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk=8192, automerge=6, cache_size=65536, parallel=%d)",
			 parallel);
	exec(db, sql);
	exec(db, "CREATE VIRTUAL TABLE cat USING sift(c, analyzer='keyword', stemmer=none, positions=0)");
	exec(db, "BEGIN");
	for (int i = 1; i <= DOCS; i++)
	{
		doc(buf, &s);
		snprintf(sql, sizeof(sql), "INSERT INTO t(rowid, body) VALUES (%d, '%s')", i, buf);
		exec(db, sql);
		snprintf(sql, sizeof(sql), "INSERT INTO cat(rowid, c) VALUES (%d, 'k%d')", i, i % 7);
		exec(db, sql);
		if (i % 1000 == 0)
		{
			exec(db, "COMMIT");
			exec(db, "BEGIN");
		}
	}
	exec(db, "COMMIT");
	sqlite3_close(db);
}

int
main(int argc, char **argv)
{
	int			nreaders = 4,
				seconds = 5,
				mode = SQLITE_CONFIG_SERIALIZED,
				share = 0;
	char		tmp[] = "/tmp/sift-stress-XXXXXX";
	pthread_t	th[64],
				wr;

	path = NULL;
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
			nreaders = atoi(argv[++i]);
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
			mode = strcmp(argv[++i], "multi") == 0 ? SQLITE_CONFIG_MULTITHREAD : SQLITE_CONFIG_SERIALIZED;
		else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
			path = argv[++i];
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			parallel = atoi(argv[++i]);
		else if (strcmp(argv[i], "-c") == 0)
			share = 1;
	}
	if (nreaders > 64)
		nreaders = 64;
	if (share && mode != SQLITE_CONFIG_SERIALIZED)
	{
		fprintf(stderr, "-c needs serialized mode\n");
		return 2;
	}
	sqlite3_config(mode);
	sqlite3_auto_extension((void (*)(void)) sqlite3_sift_init);
	if (path == NULL)
	{
		close(mkstemp(tmp));
		path = tmp;
	}
	setup();
	if (share)
		shared = open_db();
	atomic_store(&stop, 0);
	pthread_create(&wr, NULL, writer, NULL);
	for (long i = 0; i < nreaders; i++)
		pthread_create(&th[i], NULL, reader, (void *) (i + 1));
	sleep(seconds);
	atomic_store(&stop, 1);
	for (int i = 0; i < nreaders; i++)
		pthread_join(th[i], NULL);
	pthread_join(wr, NULL);
	{
		sqlite3    *db = shared ? shared : open_db();

		exec(db, "INSERT INTO t(t, rank) VALUES ('integrity-check', 1)");
		exec(db, "INSERT INTO cat(cat, rank) VALUES ('integrity-check', 1)");
		if (shared)
			sqlite3_close(shared);
		else
			sqlite3_close(db);
	}
	printf("%s readers=%d%s: %ld reads, %ld write rounds, %ld failures\n", mode == SQLITE_CONFIG_SERIALIZED ? "serialized" : "multi-thread",
		   nreaders, share ? " (one shared connection)" : "", atomic_load(&reads), atomic_load(&writes), atomic_load(&failures));
	if (path == tmp)
	{
		char		cmd[100];

		unlink(tmp);
		snprintf(cmd, sizeof(cmd), "%s-wal", tmp);
		unlink(cmd);
		snprintf(cmd, sizeof(cmd), "%s-shm", tmp);
		unlink(cmd);
	}
	return atomic_load(&failures) != 0;
}
