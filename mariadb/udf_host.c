/*
 * udf_host.c - the host of the engine for the UDFs (udf.c): the queue and the table of an index, read through a local
 * connection to the server.
 */
#include "udf_int.h"

extern struct sql_service_st sql_service_handler;

#define SQL(f)		sql_service_handler.mysql_##f##_func

/* ------------------------------------------------ the host: the queue, the table, through a local connection */

typedef struct Sql
{
	char	   *sql;
	size_t		len,
				cap;
} Sql;

static void
sql_put(Sql *s, const char *fmt,...)
{
	va_list		ap;
	int			n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (s->len + n + 1 > s->cap)
		s->sql = realloc(s->sql, s->cap = (s->len + n + 1) * 2);
	va_start(ap, fmt);
	vsnprintf(s->sql + s->len, n + 1, fmt, ap);
	va_end(ap);
	s->len += n;
}

static void
sql_free(Sql *s)
{
	free(s->sql);
}

/* The local connection of the UDF call in progress: opened by its first statement, shared by the next ones, closed at the end */
static __thread MYSQL *call_conn;

void
local_close(void)
{
	if (call_conn)
		SQL(close) (call_conn);
	call_conn = NULL;
}

/* Runs the statement in s as the server itself (no user, no database: names are qualified) and returns its rows */
static MYSQL_RES *
sql_run(Sql *s, char *err, size_t errlen)
{
	MYSQL_RES  *r;

	if (call_conn == NULL)
	{
		call_conn = SQL(init) (NULL);
		if (call_conn == NULL || SQL(real_connect_local) (call_conn) == NULL)
		{
			local_close();
			snprintf(err, errlen, "sift: could not open a local connection to the server");
			return NULL;
		}
		SQL(set_character_set) (call_conn, "utf8mb4");
	}
	if (SQL(real_query) (call_conn, s->sql, s->len) != 0 || (r = SQL(store_result) (call_conn)) == NULL)
	{
		snprintf(err, errlen, "sift: reading the table failed: %s", SQL(error) (call_conn));
		return NULL;
	}
	return r;
}

/* A statement that waits for a metadata lock of a pending DDL would deadlock with the statement that called us */
#define SQL_PREFIX		"SET STATEMENT lock_wait_timeout=5 FOR "

int
host_queue(void *ctx, SiftIdx *ix, int64_t after, SxEvent **out, size_t *n, char *err, size_t errlen)
{
	Sql			s = {0};
	MYSQL_RES  *r;
	size_t		rows;

	sql_put(&s, SQL_PREFIX "SELECT seq, op, pk FROM %s WHERE seq > %lld AND op <> 'B' ORDER BY seq", sx_cfg(ix, "queue"),
			(long long) after);
	r = sql_run(&s, err, errlen);
	if (r == NULL)
	{
		sql_free(&s);
		*out = NULL;
		*n = 0;
		return -1;
	}
	rows = SQL(num_rows) (r);
	*out = malloc((rows + 1) * sizeof(SxEvent));
	for (size_t i = 0; i < rows; i++)
	{
		MYSQL_ROW	row = SQL(fetch_row) (r);

		(*out)[i] = (SxEvent) {strtoll(row[0], NULL, 10), strtoll(row[2], NULL, 10), row[1][0]};
	}
	*n = rows;
	SQL(free_result) (r);
	sql_free(&s);
	return 0;
}

static void
put_keys(Sql *s, const int64_t *pks, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sql_put(s, "%s%lld", i ? "," : "", (long long) pks[i]);
}

typedef struct KeyAt
{
	int64_t		pk;
	size_t		at;
} KeyAt;

static int
cmp_keyat(const void *a, const void *b)
{
	int64_t		x = ((const KeyAt *) a)->pk,
				y = ((const KeyAt *) b)->pk;

	return (x > y) - (x < y);
}

static int
host_fetch(void *ctx, SiftIdx *ix, const int64_t *pks, size_t n, SxRow *rows, char *err, size_t errlen)
{
	Sql			s = {0};
	MYSQL_RES  *r;
	KeyAt	   *ord = malloc(n * sizeof(KeyAt));
	MYSQL_ROW	row;

	for (size_t i = 0; i < n; i++)
	{
		rows[i] = (SxRow) {pks[i], NULL, 0};
		ord[i] = (KeyAt) {pks[i], i};
	}
	qsort(ord, n, sizeof(KeyAt), cmp_keyat);
	sql_put(&s, SQL_PREFIX "SELECT %s, CONVERT(%s USING utf8mb4) FROM %s WHERE %s IN (", sx_cfg(ix, "keyexpr"),
			sx_cfg(ix, "textexpr"), sx_cfg(ix, "source"), sx_cfg(ix, "keyexpr"));
	put_keys(&s, pks, n);
	sql_put(&s, ")");
	r = sql_run(&s, err, errlen);
	while (r && (row = SQL(fetch_row) (r)) != NULL)
	{
		KeyAt		key = {strtoll(row[0], NULL, 10), 0};
		KeyAt	   *at = bsearch(&key, ord, n, sizeof(KeyAt), cmp_keyat);

		if (at && row[1])
		{
			unsigned long *len = SQL(fetch_lengths) (r);

			rows[at->at].text = malloc(len[1] + 1);
			memcpy(rows[at->at].text, row[1], len[1] + 1);
			rows[at->at].len = len[1];
		}
	}
	if (r)
		SQL(free_result) (r);
	sql_free(&s);
	free(ord);
	return r ? 0 : -1;
}

static int
host_exists(void *ctx, SiftIdx *ix, const int64_t *pks, size_t n, uint8_t *present, char *err, size_t errlen)
{
	Sql			s = {0};
	MYSQL_RES  *r;
	KeyAt	   *ord = malloc(n * sizeof(KeyAt));
	MYSQL_ROW	row;

	for (size_t i = 0; i < n; i++)
	{
		present[i] = 0;
		ord[i] = (KeyAt) {pks[i], i};
	}
	qsort(ord, n, sizeof(KeyAt), cmp_keyat);
	sql_put(&s, SQL_PREFIX "SELECT %s FROM %s WHERE %s IN (", sx_cfg(ix, "keyexpr"), sx_cfg(ix, "source"),
			sx_cfg(ix, "keyexpr"));
	put_keys(&s, pks, n);
	sql_put(&s, ")");
	r = sql_run(&s, err, errlen);
	while (r && (row = SQL(fetch_row) (r)) != NULL)
	{
		KeyAt		key = {strtoll(row[0], NULL, 10), 0};
		KeyAt	   *at = bsearch(&key, ord, n, sizeof(KeyAt), cmp_keyat);

		if (at)
			present[at->at] = 1;
	}
	if (r)
		SQL(free_result) (r);
	sql_free(&s);
	free(ord);
	return r ? 0 : -1;
}

const SxHost host = {.queue = host_queue,.fetch = host_fetch,.exists = host_exists};
