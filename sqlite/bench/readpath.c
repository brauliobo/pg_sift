/*
 * readpath.c - what it costs to get the bytes of the chunk rows out of SQLite, three ways, over every row of <t>_chunks:
 *   blob   sqlite3_blob_open/reopen + sqlite3_blob_read into an aligned buffer (what the extension does: one copy)
 *   column a prepared SELECT of the row, sqlite3_column_blob + memcpy into an aligned buffer (SQLite assembles an
 *          overflow chain into its own buffer first, then we copy: two copies when the row overflows)
 *   point  the same SELECT and no memcpy: the pointer SQLite returns, valid until the statement moves on (the most a
 *          zero-copy design could save; it cannot keep pages cached, as the pointer dies with the statement)
 * With PRAGMA mmap_size the page reads themselves are served from the mapping. Every pass reads the whole table twice
 * (first pass warms the OS cache) and reports the second, MB/s of payload, single thread.
 *
 *   cc -O2 -o readpath bench/readpath.c -lsqlite3 && ./readpath db table_chunks [mmap_mb]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static long long
by_blob(sqlite3 *db, const char *tbl, long long nrows, char *buf)
{
	sqlite3_blob *b = NULL;
	long long total = 0;

	for (long long id = 1; id <= nrows; id++)
	{
		int			rc = b ? sqlite3_blob_reopen(b, id) : sqlite3_blob_open(db, "main", tbl, "data", id, 0, &b);
		int			n;

		if (rc != SQLITE_OK)
			continue;
		n = sqlite3_blob_bytes(b);
		sqlite3_blob_read(b, buf, n, 0);
		total += n;
	}
	sqlite3_blob_close(b);
	return total;
}

static long long
by_column(sqlite3 *db, const char *tbl, long long nrows, char *buf, int copy)
{
	sqlite3_stmt *st;
	char		sql[200];
	long long total = 0;
	volatile char sink = 0;

	snprintf(sql, sizeof(sql), "SELECT data FROM %s WHERE id = ?1", tbl);
	sqlite3_prepare_v2(db, sql, -1, &st, NULL);
	for (long long id = 1; id <= nrows; id++)
	{
		sqlite3_bind_int64(st, 1, id);
		if (sqlite3_step(st) == SQLITE_ROW)
		{
			int			n = sqlite3_column_bytes(st, 0);
			const char *p = sqlite3_column_blob(st, 0);

			if (copy)
				memcpy(buf, p, n);
			else
				sink += p[0] + p[n - 1];
			total += n;
		}
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	return total;
}

int
main(int argc, char **argv)
{
	sqlite3    *db;
	sqlite3_stmt *st;
	char	   *buf = aligned_alloc(64, 1 << 20);
	long long nrows = 0;
	char		sql[200];
	static const char *names[] = {"blob", "column+memcpy", "pointer only"};

	if (argc < 3 || sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		return 2;
	if (argc > 3)
	{
		snprintf(sql, sizeof(sql), "PRAGMA mmap_size = %lld", atoll(argv[3]) << 20);
		sqlite3_exec(db, sql, NULL, NULL, NULL);
	}
	snprintf(sql, sizeof(sql), "SELECT max(id) FROM %s", argv[2]);
	sqlite3_prepare_v2(db, sql, -1, &st, NULL);
	if (sqlite3_step(st) == SQLITE_ROW)
		nrows = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	for (int m = 0; m < 3; m++)
	{
		long long total = 0;
		double		t = 0;

		for (int pass = 0; pass < 2; pass++)
		{
			double		t0 = now();

			total = m == 0 ? by_blob(db, argv[2], nrows, buf) : by_column(db, argv[2], nrows, buf, m == 1);
			t = now() - t0;
		}
		printf("%-14s %8.1f MB/s  (%lld rows, %.1f MB, %.3f s)\n", names[m], total / 1e6 / t, nrows, total / 1e6, t);
	}
	return 0;
}
