/*
 * udf.c - the SQL surface of sift for MariaDB: user defined functions (CREATE FUNCTION ... SONAME 'libsift_udf.so').
 *
 *   sift_create(idx, dpp)            creates an index (directory under the data directory); dpp: documents per page
 *   sift_topk(idx, query, k)         JSON [{"id": pk, "s": score}, ...], best BM25 first, for JSON_TABLE
 *   sift_count(idx, query)           number of matches
 *   sift_matches(idx, query)         JSON [pk, ...] of all matches
 *   sift_drain(idx, seq, op, pk, t)  AGGREGATE: applies queue rows (see sift.sql), returns the rows applied
 *   sift_merge(idx), sift_stats(idx), sift_tokens(idx, text), sift_drop(idx), sift_ping(k)
 * Errors of the engine reach the client as SQL errors (my_printf_error). This file includes MariaDB headers and none of
 * PostgreSQL; the engine is linked in behind engine.h.
 */
#include <my_global.h>
#include <my_sys.h>
#include <mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define EXPORT		__attribute__((visibility("default")))
#define ER_SIFT		1105		/* ER_UNKNOWN_ERROR */

extern char mysql_real_data_home[];

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

static void
ensure_dirs(void)
{
	pthread_once(&dirs_once, set_dirs);
}

static int
check_args(UDF_ARGS *args, char *message, const char *usage, unsigned n, const char *types)
{
	if (args->arg_count != n)
	{
		snprintf(message, MYSQL_ERRMSG_SIZE, "usage: %s", usage);
		return 1;
	}
	for (unsigned i = 0; i < n; i++)
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
	static __thread size_t unused;
	size_t		cap = initid->ptr ? *(size_t *) initid->ptr : 0;

	(void) unused;
	if (need + sizeof(size_t) + 1 > cap)
	{
		free(initid->ptr);
		cap = (need + sizeof(size_t) + 1) * 2;
		initid->ptr = malloc(cap);
		*(size_t *) initid->ptr = cap;
	}
	return initid->ptr + sizeof(size_t);
}

static char *
arg_name(UDF_ARGS *args, int i, char *buf, size_t n)
{
	snprintf(buf, n, "%.*s", (int) args->lengths[i], args->args[i]);
	return buf;
}

static void
common_deinit(UDF_INIT *initid)
{
	free(initid->ptr);
	initid->ptr = NULL;
}

static SiftIdx *
open_index(UDF_ARGS *args, char *error)
{
	char		name[64],
				err[512];
	SiftIdx    *ix;

	ensure_dirs();
	if (args->args[0] == NULL)
	{
		sx_raise("the index name is NULL", error);
		return NULL;
	}
	if ((ix = sx_open(arg_name(args, 0, name, sizeof(name)), err, sizeof(err))) == NULL)
		sx_raise(err, error);
	return ix;
}

/* ------------------------------------------------ sift_create / sift_drop */

EXPORT my_bool
sift_create_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 64;
	return check_args(args, message, "sift_create(index, documents_per_page)", 2, "si");
}

EXPORT char *
sift_create(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	char		name[64],
				err[512];

	ensure_dirs();
	if (sx_create(arg_name(args, 0, name, sizeof(name)), (int) *(long long *) args->args[1], err, sizeof(err)) != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	*length = snprintf(result, 255, "created %s", name);
	return result;
}

EXPORT void
sift_create_deinit(UDF_INIT *initid)
{
}

EXPORT my_bool
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

	ensure_dirs();
	if (sx_drop(arg_name(args, 0, name, sizeof(name)), err, sizeof(err)) != 0)
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

/* ------------------------------------------------ sift_topk */

EXPORT my_bool
sift_topk_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 16 * 1024 * 1024;
	initid->ptr = NULL;
	return check_args(args, message, "sift_topk(index, query, k)", 3, "ssi");
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

	if (args->args[1] == NULL || k < 1 || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	hits = malloc(sizeof(SxHit) * k);
	if (sx_topk(ix, args->args[1], args->lengths[1], k, hits, &n, err, sizeof(err)) != 0)
	{
		free(hits);
		sx_raise(err, error);
		return NULL;
	}
	out = result_buf(initid, (size_t) n * 48 + 4);
	out[w++] = '[';
	for (int i = 0; i < n; i++)
		w += sprintf(out + w, "%s{\"id\":%lld,\"s\":%.9g}", i ? "," : "", (long long) hits[i].pk, hits[i].score);
	out[w++] = ']';
	free(hits);
	*length = w;
	return out;
}

EXPORT void
sift_topk_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

/* ------------------------------------------------ sift_count */

EXPORT my_bool
sift_count_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	return check_args(args, message, "sift_count(index, query)", 2, "ss");
}

EXPORT long long
sift_count(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	SiftIdx    *ix;
	int64_t		count;
	char		err[512];

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return 0;
	}
	if (sx_count(ix, args->args[1], args->lengths[1], &count, err, sizeof(err)) != 0)
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

EXPORT my_bool
sift_matches_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 64 * 1024 * 1024;
	initid->ptr = NULL;
	return check_args(args, message, "sift_matches(index, query)", 2, "ss");
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

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (sx_matches(ix, args->args[1], args->lengths[1], &pks, &n, err, sizeof(err)) != 0)
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

/* ------------------------------------------------ sift_drain: aggregate over the queue rows */

typedef struct Drain
{
	SiftIdx    *ix;
	SiftFeed   *feed;
	char		err[512];
} Drain;

EXPORT my_bool
sift_drain_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	if (check_args(args, message, "sift_drain(index, seq, op, pk, text)", 5, "sisis"))
		return 1;
	initid->ptr = calloc(1, sizeof(Drain));
	return 0;
}

EXPORT void
sift_drain_deinit(UDF_INIT *initid)
{
	Drain	   *d = (Drain *) initid->ptr;

	if (d && d->feed)
		sx_feed_abort(d->feed);
	free(d);
}

EXPORT void
sift_drain_clear(UDF_INIT *initid, char *is_null, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;

	if (d->feed)
		sx_feed_abort(d->feed);
	memset(d, 0, sizeof(Drain));
}

EXPORT void
sift_drain_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	const char *op = args->args[2];

	if (d->err[0])
		return;
	if (d->ix == NULL && (d->ix = open_index(args, error)) == NULL)
		return;
	if (d->feed == NULL && (d->feed = sx_feed_begin(d->ix, INT64_MAX, d->err, sizeof(d->err))) == NULL)
		return;
	if (sx_feed_add(d->feed, *(long long *) args->args[1], op ? op[0] : 'D', *(long long *) args->args[3],
					args->args[4], args->lengths[4], d->err, sizeof(d->err)) != 0)
	{
		sx_feed_abort(d->feed);
		d->feed = NULL;
	}
}

EXPORT long long
sift_drain(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	int64_t		rows = 0;

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

/* ------------------------------------------------ sift_merge, sift_stats, sift_tokens, sift_ping */

static char *
stats_json(UDF_INIT *initid, SiftIdx *ix, unsigned long *length, char *error)
{
	SiftIdxStats st;
	char		err[512],
			   *out = result_buf(initid, 512);

	if (sx_stats(ix, &st, err, sizeof(err)) != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	*length = sprintf(out, "{\"segments\":%d,\"docs\":%lld,\"dead\":%lld,\"live\":%lld,\"drained\":%lld,\"bytes\":%lld,\"dpp\":%lld}",
					  st.nsegs, (long long) st.ndocs, (long long) st.ndead, (long long) st.nlive,
					  (long long) st.drained, (long long) st.bytes, (long long) st.dpp);
	return out;
}

EXPORT my_bool
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

EXPORT my_bool
sift_merge_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->ptr = NULL;
	return check_args(args, message, "sift_merge(index)", 1, "s");
}

EXPORT char *
sift_merge(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix = open_index(args, error);
	char		err[512];

	if (ix && sx_merge(ix, err, sizeof(err)) != 0)
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

EXPORT my_bool
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
EXPORT my_bool
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
