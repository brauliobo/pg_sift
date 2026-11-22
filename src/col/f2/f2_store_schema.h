/*
 * col/f2/f2_store_schema.h - what a SiftF2Store of a sift_table holds beyond its leaves, so that every consumer of the store reads the same fields for a table as for an index: the schema of
 * the leaves, the declarations of the options (compression, scope, sketches, skip_index, paths), and the default operator of each column. Also the one conversion of an exact number
 * numeric(p <= 18, s) to a scaled integer cell and back, which both the writer and the reader of a table use.
 *
 * A numeric(p <= 18, s) is a column of SIFT_CELL_INT with the hint SIFT_CHINT_NUM64 and the scale s, as in a sift_col index: the aggregates, the zone maps and the sketches see the number, not an image.
 * A value no scaled integer can hold (NaN) is a null cell with the tag SIFT_COLTAG_UNSURE and the datum as its payload, kept in the exceptions of the chunk.
 */
#ifndef SIFT_F2_STORE_SCHEMA_H
#define SIFT_F2_STORE_SCHEMA_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "utils/relcache.h"

#include "col/codec.h"
#include "col/f2/f2_kll.h"
#include "col/f2/f2_store.h"
#include "col/scope.h"

struct SiftCompressPolicy
{
	SiftCcPolicy *pol;			/* [ncols], the compression of each leaf */
};

struct SiftScopeDecl
{
	const SiftScopeSpec *spec;
	int			member_leaf;	/* the leaf of the column that holds the member key */
	int			time_leaf;		/* the leaf the units are cut by: the first date or timestamp column, -1: none */
};

struct SiftSketchPolicy
{
	SiftF2KllCols kll;			/* the leaves of the kll groups, ascending */
};

struct SiftSkipPolicy
{
	const char *spec;			/* the option as written: the columns a granule keeps a bloom filter or a trigram set of */
};

struct SiftColShred
{
	const char *spec;			/* the option paths as written */
	bool		automatic;
};

/* The scale s of a numeric(p <= 18, s) attribute (a column of a table keeps it as a scaled integer); false for every other attribute, and for a numeric of no precision */
extern bool sift_f2_attr_num64(Form_pg_attribute att, uint8 *scale);

/* The cell of a numeric datum of an attribute that is NUM64: the scaled integer, or the null cell of a NaN that keeps the datum as its payload */
extern void sift_f2_num64_cell(Datum d, uint8 scale, MemoryContext row_cx, SiftCell *c);

/* The numeric of a scaled integer, and the one of an exception payload (a copy in cx, aligned) */
extern Datum sift_f2_num64_datum(int64 key, uint8 scale);
extern Datum sift_f2_num64_exception_datum(const char *payload, uint32 len, MemoryContext cx);

/* Whether a granule of the table can hold a NaN in a column of the attribute, so that its reader must read the tags of the column */
static inline bool
sift_f2_attr_may_have_exceptions(Form_pg_attribute att)
{
	return att->atttypid == NUMERICOID;
}

/* The leaves of attribute att of a table store: 0 for a dropped column, 1 for a scalar, more for a column the store shreds */
extern int	sift_f2_store_attr_leaves(const SiftF2Store *store, int att);

/* Fills schema, the declarations of the options, spec.kll and the operator cache of a table store whose leaves, kinds, hints and scales are defined; the options are those of store->opts */
extern void sift_f2_store_hydrate(Relation rel, SiftF2Store *store);

#endif							/* SIFT_F2_STORE_SCHEMA_H */
