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
	sx_bool		oom;
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
	{
		char	   *grown = realloc(s->sql, (s->len + n + 1) * 2);

		if (grown == NULL)
		{
			s->oom = 1;
			return;
		}
		s->sql = grown;
		s->cap = (s->len + n + 1) * 2;
	}
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

/* MEM[MEM-PORT-12]: MariaDB udf_host mysql_store_result (2x full queue/fetch copy in client RAM; rows now streamed by mysql_use_result) */
/* Runs the statement in s as the server itself (no user, no database: names are qualified) and returns its rows to fetch one by one */
static MYSQL_RES *
sql_run(Sql *s, char *err, size_t errlen)
{
	MYSQL_RES  *r;

	if (s->oom)
	{
		snprintf(err, errlen, "sift: out of memory");
		return NULL;
	}
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
	if (SQL(real_query) (call_conn, s->sql, s->len) != 0 || (r = SQL(use_result) (call_conn)) == NULL)
	{
		snprintf(err, errlen, "sift: reading the table failed: %s", SQL(error) (call_conn));
		return NULL;
	}
	return r;
}

/* Releases the rows of sql_run once all were fetched: a stream that broke before its end is an error, not a short result */
static int
sql_done(MYSQL_RES *r, char *err, size_t errlen)
{
	unsigned int no = SQL(errno) (call_conn);

	if (no != 0)
		snprintf(err, errlen, "sift: reading the table failed: %s", SQL(error) (call_conn));
	SQL(free_result) (r);
	return no != 0 ? -1 : 0;
}

/* A statement that waits for a metadata lock of a pending DDL would deadlock with the statement that called us */
#define SQL_PREFIX		"SET STATEMENT lock_wait_timeout=5 FOR "

/* The array *p of *cap elements of size bytes, doubled (1024 elements to begin with); 0 when there is no memory */
static sx_bool
grow(void *p, size_t *cap, size_t size)
{
	size_t		ncap = *cap ? *cap * 2 : 1024;
	void	   *bigger = realloc(*(void **) p, ncap * size);

	if (bigger == NULL)
		return 0;
	*(void **) p = bigger;
	*cap = ncap;
	return 1;
}

/* Ends the rows of sql_run: the error of the rows (err) stays when there is one, else the one of the stream */
static int
rows_done(MYSQL_RES *r, sx_bool broke, char *err, size_t errlen)
{
	char		stream_err[256];
	int			rc = sql_done(r, broke ? stream_err : err, broke ? sizeof(stream_err) : errlen);

	return broke ? -1 : rc;
}

int
host_queue(void *ctx, SiftIdx *ix, int64_t after, SxEvent **out, size_t *n, char *err, size_t errlen)
{
	Sql			s = {0};
	MYSQL_RES  *r;
	MYSQL_ROW	row;
	size_t		rows = 0,
				cap = 0;
	sx_bool		broke = 0;

	*out = NULL;
	*n = 0;
	sql_put(&s, SQL_PREFIX "SELECT seq, op, pk FROM %s WHERE seq > %lld AND op <> 'B' ORDER BY seq", sx_cfg(ix, "queue"),
			(long long) after);
	r = sql_run(&s, err, errlen);
	sql_free(&s);
	if (r == NULL)
		return -1;
	while (!broke && (row = SQL(fetch_row) (r)) != NULL)
	{
		broke = row[0] == NULL || row[1] == NULL || row[1][0] == '\0' || row[2] == NULL;
		if (broke)
			snprintf(err, errlen, "sift: the queue has a row without seq, op or key");
		else if (rows == cap && (broke = !grow(out, &cap, sizeof(SxEvent))))
			snprintf(err, errlen, "sift: out of memory");
		else
			(*out)[rows++] = (SxEvent) {strtoll(row[0], NULL, 10), strtoll(row[2], NULL, 10), row[1][0]};
	}
	if (rows_done(r, broke, err, errlen) != 0)
	{
		free(*out);
		*out = NULL;
		return -1;
	}
	*n = rows;
	return 0;
}

int
host_queue_probe(void *ctx, SiftIdx *ix, int64_t after, int64_t *max_seq, size_t *count, char *err, size_t errlen)
{
	Sql			s = {0};
	MYSQL_RES  *r;
	MYSQL_ROW	row;
	sx_bool		broke;

	sql_put(&s, SQL_PREFIX "SELECT COUNT(*), COALESCE(MAX(seq), 0) FROM %s WHERE seq > %lld AND op <> 'B'", sx_cfg(ix, "queue"),
			(long long) after);
	r = sql_run(&s, err, errlen);
	sql_free(&s);
	if (r == NULL)
		return -1;
	row = SQL(fetch_row) (r);
	if ((broke = row == NULL || row[0] == NULL || row[1] == NULL))
		snprintf(err, errlen, "sift: the queue did not answer");
	else
	{
		*count = strtoull(row[0], NULL, 10);
		*max_seq = strtoll(row[1], NULL, 10);
	}
	return rows_done(r, broke, err, errlen);
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

/* The keys sorted with their positions, for the lookup of the keys the table answers with; NULL when n is 0 or there is no memory */
static KeyAt *
keys_sorted(const int64_t *pks, size_t n)
{
	KeyAt	   *ord = malloc((n ? n : 1) * sizeof(KeyAt));

	for (size_t i = 0; ord && i < n; i++)
		ord[i] = (KeyAt) {pks[i], i};
	if (ord)
		qsort(ord, n, sizeof(KeyAt), cmp_keyat);
	return ord;
}

/* Keys in one IN list: a long one is a statement the server takes longer to plan than to run, and may not take at all (max_allowed_packet) */
#define HOST_IN_CHUNK	2048

/* Runs the statement in s with the keys as the end of its IN list; s is released */
static MYSQL_RES *
run_keys(Sql *s, const int64_t *pks, size_t n, char *err, size_t errlen)
{
	MYSQL_RES  *r;

	put_keys(s, pks, n);
	sql_put(s, ")");
	r = sql_run(s, err, errlen);
	sql_free(s);
	return r;
}

/* The row of a lookup: the key of the answer in ord, or NULL when the table answered with a key that was not asked for */
static const KeyAt *
row_key(MYSQL_ROW row, const KeyAt *ord, size_t n)
{
	KeyAt		key = {row[0] ? strtoll(row[0], NULL, 10) : 0, 0};

	return row[0] ? bsearch(&key, ord, n, sizeof(KeyAt), cmp_keyat) : NULL;
}

static int
host_fetch(void *ctx, SiftIdx *ix, const int64_t *pks, size_t n, SxRow *rows, char *err, size_t errlen)
{
	KeyAt	   *ord = keys_sorted(pks, n);
	int			rc = ord ? 0 : -1;

	if (ord == NULL)
		snprintf(err, errlen, "sift: out of memory");
	for (size_t i = 0; i < n; i++)
		rows[i] = (SxRow) {pks[i], NULL, 0};
	for (size_t off = 0; off < n && rc == 0; off += HOST_IN_CHUNK)
	{
		Sql			s = {0};
		MYSQL_RES  *r;
		MYSQL_ROW	row;
		sx_bool		broke = 0;

		sql_put(&s, SQL_PREFIX "SELECT %s, CONVERT(%s USING utf8mb4) FROM %s WHERE %s IN (", sx_cfg(ix, "keyexpr"),
				sx_cfg(ix, "textexpr"), sx_cfg(ix, "source"), sx_cfg(ix, "keyexpr"));
		r = run_keys(&s, pks + off, n - off < HOST_IN_CHUNK ? n - off : HOST_IN_CHUNK, err, errlen);

		while (r && !broke && (row = SQL(fetch_row) (r)) != NULL)
		{
			const KeyAt *at = row_key(row, ord, n);

			if (at && row[1])
			{
				unsigned long *len = SQL(fetch_lengths) (r);

				if ((broke = (rows[at->at].text = malloc(len[1] + 1)) == NULL))
					snprintf(err, errlen, "sift: out of memory");
				else
				{
					memcpy(rows[at->at].text, row[1], len[1] + 1);
					rows[at->at].len = len[1];
				}
			}
		}
		rc = r ? rows_done(r, broke, err, errlen) : -1;
	}
	free(ord);
	return rc;
}

static int
host_exists(void *ctx, SiftIdx *ix, const int64_t *pks, size_t n, uint8_t *present, char *err, size_t errlen)
{
	KeyAt	   *ord = keys_sorted(pks, n);
	int			rc = ord ? 0 : -1;

	if (ord == NULL)
		snprintf(err, errlen, "sift: out of memory");
	memset(present, 0, n);
	for (size_t off = 0; off < n && rc == 0; off += HOST_IN_CHUNK)
	{
		Sql			s = {0};
		MYSQL_RES  *r;
		MYSQL_ROW	row;

		sql_put(&s, SQL_PREFIX "SELECT %s FROM %s WHERE %s IN (", sx_cfg(ix, "keyexpr"), sx_cfg(ix, "source"),
				sx_cfg(ix, "keyexpr"));
		r = run_keys(&s, pks + off, n - off < HOST_IN_CHUNK ? n - off : HOST_IN_CHUNK, err, errlen);

		while (r && (row = SQL(fetch_row) (r)) != NULL)
		{
			const KeyAt *at = row_key(row, ord, n);

			if (at)
				present[at->at] = 1;
		}
		rc = r ? rows_done(r, 0, err, errlen) : -1;
	}
	free(ord);
	return rc;
}

const SxHost host = {.queue = host_queue,.queue_probe = host_queue_probe,.fetch = host_fetch,.exists = host_exists};
