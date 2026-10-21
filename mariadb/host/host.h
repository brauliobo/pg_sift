/*
 * host.h - the host interface of the portable core (src/core/host.h) for MariaDB, selected with -DSIFT_HOST_MARIADB.
 *
 * The vocabulary is the one of the SQLite host: memory contexts over malloc, ereport as a longjmp to the innermost handler,
 * StringInfo, List, hash tables, UTF-8 and regex helpers, atomics and locks, spool files. It is plain C over the C library and
 * keeps its state per thread (the current memory context, the error), which is what a server that runs queries on many threads
 * needs, so this host uses sqlite/host/host.c (with container.c, mb.c and spool.c) and host_regex.c as they are. What differs is
 * the storage: the pages, buffers and relations of host_store.h are served by store.c, which maps segment files.
 */
#ifndef SIFT_MDB_HOST_H
#define SIFT_MDB_HOST_H

#include "../../sqlite/host/host.h"

#endif							/* SIFT_MDB_HOST_H */
