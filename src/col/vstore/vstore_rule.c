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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/build.h"
#include "col/keycols/keycols_int.h"
#include "col/pg.h"
#include "col/vstore/vstore.h"

#define VSTORE_MIN_BYTES	5.0		/* bytes a row that sharing spares, at the least: a short or repeated string costs about as much as its reference */
#define VSTORE_MIN_SAVED	0.25	/* the share of the bytes of the leaf (strings, references and codes) that sharing spares, at the least */
#define VSTORE_ENTRY_BYTES	4.0		/* the bytes a string takes in a dictionary beyond its width: its offset (the order array comes with the form chosen) */

/*
 * The order of the units of an index: the leaves of its key_cut and its key_order, as the build resolved them. Two indexes of one table with the same order meet the
 * rows of a column in the same sequence, so the codes the first of them gave to its strings (in the order it met them) are the order of the second's own, and
 * the references that the second's segments hold climb with its rows. Indexes that differ keep their own strings (docs/COL-VSTORE.md)
 */
char *
sift_vstore_order(Relation index)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	const char *ko = sift_col_options_key_order(index);
	StringInfoData sig;

	initStringInfo(&sig);
	for (int i = 0; sift_col_key_cut(r) && i < r->ncut; i++)
		appendStringInfo(&sig, "%s%s", i ? ", " : "key_cut = ", keycols_leaf_name(index, &r->schema->cols[r->cut[i]]));
	if (strncmp(ko, "auto:", 5) == 0)
		ko += 5;
	for (; *ko == ' '; ko++)
		;
	if (*ko != '\0' && strcmp(ko, "off") != 0)
		appendStringInfo(&sig, "%skey_order = %s", sig.len ? "; " : "", ko);
	if (sig.len == 0)
		appendStringInfoString(&sig, "table order");
	return sig.data;
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

void
sift_vstore_verdict(Relation heap, Relation index, const SiftColDef *d, SiftVStoreVerdict *v)
{
	AttrNumber	hatt = sift_vstore_leaf_attno(index, d);
	double		nd,
				corr,
				rows = keycols_heap_rows(heap),
				per_row,
				share;
	int32		width;
	char	   *mine = NULL;

	*v = (SiftVStoreVerdict) {.state = SIFT_VSTORE_NONE};
	if (hatt == InvalidAttrNumber || sift_col_list_has(sift_col_options_count_columns(index), keycols_leaf_name(index, d)))
		return;
	/*
	 * pg_upgrade builds the index on an empty table and then puts the files of the old index in place: what the old index held is in those files, and
	 * the dependency on the store of its column has to be made here. An index of another key order that the old cluster kept apart depends on the store too
	 * and the store outlives the members that read it until it is dropped, which costs nothing else.
	 */
	if (IsBinaryUpgrade)
	{
		if (sift_vstore_exists(heap, hatt))
			v->state = SIFT_VSTORE_SHARED;
		return;
	}
	if (!keycols_col_stats(heap, hatt, &nd, &corr, &width))
		return;
	saving(rows, nd, width, &per_row, &share);
	if (per_row < VSTORE_MIN_BYTES || share <= VSTORE_MIN_SAVED)
		return;
	foreach_oid(oid, RelationGetIndexList(heap))
	{
		Relation	o;
		bool		holds = false;

		if (oid == RelationGetRelid(index))
			continue;
		o = index_open(oid, AccessShareLock);
		if (o->rd_rel->relam == index->rd_rel->relam && o->rd_index->indisvalid)
			for (int i = 0; !holds && i < IndexRelationGetNumberOfKeyAttributes(o); i++)
				holds = o->rd_index->indkey.values[i] == hatt;
		if (holds)
		{
			char	   *theirs = sift_vstore_order(o);

			if (mine == NULL)
				mine = sift_vstore_order(index);
			if (strcmp(mine, theirs) == 0)
			{
				v->state = SIFT_VSTORE_SHARED;
				v->other = pstrdup(RelationGetRelationName(o));
				v->other_order = theirs;
				index_close(o, AccessShareLock);
				return;
			}
			if (v->state == SIFT_VSTORE_NONE)
			{
				v->state = SIFT_VSTORE_OTHER_ORDER;
				v->other = pstrdup(RelationGetRelationName(o));
				v->other_order = theirs;
				v->duplicate_bytes = nd * (width + VSTORE_ENTRY_BYTES);
				v->saving_bytes = per_row * rows;
			}
		}
		index_close(o, AccessShareLock);
	}
	v->order = mine;
}

bool
sift_vstore_pays(Relation heap, Relation index, const SiftColDef *d)
{
	SiftVStoreVerdict v;

	sift_vstore_verdict(heap, index, d, &v);
	return v.state == SIFT_VSTORE_SHARED;
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

	if (hatt == InvalidAttrNumber || sift_test_vstore_columns == NULL || sift_test_vstore_columns[0] == '\0' || sift_test_vstore_columns[0] == '-')
		return false;
	name = get_attname(RelationGetRelid(heap), hatt, false);
	SplitIdentifierString(pstrdup(sift_test_vstore_columns), ',', &cols);
	foreach_ptr(char, col, cols)
		forced |= strcmp(col, "*") == 0 || strcmp(col, name) == 0;
	return forced;
}
#endif

/* Whether another sift_col index of the table holds the column with the order of this one: the strings can be shared with it */
static bool
has_peer(Relation heap, Relation index, AttrNumber hatt)
{
	char	   *mine = NULL;
	bool		found = false;

	foreach_oid(oid, RelationGetIndexList(heap))
	{
		Relation	o;
		bool		holds = false;

		if (oid == RelationGetRelid(index))
			continue;
		o = index_open(oid, AccessShareLock);
		if (o->rd_rel->relam == index->rd_rel->relam && o->rd_index->indisvalid)
			for (int i = 0; !holds && i < IndexRelationGetNumberOfKeyAttributes(o); i++)
				holds = o->rd_index->indkey.values[i] == hatt;
		if (holds)
		{
			if (mine == NULL)
				mine = sift_vstore_order(index);
			found = strcmp(mine, sift_vstore_order(o)) == 0;
		}
		index_close(o, AccessShareLock);
		if (found)
			break;
	}
	return found;
}

/*
 * Whether the leaf names a store that nothing else shares: the index depends on the store of its column and no other index of the table holds the column with its
 * order (the others were dropped, or rebuilt with another order). Its strings go back into its segments as they are written, the maintenance rewrites the segments
 * that name the store, and the store goes when none does (sift_vstore_vacuum)
 */
bool
sift_vstore_leaf_left(Relation heap, Relation index, const SiftColDef *d)
{
	AttrNumber	hatt = sift_vstore_leaf_attno(index, d);

#ifdef SIFT_TESTING
	if (hatt != InvalidAttrNumber && test_forced(heap, index, d))
		return false;
#endif
	return hatt != InvalidAttrNumber && !has_peer(heap, index, hatt);
}

/* The leaves of the schema that are left (see above) and that the index still depends on a store for; NULL when there is none */
bool *
sift_vstore_left_leaves(Relation index, const SiftColSchema *schema)
{
	Relation	heap = NULL;
	bool	   *left = NULL;

	if (index->rd_rel->relkind != RELKIND_INDEX)
		return NULL;
	for (int c = 0; c < schema->ncols; c++)
	{
		if (!OidIsValid(sift_vstore_leaf_store(index, &schema->cols[c])))
			continue;
		if (heap == NULL)
			heap = table_open(index->rd_index->indrelid, AccessShareLock);
		if (sift_vstore_leaf_left(heap, index, &schema->cols[c]))
		{
			left = left ? left : palloc0(schema->ncols * sizeof(bool));
			left[c] = true;
		}
	}
	if (heap != NULL)
		table_close(heap, NoLock);
	return left;
}

/* The build of an index: the leaves that sharing pays for get their store */
void
sift_vstore_auto(Relation heap, Relation index)
{
	const SiftColSchema *schema;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return;
#ifdef SIFT_TESTING
	if (sift_test_vstore_columns != NULL && strcmp(sift_test_vstore_columns, "-") == 0)	/* "-": nothing is shared (the baseline of a measurement) */
		return;
#endif
	schema = sift_col_rel(index)->schema;
	for (int c = 0; c < schema->ncols; c++)
	{
		const SiftColDef *d = &schema->cols[c];
		AttrNumber	hatt = sift_vstore_leaf_attno(index, d);
		bool		share = sift_vstore_pays(heap, index, d);

#ifdef SIFT_TESTING
		share |= test_forced(heap, index, d);
#endif
		if (share)
			(void) sift_vstore_ensure(heap, index, hatt);
		else if (hatt != InvalidAttrNumber)
			sift_vstore_release(heap, index, hatt);	/* a REINDEX of a member whose order is no longer another's: it holds its strings again */
	}
}

PG_FUNCTION_INFO_V1(sift_col_value_store_members);

/*
 * sift.col_value_store_members(): for each sift_col index the user can read, each string column of it that a shared value store serves or could serve:
 * 'shared' (its segments refer to the store), 'own: different key order' (another index of the table holds the column with another key order, so this one stores
 * it again) or 'own: not yet rebuilt' (another index has the same order, a REINDEX shares it)
 */
Datum
sift_col_value_store_members(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	cls = table_open(RelationRelationId, AccessShareLock);
	TableScanDesc scan;
	HeapTuple	tup;

	InitMaterializedSRF(fcinfo, 0);
	scan = table_beginscan_catalog(cls, 0, NULL);
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);
		Relation	index,
					heap;

		if (c->relkind != RELKIND_INDEX || c->relam != sift_col_am_oid())
			continue;
		index = index_open(c->oid, AccessShareLock);
		heap = table_open(index->rd_index->indrelid, AccessShareLock);
		if (pg_class_aclcheck(RelationGetRelid(heap), GetUserId(), ACL_SELECT) == ACLCHECK_OK)
		{
			const SiftColSchema *schema = sift_col_rel(index)->schema;

			for (int l = 0; l < schema->ncols; l++)
			{
				const SiftColDef *d = &schema->cols[l];
				AttrNumber	hatt = sift_vstore_leaf_attno(index, d);
				SiftVStoreVerdict v;
				const char *state;
				Datum		vals[6] = {0};
				bool		nulls[6] = {0};

				if (hatt == InvalidAttrNumber)
					continue;
				if (OidIsValid(sift_vstore_leaf_store(index, d)))
					state = "shared";
				else
				{
					sift_vstore_verdict(heap, index, d, &v);
					if (v.state == SIFT_VSTORE_SHARED)
						state = "own: not yet rebuilt";
					else if (v.state == SIFT_VSTORE_OTHER_ORDER)
						state = "own: different key order";
					else
						continue;
				}
				vals[0] = ObjectIdGetDatum(RelationGetRelid(index));
				vals[1] = ObjectIdGetDatum(RelationGetRelid(heap));
				vals[2] = CStringGetTextDatum(get_attname(RelationGetRelid(heap), hatt, false));
				vals[3] = CStringGetTextDatum(state);
				vals[4] = CStringGetTextDatum(sift_vstore_order(index));
				nulls[5] = true;
				if (strcmp(state, "own: different key order") == 0)
				{
					vals[5] = Float8GetDatum(v.duplicate_bytes);
					nulls[5] = false;
				}
				tuplestore_putvalues(rsi->setResult, rsi->setDesc, vals, nulls);
			}
		}
		table_close(heap, AccessShareLock);
		index_close(index, AccessShareLock);
	}
	table_endscan(scan);
	table_close(cls, AccessShareLock);
	return (Datum) 0;
}

/* texts[0]: the string columns the segments of the index refer to the store for; texts[1]: those it stores again because another index holds them
 * with another order; NULL for none */
void
sift_vstore_choice_texts(Relation heap, Relation index, const SiftColIndexMeta *defs, char **texts)
{
	StringInfoData sh,
				ot;

	initStringInfo(&sh);
	initStringInfo(&ot);
	for (int c = 0; c < defs->ncols; c++)
	{
		const SiftColDef *def = &defs->cols[c];
		const char *col;
		SiftVStoreVerdict v;

		if (sift_vstore_leaf_attno(index, def) == InvalidAttrNumber)
			continue;
		col = NameStr(TupleDescAttr(RelationGetDescr(index), def->attno - 1)->attname);
		if (OidIsValid(sift_vstore_leaf_store(index, def)))
			appendStringInfo(&sh, "%s%s", sh.len ? ", " : "", col);
		else
		{
			sift_vstore_verdict(heap, index, def, &v);
			if (v.state == SIFT_VSTORE_OTHER_ORDER)
				appendStringInfo(&ot, "%s%s: %.1f MB again (%.1f MB less)", ot.len ? "; " : "", col, v.duplicate_bytes / (1024 * 1024),
								 v.saving_bytes / (1024 * 1024));
		}
	}
	texts[0] = sh.len > 0 ? sh.data : NULL;
	texts[1] = ot.len > 0 ? ot.data : NULL;
}
