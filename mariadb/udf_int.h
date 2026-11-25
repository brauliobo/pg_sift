/*
 * udf_int.h - what the files of the UDF library share: the MariaDB headers, the argument checks, the error and result
 * helpers and the host callbacks of the engine (udf.c, udf_host.c, udf_query.c, udf_drain.c).
 */
#ifndef SIFT_UDF_INT_H
#define SIFT_UDF_INT_H

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

/* The most rows a top-k asks for: a result of that many hits is about 50 MB of JSON */
#define SX_MAX_K		1000000

/* A malloc'd pointer that is freed whichever way the function returns */
static inline void
free_ptr(void *p)
{
	free(*(void **) p);
}
#define AUTO_FREE		__attribute__((cleanup(free_ptr)))

/* udf.c */
extern sx_bool check_args(UDF_ARGS *args, char *message, const char *usage, unsigned nmin, const char *types);
extern void sx_raise(const char *err, char *error);
/* The buffer of a result of need bytes, or NULL with the error raised */
extern char *result_buf(UDF_INIT *initid, size_t need, char *error);
extern void common_deinit(UDF_INIT *initid);
extern SiftIdx *open_index(UDF_ARGS *args, char *error);

/* A reference of open_index that the function gives back whichever way it returns */
extern void ix_scope_end(SiftIdx **ixp);
#define SCOPED_IX	SiftIdx *__attribute__((cleanup(ix_scope_end)))

/* udf_host.c */
extern const SxHost host;
extern void local_close(void);
extern int host_queue(void *ctx, SiftIdx *ix, int64_t after, SxEvent **out, size_t *n, char *err, size_t errlen);
extern int host_queue_probe(void *ctx, SiftIdx *ix, int64_t after, int64_t *max_seq, size_t *count, char *err, size_t errlen);

#endif							/* SIFT_UDF_INT_H */
