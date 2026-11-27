/*
 * core/store/table_qual.h - the quals of a sift_table its store decides as they are written (a comparison of a column with a constant, an IN list of constants, a NULL test), read off the
 * schema of the store (SiftStoreSchema) alone: the planner of the aggregate and the executor of a plain scan of the table judge a clause alike.
 */
#ifndef SIFT_CORE_STORE_TABLE_QUAL_H
#define SIFT_CORE_STORE_TABLE_QUAL_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "core/store/ops.h"

/* A column of a sift_table whose comparisons its store decides: a number or a time held as it is (not LOSSY, not wide, not a string, whose order a collation may decide) */
extern bool sift_store_table_col_exact(const SiftStoreSchema *t, AttrNumber attno);

/* The clause as the store decides it (the column on the left, without a relabel), NULL when it does not */
extern Expr *sift_store_table_qual(const SiftStoreSchema *t, Expr *clause);

#endif							/* SIFT_CORE_STORE_TABLE_QUAL_H */
