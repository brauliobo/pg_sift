/*
 * udf.c - the SQL surface of sift for MariaDB: user defined functions (CREATE FUNCTION ... SONAME 'libsift_udf.so').
 *
 *   sift_create(idx, cfg), sift_exists(idx), sift_drop(idx)     the index of a table (sift_attach builds the cfg)
 *   sift_topk(idx, query, k [, filter])   JSON [{"id": key, "s": score}, ...], best BM25 first, for JSON_TABLE
 *   sift_count(idx, query [, filter])     number of matches
 *   sift_matches(idx, query [, filter])   JSON [key, ...] of all matches
 *   sift_idset(key)                       AGGREGATE: the set of keys, the filter argument of the three above
 *   sift_drain(idx, seq, op, key, text)   AGGREGATE: applies queue rows (sift_sync); sift_load: rebuilds from the table
 *   sift_merge(idx [, 'full']), sift_stats(idx), sift_watermark(idx), sift_tokens(idx, text), sift_version(), sift_ping(k)
 * The queries read the queue, the table and nothing else through a local connection of the server (the SQL service of
 * the server: no network, no password), see the SxHost callbacks. Errors reach the client as SQL errors.
 * This file includes MariaDB headers and none of PostgreSQL; the engine is linked in behind engine.h.
 */
#include <my_global.h>
#include <my_sys.h>
#include <mysql.h>
#include <mysql/service_sql.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "engine.h"

#define EXPORT		__attribute__((visibility("default")))
#define ER_SIFT		1105		/* ER_UNKNOWN_ERROR */

/* The UDF interface this file is written against: a server that lays these out differently is refused at build time */
#if MYSQL_VERSION_ID < 100500 || MYSQL_VERSION_ID >= 140000
#error "sift_udf: built against MariaDB headers outside 10.5 .. 13.x, see mariadb/README.md (supported versions)"
#endif
_Static_assert(sizeof(UDF_INIT) == 40 && offsetof(UDF_INIT, ptr) == 16, "UDF_INIT changed");
_Static_assert(sizeof(UDF_ARGS) == 64 && offsetof(UDF_ARGS, lengths) == 24, "UDF_ARGS changed");
_Static_assert(sizeof(my_bool) == 1, "my_bool changed");

typedef char sx_bool;

extern char mysql_real_data_home[];
extern struct sql_service_st sql_service_handler;

#define SQL(f)		sql_service_handler.mysql_##f##_func

static pthread_once_t dirs_once = PTHREAD_ONCE_INIT;

static void
set_dirs(void)
{
	const char *env = getenv("SIFT_DIR");
	char		base[512];

	if (env)
		snprintf(base, sizeof(base), "%s", env);
	else
		snprintf(base, sizeof(base), "%s.sift", mysql_real_data_home);
	mkdir(base, 0755);
	sx_set_dirs(base, base);
}

/* The compiled-for series (13.0) against the running server's: another series may lay the interfaces out differently */
static char abi_error[200];

static void
abi_check(void)
{
	const char *running = dlsym(RTLD_DEFAULT, "server_version");
	int			a = 0,
				b = 0;

	if (running == NULL || sscanf(running, "%d.%d", &a, &b) != 2)
		return;
	if (a * 10000 + b * 100 != MYSQL_VERSION_ID / 100 * 100)
		snprintf(abi_error, sizeof(abi_error), "sift: libsift_udf.so was built for MariaDB %s, this server is %s: rebuild it "
				 "against the headers of this server", MYSQL_SERVER_VERSION, running);
}

static pthread_once_t abi_once = PTHREAD_ONCE_INIT;

/* Checks the number of arguments (nmin .. strlen(types)) and sets their types: s string, i integer */
static sx_bool
check_args(UDF_ARGS *args, char *message, const char *usage, unsigned nmin, const char *types)
{
	pthread_once(&abi_once, abi_check);
	if (abi_error[0])
	{
		snprintf(message, MYSQL_ERRMSG_SIZE, "%s", abi_error);
		return 1;
	}
	if (args->arg_count < nmin || args->arg_count > strlen(types))
	{
		snprintf(message, MYSQL_ERRMSG_SIZE, "usage: %s", usage);
		return 1;
	}
	for (unsigned i = 0; i < args->arg_count; i++)
		args->arg_type[i] = types[i] == 's' ? STRING_RESULT : INT_RESULT;
	return 0;
}

static void
sx_raise(const char *err, char *error)
{
	my_printf_error(ER_SIFT, "%s", MYF(0), err);
	*error = 1;
}

/* A result in the buffer kept in initid->ptr, grown as needed */
static char *
result_buf(UDF_INIT *initid, size_t need)
{
	size_t		cap = initid->ptr ? *(size_t *) initid->ptr : 0;

	if (need + sizeof(size_t) + 1 > cap)
	{
		free(initid->ptr);
		cap = (need + sizeof(size_t) + 1) * 2;
		initid->ptr = malloc(cap);
		*(size_t *) initid->ptr = cap;
	}
	return initid->ptr + sizeof(size_t);
}

static void
common_deinit(UDF_INIT *initid)
{
	free(initid->ptr);
	initid->ptr = NULL;
}

/* The name of an index is a directory name: letters, digits and underscores */
static char *
arg_name(UDF_ARGS *args, int i, char *buf, size_t n, char *error)
{
	size_t		len = args->args[i] ? args->lengths[i] : 0;

	if (len == 0 || len >= n || strspn(args->args[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != len)
	{
		sx_raise("sift: an index name is 1 to 63 letters, digits or underscores", error);
		return NULL;
	}
	snprintf(buf, n, "%.*s", (int) len, args->args[i]);
	return buf;
}

static SiftIdx *
open_index(UDF_ARGS *args, char *error)
{
	char		name[64],
				err[512];
	SiftIdx    *ix;

	pthread_once(&dirs_once, set_dirs);
	if (arg_name(args, 0, name, sizeof(name), error) == NULL)
		return NULL;
	if ((ix = sx_open(name, err, sizeof(err))) == NULL)
		sx_raise(err, error);
	return ix;
}

/* ------------------------------------------------ the host: the queue, the table, through a local connection */

typedef struct Sql
{
	MYSQL	   *m;
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
	if (s->m)
		SQL(close) (s->m);
	free(s->sql);
}

/* Runs the statement in s as the server itself (no user, no database: names are qualified) and returns its rows */
static MYSQL_RES *
sql_run(Sql *s, char *err, size_t errlen)
{
	MYSQL_RES  *r;

	s->m = SQL(init) (NULL);
	if (s->m == NULL || SQL(real_connect_local) (s->m) == NULL)
	{
		snprintf(err, errlen, "sift: could not open a local connection to the server");
		return NULL;
	}
	SQL(set_character_set) (s->m, "utf8mb4");
	if (SQL(real_query) (s->m, s->sql, s->len) != 0 || (r = SQL(store_result) (s->m)) == NULL)
	{
		snprintf(err, errlen, "sift: reading the table failed: %s", SQL(error) (s->m));
		return NULL;
	}
	return r;
}

/* A statement that waits for a metadata lock of a pending DDL would deadlock with the statement that called us */
#define SQL_PREFIX		"SET STATEMENT lock_wait_timeout=5 FOR "

static int
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

static const SxHost host = {.queue = host_queue,.fetch = host_fetch,.exists = host_exists};

/* ------------------------------------------------ sift_create / sift_exists / sift_drop / sift_version */

EXPORT sx_bool
sift_create_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 64;
	return check_args(args, message, "sift_create(index, config)", 2, "ss");
}

EXPORT char *
sift_create(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	char		name[64],
				err[512],
			   *cfg;

	pthread_once(&dirs_once, set_dirs);
	if (arg_name(args, 0, name, sizeof(name), error) == NULL)
		return NULL;
	cfg = strndup(args->args[1], args->lengths[1]);
	if (sx_create(name, cfg, err, sizeof(err)) != 0)
	{
		free(cfg);
		sx_raise(err, error);
		return NULL;
	}
	free(cfg);
	*length = snprintf(result, 255, "created %s", name);
	return result;
}

EXPORT void
sift_create_deinit(UDF_INIT *initid)
{
}

EXPORT sx_bool
sift_exists_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return check_args(args, message, "sift_exists(index)", 1, "s");
}

EXPORT long long
sift_exists(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	char		name[64];

	pthread_once(&dirs_once, set_dirs);
	return arg_name(args, 0, name, sizeof(name), error) ? sx_exists(name) : 0;
}

EXPORT void
sift_exists_deinit(UDF_INIT *initid)
{
}

EXPORT sx_bool
sift_drop_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	return check_args(args, message, "sift_drop(index)", 1, "s");
}

EXPORT char *
sift_drop(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	char		name[64],
				err[512];

	pthread_once(&dirs_once, set_dirs);
	if (arg_name(args, 0, name, sizeof(name), error) == NULL)
		return NULL;
	if (sx_drop(name, err, sizeof(err)) != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	*length = snprintf(result, 255, "dropped %s", name);
	return result;
}

EXPORT void
sift_drop_deinit(UDF_INIT *initid)
{
}

EXPORT sx_bool
sift_version_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 300;
	return check_args(args, message, "sift_version()", 0, "");
}

EXPORT char *
sift_version(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	*length = snprintf(result, 255, "%s", sx_version());
	return result;
}

EXPORT void
sift_version_deinit(UDF_INIT *initid)
{
}

/* ------------------------------------------------ the filter argument: sift_idset */

#define IDSET_MAGIC		"SIFTIDS1"

typedef struct IdSet
{
	int64_t    *ids;
	size_t		n,
				cap;
	char	   *out;			/* the result, kept until the next one */
} IdSet;

static int
cmp_i64(const void *a, const void *b)
{
	int64_t		x = *(const int64_t *) a,
				y = *(const int64_t *) b;

	return (x > y) - (x < y);
}

static size_t
sort_unique(int64_t *ids, size_t n)
{
	size_t		w = 0;

	qsort(ids, n, sizeof(int64_t), cmp_i64);
	for (size_t i = 0; i < n; i++)
		if (w == 0 || ids[w - 1] != ids[i])
			ids[w++] = ids[i];
	return w;
}

EXPORT sx_bool
sift_idset_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 0;
	initid->max_length = 256 * 1024 * 1024;
	if (check_args(args, message, "sift_idset(key)", 1, "i"))
		return 1;
	initid->ptr = calloc(1, sizeof(IdSet));
	return 0;
}

EXPORT void
sift_idset_deinit(UDF_INIT *initid)
{
	IdSet	   *s = (IdSet *) initid->ptr;

	if (s)
	{
		free(s->ids);
		free(s->out);
	}
	free(s);
}

EXPORT void
sift_idset_clear(UDF_INIT *initid, char *is_null, char *error)
{
	((IdSet *) initid->ptr)->n = 0;
}

EXPORT void
sift_idset_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	IdSet	   *s = (IdSet *) initid->ptr;

	if (args->args[0] == NULL)
		return;
	if (s->n == s->cap)
		s->ids = realloc(s->ids, (s->cap = s->cap ? s->cap * 2 : 1024) * sizeof(int64_t));
	s->ids[s->n++] = *(long long *) args->args[0];
}

/* The set as a string: the magic, the count and the ascending keys, 8 bytes each */
EXPORT char *
sift_idset(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	IdSet	   *s = (IdSet *) initid->ptr;
	char	   *out;
	int64_t		n;

	s->n = sort_unique(s->ids, s->n);
	n = s->n;
	free(s->out);
	out = s->out = malloc(16 + n * sizeof(int64_t));
	memcpy(out, IDSET_MAGIC, 8);
	memcpy(out + 8, &n, 8);
	memcpy(out + 16, s->ids, n * sizeof(int64_t));
	*length = 16 + n * sizeof(int64_t);
	return out;
}

/* The filter argument: an idset, or a JSON array of keys; the keys are in *ids (malloc'd) */
static sx_bool
parse_filter(UDF_ARGS *args, int i, SxIdSet *flt, int64_t **ids, char *error)
{
	const char *p;
	size_t		len;

	*ids = NULL;
	if ((unsigned) i >= args->arg_count || args->args[i] == NULL)
		return 0;
	p = args->args[i];
	len = args->lengths[i];
	if (len >= 16 && memcmp(p, IDSET_MAGIC, 8) == 0)
	{
		int64_t		n;

		memcpy(&n, p + 8, 8);
		if (n < 0 || len != 16 + (size_t) n * 8)
		{
			sx_raise("sift: the filter is not a sift_idset", error);
			return 1;
		}
		*ids = malloc(n * 8 + 8);
		memcpy(*ids, p + 16, n * 8);
		*flt = (SxIdSet) {*ids, n};
		return 0;
	}
	{
		size_t		n = 0,
					cap = 64;
		char	   *end;
		char	   *copy = strndup(p, len);
		const char *q = copy;

		*ids = malloc(cap * 8);
		while (*q && *q != '[')
			q++;
		if (*q != '[')
		{
			free(copy);
			free(*ids);
			*ids = NULL;
			sx_raise("sift: the filter is a sift_idset or a JSON array of keys", error);
			return 1;
		}
		for (q++; *q && *q != ']';)
		{
			long long	v = strtoll(q, &end, 10);

			if (end == q)
			{
				q++;
				continue;
			}
			if (n == cap)
				*ids = realloc(*ids, (cap *= 2) * 8);
			(*ids)[n++] = v;
			q = end;
		}
		free(copy);
		n = sort_unique(*ids, n);
		*flt = (SxIdSet) {*ids, n};
	}
	return 0;
}

/* ------------------------------------------------ sift_topk */

EXPORT sx_bool
sift_topk_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 16 * 1024 * 1024;
	initid->ptr = NULL;
	return check_args(args, message, "sift_topk(index, query, k [, filter])", 3, "ssis");
}

EXPORT char *
sift_topk(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix;
	int			k = (int) *(long long *) args->args[2],
				n = 0;
	SxHit	   *hits;
	char		err[512],
			   *out;
	size_t		w = 0;
	SxIdSet		flt;
	int64_t    *ids;

	if (args->args[1] == NULL || k < 1 || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (parse_filter(args, 3, &flt, &ids, error))
		return NULL;
	hits = malloc(sizeof(SxHit) * k);
	if (sx_topk(ix, &host, args->args[1], args->lengths[1], k, ids ? &flt : NULL, hits, &n, err, sizeof(err)) != 0)
	{
		free(hits);
		free(ids);
		sx_raise(err, error);
		return NULL;
	}
	out = result_buf(initid, (size_t) n * 48 + 4);
	out[w++] = '[';
	for (int i = 0; i < n; i++)
		w += sprintf(out + w, "%s{\"id\":%lld,\"s\":%.9g}", i ? "," : "", (long long) hits[i].pk, hits[i].score);
	out[w++] = ']';
	free(hits);
	free(ids);
	*length = w;
	return out;
}

EXPORT void
sift_topk_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

/* ------------------------------------------------ sift_count */

EXPORT sx_bool
sift_count_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	return check_args(args, message, "sift_count(index, query [, filter])", 2, "sss");
}

EXPORT long long
sift_count(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	SiftIdx    *ix;
	int64_t		count;
	char		err[512];
	SxIdSet		flt;
	int64_t    *ids;
	int			rc;

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return 0;
	}
	if (parse_filter(args, 2, &flt, &ids, error))
		return 0;
	rc = sx_count(ix, &host, args->args[1], args->lengths[1], ids ? &flt : NULL, &count, err, sizeof(err));
	free(ids);
	if (rc != 0)
	{
		sx_raise(err, error);
		return 0;
	}
	return count;
}

EXPORT void
sift_count_deinit(UDF_INIT *initid)
{
}

/* ------------------------------------------------ sift_matches */

EXPORT sx_bool
sift_matches_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 64 * 1024 * 1024;
	initid->ptr = NULL;
	return check_args(args, message, "sift_matches(index, query [, filter])", 2, "sss");
}

EXPORT char *
sift_matches(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix;
	int64_t    *pks,
				n;
	char		err[512],
			   *out;
	size_t		w = 0;
	SxIdSet		flt;
	int64_t    *ids;
	int			rc;

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (parse_filter(args, 2, &flt, &ids, error))
		return NULL;
	rc = sx_matches(ix, &host, args->args[1], args->lengths[1], ids ? &flt : NULL, &pks, &n, err, sizeof(err));
	free(ids);
	if (rc != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	out = result_buf(initid, (size_t) n * 21 + 4);
	out[w++] = '[';
	for (int64_t i = 0; i < n; i++)
		w += sprintf(out + w, "%s%lld", i ? "," : "", (long long) pks[i]);
	out[w++] = ']';
	free(pks);
	*length = w;
	return out;
}

EXPORT void
sift_matches_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

/* ------------------------------------------------ sift_drain, sift_load: aggregates over the rows of a drain or a rebuild */

typedef struct Drain
{
	SiftIdx    *ix;
	SiftFeed   *feed;
	char		load;
	char		err[512];
} Drain;

static sx_bool
drain_init(UDF_INIT *initid, UDF_ARGS *args, char *message, int load)
{
	Drain	   *d;

	if (load ? check_args(args, message, "sift_load(index, watermark, key, text)", 4, "siis")
		: check_args(args, message, "sift_drain(index, seq, op, key, text)", 5, "sisis"))
		return 1;
	initid->ptr = (char *) (d = calloc(1, sizeof(Drain)));
	d->load = load;
	return 0;
}

EXPORT sx_bool
sift_drain_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return drain_init(initid, args, message, false);
}

EXPORT sx_bool
sift_load_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return drain_init(initid, args, message, true);
}

static void
drain_deinit(UDF_INIT *initid)
{
	Drain	   *d = (Drain *) initid->ptr;

	if (d && d->feed)
		sx_feed_abort(d->feed);
	free(d);
}

EXPORT void
sift_drain_deinit(UDF_INIT *initid)
{
	drain_deinit(initid);
}

EXPORT void
sift_load_deinit(UDF_INIT *initid)
{
	drain_deinit(initid);
}

static void
drain_clear(UDF_INIT *initid, char *is_null, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	char		load = d->load;

	if (d->feed)
		sx_feed_abort(d->feed);
	memset(d, 0, sizeof(Drain));
	d->load = load;
}

EXPORT void
sift_drain_clear(UDF_INIT *initid, char *is_null, char *error)
{
	drain_clear(initid, is_null, error);
}

EXPORT void
sift_load_clear(UDF_INIT *initid, char *is_null, char *error)
{
	drain_clear(initid, is_null, error);
}

/* The row (seq, op, key, text) of a drain or (watermark, key, text) of a load */
static void
drain_add(UDF_INIT *initid, UDF_ARGS *args, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	int			t = d->load ? 3 : 4;
	const char *op = d->load ? "R" : args->args[2];
	long long	seq = d->load ? 0 : *(long long *) args->args[1],
				key = *(long long *) args->args[d->load ? 2 : 3];

	if (d->err[0])
		return;
	if (d->ix == NULL && (d->ix = open_index(args, error)) == NULL)
		return;
	if (d->feed == NULL &&
		(d->feed = sx_feed_begin(d->ix, d->load ? *(long long *) args->args[1] : -1, d->err, sizeof(d->err))) == NULL)
		return;
	if (sx_feed_add(d->feed, seq, op ? op[0] : 'D', key, args->args[t], args->lengths[t], d->err, sizeof(d->err)) != 0)
	{
		sx_feed_abort(d->feed);
		d->feed = NULL;
	}
}

EXPORT void
sift_drain_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	drain_add(initid, args, error);
}

EXPORT void
sift_load_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	drain_add(initid, args, error);
}

/* A load of an empty table has no row to open the index with: it still replaces the contents of the index */
static void
load_empty(Drain *d, UDF_ARGS *args, char *error)
{
	if (d->feed == NULL && d->err[0] == 0 && (d->ix = open_index(args, error)))
		d->feed = sx_feed_begin(d->ix, *(long long *) args->args[1], d->err, sizeof(d->err));
}

static long long
drain_end(UDF_INIT *initid, UDF_ARGS *args, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	int64_t		rows = 0;

	if (d->load && d->ix == NULL)
		load_empty(d, args, error);
	if (d->feed)
	{
		SiftFeed   *f = d->feed;

		d->feed = NULL;
		if (sx_feed_end(f, &rows, d->err, sizeof(d->err)) != 0)
			rows = 0;
	}
	if (d->err[0])
	{
		sx_raise(d->err, error);
		return 0;
	}
	return rows;
}

EXPORT long long
sift_drain(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	return drain_end(initid, args, error);
}

EXPORT long long
sift_load(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	return drain_end(initid, args, error);
}

/* ------------------------------------------------ sift_merge, sift_stats, sift_watermark, sift_tokens, sift_ping */

static char *
stats_json(UDF_INIT *initid, SiftIdx *ix, unsigned long *length, char *error)
{
	SiftIdxStats st;
	char		err[512],
			   *out = result_buf(initid, 512);
	SxEvent    *ev;
	size_t		pending = 0;

	if (sx_stats(ix, &st, err, sizeof(err)) != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	if (sx_cfg(ix, "queue") && host_queue(NULL, ix, st.drained, &ev, &pending, err, sizeof(err)) == 0)
		free(ev);
	*length = sprintf(out, "{\"segments\":%d,\"docs\":%lld,\"dead\":%lld,\"live\":%lld,\"drained\":%lld,\"pending\":%zu,"
					  "\"bytes\":%lld,\"dpp\":%lld,\"docbase\":%lld,\"merges\":%lld,\"format\":%u}",
					  st.nsegs, (long long) st.ndocs, (long long) st.ndead, (long long) st.nlive,
					  (long long) st.drained, pending, (long long) st.bytes, (long long) st.dpp, (long long) st.docbase,
					  (long long) st.merges, st.format);
	return out;
}

EXPORT sx_bool
sift_stats_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->ptr = NULL;
	return check_args(args, message, "sift_stats(index)", 1, "s");
}

EXPORT char *
sift_stats(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix = open_index(args, error);

	return ix ? stats_json(initid, ix, length, error) : NULL;
}

EXPORT void
sift_stats_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

EXPORT sx_bool
sift_watermark_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return check_args(args, message, "sift_watermark(index)", 1, "s");
}

EXPORT long long
sift_watermark(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	SiftIdx    *ix = open_index(args, error);
	SiftIdxStats st;
	char		err[512];

	if (ix == NULL || sx_stats(ix, &st, err, sizeof(err)) != 0)
		return -1;
	return st.drained;
}

EXPORT void
sift_watermark_deinit(UDF_INIT *initid)
{
}

EXPORT sx_bool
sift_merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->ptr = NULL;
	return check_args(args, message, "sift_merge(index [, 'full'])", 1, "ss");
}

EXPORT char *
sift_merge(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix = open_index(args, error);
	char		err[512];
	int			full = args->arg_count > 1 && args->args[1] && strncmp(args->args[1], "full", 4) == 0;

	if (ix && sx_merge(ix, full, err, sizeof(err)) != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	return ix ? stats_json(initid, ix, length, error) : NULL;
}

EXPORT void
sift_merge_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

EXPORT sx_bool
sift_tokens_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->ptr = NULL;
	return check_args(args, message, "sift_tokens(index, text)", 2, "ss");
}

EXPORT char *
sift_tokens(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix;
	char		err[512],
			   *toks,
			   *out;

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (sx_tokens(ix, args->args[1], args->lengths[1], &toks, err, sizeof(err)) != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	*length = strlen(toks);
	out = result_buf(initid, *length);
	memcpy(out, toks, *length);
	free(toks);
	return out;
}

EXPORT void
sift_tokens_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

/* sift_ping(k): the JSON of a top-k of k fake hits without touching the engine, to measure the call and JSON_TABLE cost */
EXPORT sx_bool
sift_ping_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->ptr = NULL;
	return check_args(args, message, "sift_ping(k)", 1, "i");
}

EXPORT char *
sift_ping(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	int			k = (int) *(long long *) args->args[0];
	char	   *out = result_buf(initid, (size_t) k * 48 + 4);
	size_t		w = 0;

	out[w++] = '[';
	for (int i = 0; i < k; i++)
		w += sprintf(out + w, "%s{\"id\":%d,\"s\":%.9g}", i ? "," : "", 1000 + i * 37, 20.0 - i * 0.37);
	out[w++] = ']';
	*length = w;
	return out;
}

EXPORT void
sift_ping_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}
