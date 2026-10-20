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

/* udf.c */
extern sx_bool check_args(UDF_ARGS *args, char *message, const char *usage, unsigned nmin, const char *types);
extern void sx_raise(const char *err, char *error);
extern char *result_buf(UDF_INIT *initid, size_t need);
extern void common_deinit(UDF_INIT *initid);
extern SiftIdx *open_index(UDF_ARGS *args, char *error);

/* udf_host.c */
extern const SxHost host;
extern void local_close(void);
extern int host_queue(void *ctx, SiftIdx *ix, int64_t after, SxEvent **out, size_t *n, char *err, size_t errlen);

#endif							/* SIFT_UDF_INT_H */
