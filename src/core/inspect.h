/*
 * core/inspect.h - who may look at a sift index or define something on a table: one rule for every view and inspector (sift._may_inspect)
 * and the owner checks of the catalogs a role writes through the sift functions (core/inspect.c).
 */
#ifndef SIFT_INSPECT_H
#define SIFT_INSPECT_H

#include "utils/rel.h"

/* Whether the caller may look at the sift objects of the table: SELECT on it, a maintain right on it, or membership of pg_monitor */
extern bool sift_may_inspect(Oid heap);

/* Errors unless sift_may_inspect: rel is a sift index, or a sift_table */
extern void sift_check_inspect(Relation rel);

/* Errors unless the relation uses the access method want ("sift", "sift_col"; NULL: any sift one); one of another sift access method is named with the function that serves it (use, may be NULL) */
extern void sift_inspect_expect(Oid relid, const char *want, const char *use);
extern Oid sift_rel_must_exist(Oid relid);

#endif							/* SIFT_INSPECT_H */
