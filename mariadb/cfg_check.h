/*
 * cfg_check.h - the check of the definition of an index (the cfg text of sift_create, kept in its MANIFEST).
 */
#ifndef SIFT_CFG_CHECK_H
#define SIFT_CFG_CHECK_H

#include <stdbool.h>
#include <stddef.h>

#define NMETA		6

/* The keys of the definition that name SQL the library runs as the server itself: queue, keyexpr, source, textexpr, database, table */
extern const char *const cfg_meta_names[NMETA];

/* The value of key in the lines of cfg, malloc'd; NULL when absent */
extern char *cfg_value(const char *cfg, const char *key);

/* True when the definition has none of the SQL keys or all of them in the forms sift_attach writes; else err says which is not */
extern bool cfg_check(const char *name, const char *cfg, char *err, size_t errlen);

#endif							/* SIFT_CFG_CHECK_H */
