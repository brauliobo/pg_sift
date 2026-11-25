/*
 * agg/v6/unique_key.h - whether a unique index of a table holds only given columns
 */
#ifndef SIFT_AGG_V6_UNIQUE_KEY_H
#define SIFT_AGG_V6_UNIQUE_KEY_H

#include "postgres.h"

#include "nodes/pg_list.h"

/*
 * does a usable unique index of the table hold only columns among atts (nkey compared with `=`, then nord of an ORDER BY, NOT NULL where the index
 * holds them: it takes NULLs for different)? Then a row is the only one with its values of them. *cols, when given, gets the columns of that index.
 */
extern bool sift_v6_unique_key(Oid relid, int nkey, int nord, const int *atts, List **cols);

#endif							/* SIFT_AGG_V6_UNIQUE_KEY_H */
