/*
 * col/vstore/vstore_rule.c - when the strings of a column are shared (docs/COL-VSTORE.md)
 *
 * A string leaf of an index is shared when the column is a plain attribute of the table, another sift_col index of the table (or an existing store)
 * already holds its strings, and the shared store spares the leaf more than it costs: the strings the segments of the index would hold again, less the
 * references that replace them, are at least VSTORE_MIN_BYTES a row and more than VSTORE_MIN_SAVED of the bytes of the leaf. The numbers come from the
 * statistics of the column (distinct values and width), the same ones the other automatic choices of the build read. A leaf that counts the rows of its
 * strings (option count_columns) is never shared: the reference holds no counts.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/build.h"
#include "col/keycols/keycols_int.h"
#include "col/vstore/vstore.h"

#define VSTORE_MIN_BYTES	5.0		/* bytes a row that sharing spares, at the least: a short or repeated string costs about as much as its reference */
#define VSTORE_MIN_SAVED	0.25	/* the share of the bytes of the leaf (strings, references and codes) that sharing spares, at the least */
#define VSTORE_ENTRY_BYTES	4.0		/* the bytes a string takes in a dictionary beyond its width: its offset (the order array comes with the form chosen) */

/* Whether another sift_col index of the table has a plain string leaf of the column */
static bool
other_member(Relation heap, Relation index, AttrNumber hatt)
{
	bool		found = false;

	foreach_oid(oid, RelationGetIndexList(heap))
	{
		Relation	o;

		if (oid == RelationGetRelid(index))
			continue;
		o = index_open(oid, AccessShareLock);
		if (o->rd_rel->relam == index->rd_rel->relam && o->rd_index->indisvalid)
			for (int i = 0; !found && i < IndexRelationGetNumberOfKeyAttributes(o); i++)
				found = o->rd_index->indkey.values[i] == hatt;
		index_close(o, AccessShareLock);
		if (found)
			break;
	}
	return found;
}

/* The bytes a row that a store spares the leaf, and the share of the leaf's bytes, of a column of nd distinct strings of width bytes in rows rows */
static void
saving(double rows, double nd, double width, double *per_row, double *share)
{
	double		bits = Max(ceil(log2(Max(nd, 2))), 1),
				strings = nd * (width + VSTORE_ENTRY_BYTES),
				refs = nd * bits / 8,
				codes = rows * bits / 8;

	*per_row = (strings - refs) / Max(rows, 1);
	*share = (strings - refs) / (strings + codes);
}

bool
sift_vstore_pays(Relation heap, Relation index, const SiftColDef *d)
{
	AttrNumber	hatt = sift_vstore_leaf_attno(index, d);
	double		nd,
				corr,
				rows = keycols_heap_rows(heap),
				per_row,
				share;
	int32		width;

	if (hatt == InvalidAttrNumber || sift_col_list_has(sift_col_options_count_columns(index), keycols_leaf_name(index, d)))
		return false;
	if (!OidIsValid(sift_vstore_find(heap, hatt)) && !other_member(heap, index, hatt))
		return false;
	if (!keycols_col_stats(heap, hatt, &nd, &corr, &width))
		return false;
	saving(rows, nd, width, &per_row, &share);
	return per_row >= VSTORE_MIN_BYTES && share > VSTORE_MIN_SAVED;
}

#ifdef SIFT_TESTING
/* sift.test_vstore_columns ("*": every string column) names columns that are shared whatever the rule says */
static bool
test_forced(Relation heap, Relation index, const SiftColDef *d)
{
	List	   *cols = NIL;
	AttrNumber	hatt = sift_vstore_leaf_attno(index, d);
	const char *name;
	bool		forced = false;

	if (hatt == InvalidAttrNumber || sift_test_vstore_columns == NULL || sift_test_vstore_columns[0] == '\0')
		return false;
	name = get_attname(RelationGetRelid(heap), hatt, false);
	SplitIdentifierString(pstrdup(sift_test_vstore_columns), ',', &cols);
	foreach_ptr(char, col, cols)
		forced |= strcmp(col, "*") == 0 || strcmp(col, name) == 0;
	return forced;
}
#endif

/* The build of an index: the leaves that sharing pays for get their store */
void
sift_vstore_auto(Relation heap, Relation index)
{
	const SiftColSchema *schema;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return;
	schema = sift_col_rel(index)->schema;
	for (int c = 0; c < schema->ncols; c++)
		if (sift_vstore_pays(heap, index, &schema->cols[c])
#ifdef SIFT_TESTING
			|| test_forced(heap, index, &schema->cols[c])
#endif
			)
			(void) sift_vstore_ensure(heap, index, sift_vstore_leaf_attno(index, &schema->cols[c]));
}
