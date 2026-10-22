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
 * This file has the argument checks and results of every UDF, the index and server functions; the queries are in
 * udf_query.c, the aggregates of a drain in udf_drain.c, the local connection to the server in udf_host.c.
 * This file includes MariaDB headers and none of PostgreSQL; the engine is linked in behind engine.h.
 */

#include "udf_int.h"

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
sx_bool
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

void
sx_raise(const char *err, char *error)
{
	my_printf_error(ER_SIFT, "%s", MYF(0), err);
	*error = 1;
}

/* A result in the buffer kept in initid->ptr, grown as needed */
char *
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

void
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

SiftIdx *
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
	local_close();
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
