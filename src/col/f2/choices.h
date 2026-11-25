/*
 * col/f2/choices.h - sift.index_choices(index) for a sift_col index or a sift_table of format major 2: what the build and the maintenance chose, read back from the file;
 * sift_f2_relation is how the SQL functions of col/am/sql_dispatch.c tell such a relation from a Format 1 one
 */
#ifndef SIFT_COL_F2_CHOICES_H
#define SIFT_COL_F2_CHOICES_H

#include "postgres.h"

#include "fmgr.h"

/* The relation is a sift_col index or a sift_table of format major 2 (false for any other relation: Format 1 and the error paths answer it) */
extern bool sift_f2_relation(Oid relid);

extern Datum sift_f2_index_choices(PG_FUNCTION_ARGS);

/* sift.col_sections of a relation of format major 2 (col/f2/inspect.c) */
extern Datum sift_f2_sections(PG_FUNCTION_ARGS);

#endif
