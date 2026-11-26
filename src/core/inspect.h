/*
 * core/inspect.h - who may look at a sift index or define something on a table: one rule for every view and inspector (sift._may_inspect)
 * and the owner checks of the catalogs a role writes through the sift functions (core/inspect.c).
 */
#ifndef SIFT_INSPECT_H
#define SIFT_INSPECT_H

#include "fmgr.h"
#include "utils/acl.h"
#include "utils/rel.h"

/* Whether the caller may look at the sift objects of the table: SELECT on it, a maintain right on it, or membership of pg_monitor */
extern bool sift_may_inspect(Oid heap);

/* sift_may_inspect for another role */
extern bool sift_may_inspect_as(Oid user, Oid heap);

/* The rights that remove the rows of a table, which dropping its parts and a TTL do: a maintain right does not */
#define SIFT_RETENTION_ACL (ACL_DELETE | ACL_TRUNCATE)

/* Whether the caller is a member of pg_read_all_stats; sift_require_stats_reader errors unless it is, naming the function fn */
extern bool sift_may_read_stats(void);
extern void sift_require_stats_reader(const char *fn);

/* Errors unless the caller holds one of the rights any_of on the table of the relation (an index: the table of the index): the check a function makes before it locks the relation, and again after */
extern void sift_require_acl(Oid relid, AclMode any_of);

/* sift_may_inspect of the table of an index, or of a table: what the registries of the server (quarantine, jobs, progress) filter their rows by */
extern bool sift_may_inspect_rel(Oid rel);

/* Errors unless the trigger function fn was called as a BEFORE ROW trigger of the catalog table ("sift.table_paths") of the extension */
extern void sift_check_catalog_trigger(FunctionCallInfo fcinfo, const char *fn, const char *catalog);

/* Errors unless sift_may_inspect: rel is a sift index, or a sift_table */
extern void sift_check_inspect(Relation rel);

/* Errors unless the relation uses the access method want ("sift", "sift_col"; NULL: any sift one); one of another sift access method is named with the function that serves it (use, may be NULL) */
extern void sift_inspect_expect(Oid relid, const char *want, const char *use);
extern void sift_refuse_partitioned_index(bool partitioned, const char *name);

#endif							/* SIFT_INSPECT_H */
