/*
 * col/v1/keycols/keycols.c - the default of the option key_columns: an index created without it, or with key_columns = 'auto', keeps the keys of its
 * foreign key columns and of its columns named *_id, as far as they are int2, int4, int8 or date, not unique, and not held by the cube. CREATE INDEX is the only
 * place it is decided: the columns are written to the option as 'auto: a, b', so that a rebuild keeps them, and the build keeps those that hold a key list or
 * a bitmap in half of the units or more. The same file decides the other defaults that read the columns' data before a build: the jsonb paths and the plain
 * columns that take the dimensions of the cube when there are more than it holds.
 */
#include "postgres.h"

#include <math.h>


#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_statistic.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "core/maint/maint.h"
#include "col/am.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/keycols/int.h"
#include "core/restrict.h"
#include "col/v1/ktab.h"
#include "col/v1/setsum.h"

static bool
ends_in_id(const char *name)
{
	size_t		n = strlen(name);

	return n > 3 && strcmp(name + n - 3, "_id") == 0;
}

/* The column is an entity the rows belong to, the one a query filters by: a foreign key or a column named *_id */
bool
keycols_is_entity(Relation heap, AttrNumber hatt, const char *name)
{
	return sift_attr_is_fk(heap, hatt) || ends_in_id(name);
}

/*
 * The b-tree of the table that the shape of the second order already runs on: WHERE col = x ORDER BY key, a non-unique b-tree whose first two key columns are col and a
 * date or timestamp column of the index (the key of its order arrays), in either order (a partial one too: a predicate narrows the rows, not the shape). A key column that
 * only INCLUDEs the second is no order, and a unique b-tree is there for its constraint. Returns its oid, InvalidOid where there is none.
 */
Oid
keycols_group_btree(Relation heap, Relation index, AttrNumber hatt)
{
	const SiftColRel *rel = sift_col_rel_private(index, CurrentMemoryContext);
	Bitmapset  *orders = NULL;
	Oid			found = InvalidOid;
	ListCell   *lc;

	for (int c = 0; c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	att = keycols_heap_attno(index, d);

		if (d->parent < 0 && att != 0 && sift_cgord_leaf_ok(rel->schema, d) && sift_cgord_leaf_time(d))
			orders = bms_add_member(orders, att);
	}
	foreach(lc, RelationGetIndexList(heap))
	{
		HeapTuple	t = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index ix;

		if (!HeapTupleIsValid(t))
			continue;
		ix = (Form_pg_index) GETSTRUCT(t);
		if (!OidIsValid(found) && ix->indisvalid && !ix->indisunique && ix->indnkeyatts >= 2 && get_rel_relam(ix->indexrelid) == BTREE_AM_OID &&
			((ix->indkey.values[0] == hatt && bms_is_member(ix->indkey.values[1], orders)) || (ix->indkey.values[1] == hatt && bms_is_member(ix->indkey.values[0], orders))))
			found = ix->indexrelid;
		ReleaseSysCache(t);
	}
	bms_free(orders);
	return found;
}

/* Does the option list hold name = value (value NULL: no name)? */
static bool
option_holds(Datum options, const char *name, const char *value)
{
	ListCell   *lc;

	foreach(lc, untransformRelOptions(options))
	{
		DefElem    *d = lfirst(lc);

		if (strcmp(d->defname, name) == 0)
			return value != NULL && strcmp(defGetString(d), value) == 0;
	}
	return value == NULL;
}

/*
 * The options of index as defs (DefElem, a value each) written in one update of its catalog row, or reset (the names of defs): visible to the relcache entry at once. It is a catalog update (and
 * moves the options to the end of the list), so a choice that stands is not written again: a decision that the data repeats leaves the row as it is. The options are a set that must be valid
 * together (the column lists of a renamed column), which is why they go in one write.
 */
static void
write_options(Relation index, List *defs, bool reset, bool validate)
{
	Relation	pgclass = table_open(RelationRelationId, RowExclusiveLock);
	HeapTuple	tuple,
				newtuple;
	ItemPointerData otid;
	Datum		old,
				newopts,
				values[Natts_pg_class] = {0};
	bool		isnull,
				nulls[Natts_pg_class] = {false},
				repl[Natts_pg_class] = {false};
	List	   *todo = NIL;

	sift_catalog_write_guard("an index option");
	CommandCounterIncrement();
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(index)));
	SIFT_INVARIANT(HeapTupleIsValid(tuple), "sift_col: no pg_class row for index %u", RelationGetRelid(index));
	/*
	 * The lock an update of an index's pg_class row expects (checked in assert builds) is the tuple lock of the row, not a lock on its table: ShareUpdateExclusiveLock on the table waits for
	 * the ShareLock of every other build of an index of it, and two builds that wait for each other are a deadlock (a parallel restore).
	 */
	otid = tuple->t_self;
	LockTuple(pgclass, &otid, InplaceUpdateTupleLock);
	old = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	foreach_node(DefElem, def, defs)
		if (!option_holds(isnull ? (Datum) 0 : old, def->defname, reset ? NULL : defGetString(def)))
			todo = lappend(todo, def);
	if (todo == NIL)
	{
		UnlockTuple(pgclass, &otid, InplaceUpdateTupleLock);
		ReleaseSysCache(tuple);
		table_close(pgclass, RowExclusiveLock);
		return;
	}
	newopts = transformRelOptions(isnull ? (Datum) 0 : old, todo, NULL, NULL, false, reset);
	if (validate)
		(void) index_reloptions(index->rd_indam->amoptions, newopts, true);
	values[Anum_pg_class_reloptions - 1] = newopts;
	nulls[Anum_pg_class_reloptions - 1] = newopts == (Datum) 0;
	repl[Anum_pg_class_reloptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass), values, nulls, repl);
	sift_cat_update(pgclass, newtuple);
	UnlockTuple(pgclass, &otid, InplaceUpdateTupleLock);
	heap_freetuple(newtuple);
	ReleaseSysCache(tuple);
	table_close(pgclass, RowExclusiveLock);
	CommandCounterIncrement();
	sift_maint_options_written(index);
}

/* The option name of index set to value (NULL: reset) */
void
sift_col_set_option(Relation index, const char *name, const char *value)
{
	write_options(index, list_make1(makeDefElem((char *) name, value ? (Node *) makeString(pstrdup(value)) : NULL, -1)), value == NULL, true);
}

/* The options of defs (DefElem, a string value each) of index set together, as they are: the caller checks the index after, which another change that it makes goes with these */
void
sift_col_set_options(Relation index, List *defs)
{
	write_options(index, defs, false, false);
}

void
keycols_append_name(StringInfo s, const char *name)
{
	if (s->len > 0)
		appendStringInfoString(s, ", ");
	appendStringInfoString(s, name);
}

const char *
keycols_leaf_name(Relation index, const SiftColDef *d)
{
	return NameStr(TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attname);
}

/* the heap column of a leaf (0: an expression, or a path of a document, which has no statistics or constraint of its own) */
AttrNumber
keycols_heap_attno(Relation index, const SiftColDef *d)
{
	return d->path[0] != '\0' ? 0 : index->rd_index->indkey.values[d->attno - 1];
}

bool
sift_col_leaf_unique(Relation heap, Relation index, const SiftColDef *d)
{
	AttrNumber	hatt = keycols_heap_attno(index, d);

	return hatt != 0 && sift_attr_is_unique(heap, hatt);
}

/*
 * A key the point directory could list (col/v1/keycols/point.c): an integer of nearly unique values (POINT_AUTO_UNIQUE of the rows) that no unique b-tree of the table takes for the
 * equality. Its correlation with the heap order comes back, which tells the directory from the order array (gorder.c).
 */
bool
keycols_point_key(Relation heap, Relation index, const SiftColSchema *schema, const SiftColDef *d, double *corr)
{
	AttrNumber	hatt = keycols_heap_attno(index, d);
	double		distinct;
	int32		width;
	Oid			type = hatt == 0 ? InvalidOid : TupleDescAttr(RelationGetDescr(index), d->attno - 1)->atttypid;

	if (hatt == 0 || d->parent >= 0 || d->store != SIFT_COLS_INT || d->lane != 0 || type == DATEOID || type == TIMESTAMPOID || type == TIMESTAMPTZOID ||
		!sift_cset_leaf_ok(schema, d) || strchr(keycols_leaf_name(index, d), ',') != NULL)
		return false;
	return keycols_col_stats(heap, hatt, &distinct, corr, &width) && distinct >= POINT_AUTO_UNIQUE * keycols_heap_rows(heap) && !sift_attr_is_unique(heap, hatt);
}

bool
sift_col_key_attr(Relation heap, AttrNumber hatt, const SiftColSchema *schema, const SiftColDef *d)
{
	const char *name = NameStr(TupleDescAttr(RelationGetDescr(heap), hatt - 1)->attname);

	/* the option is a list split at commas, and its names are not quoted */
	return sift_cset_pres_leaf_ok(schema, d) && strchr(name, ',') == NULL && keycols_is_entity(heap, hatt, name) && !sift_attr_is_unique(heap, hatt);
}

bool
sift_col_key_shaped(Relation heap, Relation index, const SiftColSchema *schema, const SiftColDef *d)
{
	AttrNumber	hatt = keycols_heap_attno(index, d);

	return hatt != 0 && sift_col_key_attr(heap, hatt, schema, d);
}

/*
 * The scope key of a table shaped like a denormalised copy (CREATE TABLE AS of a join with a table of (member, key) pairs sorted by the key): the first column of the
 * index is an integer of few values (GORD_KEYORDER_ENTITIES to SCOPE_KEY_DISTINCT) with many rows each (SCOPE_KEY_DEPTH), neither a foreign key nor unique, and another key-shaped column
 * has SCOPE_KEY_RATIO times its values or more (unrelated small keys do not make a scope): the column the rows were copied by, which the build would take for the group of key_order and the key of key_cut. The lookups of such a table are by its first
 * column. NULL where the shape does not hold, where key_order is the user's, or where the heap is in the order of the other column (it serves that key already).
 * build: the table is analyzed first where the choice of the arrays would analyze it (a CREATE TABLE AS copy is indexed before autovacuum has seen it).
 */
const char *
sift_col_scope_key(Relation heap, Relation index, bool build)
{
	const SiftColRel *rel;
	const SiftColDef *lead = NULL;
	AttrNumber	hatt;
	double		nd,
				corr,
				rows;
	int32		width;
	bool		other = false;

	if ((sift_col_options_key_order(index)[0] != '\0' && !sift_col_options_key_order_auto(index)))
		return NULL;
	if (build && keycols_arrays_open(index))
		keycols_analyze_if_unknown(heap, index);
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	for (int c = 0; c < rel->schema->ncols && lead == NULL; c++)
		if (rel->schema->cols[c].attno == 1)
			lead = &rel->schema->cols[c];
	hatt = lead == NULL ? 0 : keycols_heap_attno(index, lead);
	rows = keycols_heap_rows(heap);
	if (hatt == 0 || lead->parent >= 0 || lead->lane != 0 || lead->store != SIFT_COLS_INT || sift_cgord_leaf_time(lead) || !sift_cset_pres_leaf_ok(rel->schema, lead) ||
		sift_attr_is_fk(heap, hatt) || sift_attr_is_unique(heap, hatt) || !keycols_col_stats(heap, hatt, &nd, &corr, &width) ||
		nd < GORD_KEYORDER_ENTITIES || nd > SCOPE_KEY_DISTINCT || rows / nd < SCOPE_KEY_DEPTH)
		return NULL;
	for (int c = 0; c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	att = keycols_heap_attno(index, d);
		double		nd2,
					corr2;

		if (d == lead || att == 0 || d->parent >= 0 || !sift_col_key_attr(heap, att, rel->schema, d) || !keycols_col_stats(heap, att, &nd2, &corr2, &width) || nd2 < SCOPE_KEY_RATIO * nd)
			continue;
		if (corr2 >= GORD_AUTO_CORR && corr < GORD_AUTO_CORR)
			return NULL;
		other = true;
	}
	return other ? pstrdup(keycols_leaf_name(index, lead)) : NULL;
}

/*
 * A column that is unique alone (the id) is not a candidate for the count(DISTINCT) that the other key lists serve, but an equality on it is the lookup a b-tree
 * answers: a list of its keys in a unit skips the units whose zone holds the key, and it fits the share of the unit only as runs of consecutive keys (the
 * ids that a batch inserted). It is worth a list where the zones cannot skip the units: the units are cut by a key, or the column does not follow the order
 * of the heap (its correlation in pg_statistic is under 0.9); the report after the build keeps it only where the lists fit.
 */
static bool
unique_needs_list(Relation heap, Relation index, const SiftColRel *rel, const SiftColDef *d)
{
	AttrNumber	hatt = keycols_heap_attno(index, d);
	const char *name;
	HeapTuple	tp;
	bool		follows = true;
	AttStatsSlot sslot;

	if (hatt == 0 || !sift_cset_pres_leaf_ok(rel->schema, d) || !sift_attr_is_unique(heap, hatt))
		return false;
	name = NameStr(TupleDescAttr(RelationGetDescr(heap), hatt - 1)->attname);
	if (strchr(name, ',') != NULL)
		return false;
	if (sift_col_key_cut(rel))
		return true;
	if ((tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false))) == NULL)
		return false;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		follows = sslot.nnumbers > 0 && fabs(sslot.numbers[0]) >= 0.9;
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return !follows;
}

/* 'auto: a, b'; 'auto' for none, which the next REINDEX decides again */
static void
set_chosen(Relation index, const StringInfoData *list)
{
	StringInfoData v;

	if (list->len == 0)
	{
		sift_col_set_option(index, "key_columns", "auto");
		return;
	}
	initStringInfo(&v);
	appendStringInfo(&v, "auto: %s", list->data);
	sift_col_set_option(index, "key_columns", v.data);
	pfree(v.data);
}

/*
 * Before a build: when the index has no key_columns (or 'auto') and sift.col_format_minor lets minor 15 be written, the columns that qualify are written to
 * the option, and the build (and its workers) see them. Returns whether the build's choice is in force, for sift_col_auto_keys_report: the columns were
 * chosen here, or an earlier build chose them and this one keeps them.
 */
bool
sift_col_auto_keys(Relation heap, Relation index)
{
	const SiftColRel *rel;
	StringInfoData list;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return false;
	rel = sift_col_rel_private(index, CurrentMemoryContext);	/* the loop reads catalogs, which reload the cached state */
	if (sift_col_options_key_columns(index) != NULL && !rel->pagg.keyauto)
		return false;
	for (int c = 0; c < rel->schema->ncols; c++)
		if (rel->pagg.set[c])
			return true;
	initStringInfo(&list);
	for (int c = 0; c < rel->schema->ncols; c++)
		if (sift_col_key_shaped(heap, index, rel->schema, &rel->schema->cols[c]) || unique_needs_list(heap, index, rel, &rel->schema->cols[c]))
			keycols_append_name(&list, keycols_leaf_name(index, &rel->schema->cols[c]));
	if (list.len == 0)
		return false;
	set_chosen(index, &list);
	return true;
}

/* Whether the choice of sift_col_auto_redecide could be made but for the heap lock it does not wait for (someone else holds it): a step that needs the choice is left to the next call */
bool
sift_col_redecide_busy(Relation index)
{
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || !sift_catalog_write_ok())
		return false;
	return !ConditionalLockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock);
}

/*
 * Before a step of the rewrite: what a build chooses from the data and keeps in an option it wrote itself (order_array_columns, point_tid_columns,
 * each 'auto' or 'auto: list') is chosen again, in the order of the build, so the segments about to be written are those a REINDEX would write. key_cut is never chosen
 * here: it is the user's. An option the user wrote is left as it is. The caller reads the schema again after it. It writes the catalog: a VACUUM does not
 * (sift_catalog_write_ok), and only an explicit rewrite chooses again.
 */
void
sift_col_auto_redecide(Relation index)
{
	Relation	heap;
	char	   *arr,
			   *pt;
	const char *o;
	bool		pushed = false;
	StringInfoData what;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || !sift_catalog_write_ok())
		return;
	/* the lock and the reasons of sift_col_auto_keys_merge: the merge holds the metapage, a VACUUM the heap */
	if (!ConditionalLockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock))
		return;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	o = sift_col_options_order_array_columns(index);
	arr = pstrdup(o ? o : "");
	pt = pstrdup(sift_col_options_point_tid_columns(index));
	sift_col_auto_gorder(heap, index, false);
	sift_col_auto_point(heap, index);
	if (pushed)
		PopActiveSnapshot();
	table_close(heap, NoLock);
	initStringInfo(&what);
	o = sift_col_options_order_array_columns(index);
	if (strcmp(arr, o ? o : "") != 0)
		appendStringInfo(&what, "order_array_columns = '%s' (was '%s'); ", o ? o : "", arr);
	if (strcmp(pt, sift_col_options_point_tid_columns(index)) != 0)
		appendStringInfo(&what, "point_tid_columns = '%s' (was '%s'); ", sift_col_options_point_tid_columns(index), pt);
	if (what.len == 0)
		return;
	what.data[what.len - 2] = '\0';
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": the rewrite chooses again from the data: %s", RelationGetRelationName(index), what.data),
			 errdetail("Rows appended since the build change what the defaults choose; an option you wrote is never changed."),
			 errhint("ALTER INDEX %s SET (point_tid_columns = 'off') keeps a choice off; the other options do the same.", sift_col_index_ref(index))));
}

/*
 * Before a merge of every immutable segment of an index whose key_columns is 'auto' (or absent): the choice of the build is made again from the segments about to
 * be rewritten. Churn puts new row versions in the tail of the heap, a unit then holds many keys, the cube no longer holds the column in the unit and a key list
 * would prune it; a REINDEX chose it, a merge kept the choice of the day of the build. A key-shaped column that the cube holds in under SIFT_COL_AUTO_ADD_PCT of
 * the units joins the option (the merge writes its lists, sift_col_auto_keys_report then drops it if they do not fit), so a column is added below 40% and dropped
 * from 50%: it does not alternate. Returns whether the option changed; the caller reads the index's schema again after it.
 */
bool
sift_col_auto_keys_merge(Relation index, const SiftSegEntry *in, int n)
{
	const SiftColRel *rel;
	Relation	heap;
	MemoryContext cx;
	uint32		units = 0,
				cubed[SIFT_COL_MAX_COLS] = {0};
	StringInfoData list;
	bool		added = false;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return false;
	/*
	 * A VACUUM does not write the catalog (sift_catalog_write_ok), and the leader of a parallel index vacuum is in parallel mode,
	 * where the write of the option cannot assign a transaction id (nor increment the command counter): the next merge decides.
	 */
	if (!sift_catalog_write_ok())
		return false;
	/*
	 * The merge holds the index metapage; a VACUUM holds the heap and waits for the metapage, so waiting for the heap here is a deadlock. The lock is the one
	 * that the write of the option takes, taken first and without waiting: when someone else holds it the choice is left to the next merge.
	 */
	if (!ConditionalLockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock))
		return false;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	if (sift_col_options_key_columns(index) != NULL && !rel->pagg.keyauto)
		return false;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col auto keys merge", ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < n; i++)
	{
		SiftColSegment *seg = sift_cseg_open(index, &in[i], cx);
		SiftCPaggCensus c;

		units += seg->nsb;
		sift_cseg_pre_census(seg, &rel->pagg, sift_col_format_minor, 0, &c);
		for (int d = 0; d < rel->pagg.ndims; d++)
			if (!rel->pagg.dim[d].cal)
				cubed[rel->pagg.dim[d].leaf] += c.dim[d];
		sift_cseg_close(seg);
		MemoryContextReset(cx);
	}
	MemoryContextDelete(cx);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	initStringInfo(&list);
	for (int c = 0; c < rel->schema->ncols; c++)
	{
		if (rel->pagg.set[c])
			keycols_append_name(&list, keycols_leaf_name(index, &rel->schema->cols[c]));
		else if (units > 0 && (uint64) cubed[c] * 100 < (uint64) units * SIFT_COL_AUTO_ADD_PCT && sift_col_key_shaped(heap, index, rel->schema, &rel->schema->cols[c]))
		{
			keycols_append_name(&list, keycols_leaf_name(index, &rel->schema->cols[c]));
			added = true;
		}
	}
	table_close(heap, NoLock);
	if (added)
		set_chosen(index, &list);
	pfree(list.data);
	return added;
}

/*
 * The spec of a rewrite of the segments in (a merge, sift.col_refresh, the rollup worker) of an index whose key_columns is 'auto' (or absent): the one the options
 * make, and a key list or bitmap for each leaf that a segment holds them of. The report after a build drops from the option a column the cube holds and keeps its lists
 * in the units written (sift_col_auto_keys_report), so a spec made from the option wrote the units of the rewrite without them: an equality on the key then read
 * every unit whose zone held it, a refresh after ALTER INDEX turned a segment of minor 22 into one of minor 14, and the planner priced the scan above a btree plan.
 * Nothing is written to the option (the build writes the lists again at every REINDEX, whatever the option says). An explicit key_columns, a leaf of hll_columns
 * and sift.col_format_minor under 15 write what the options say.
 */
void
sift_col_pagg_rewrite(Relation index, const SiftSegEntry *in, int n, SiftCPaggSpec *spec)
{
	const SiftColRel *rel = sift_col_rel_private(index, CurrentMemoryContext);
	MemoryContext cx;
	bool		kept = false;

	*spec = rel->pagg;
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || (sift_col_options_key_columns(index) != NULL && !spec->keyauto))
		return;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col rewrite keys", ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < n; i++)
	{
		SiftColSegment *seg = sift_cseg_open(index, &in[i], cx);

		for (int l = 0; l < rel->schema->ncols; l++)
			if (!spec->set[l] && !spec->hll[l] && sift_cset_pres_leaf_ok(rel->schema, &rel->schema->cols[l]) && sift_cseg_set_kinds(seg, l) != 0)
			{
				spec->set[l] = spec->pres[l] = true;
				spec->lookup[l] |= sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO;
				kept = true;
			}
		sift_cseg_close(seg);
		MemoryContextReset(cx);
	}
	MemoryContextDelete(cx);
	if (kept)
	{
		spec->keyauto = true;
		sift_ktab_spec(rel->schema, spec);
	}
}

/*
 * After a build that is the build's choice: the columns that hold a key list or a bitmap in SIFT_COL_AUTO_KEEP_PCT of the units stay in the option, the others go out
 * of it (the cube answers a column it holds in as many units; a column whose keys fit no share of a unit is scanned). The summary of the build says which stay.
 */
void
sift_col_auto_keys_report(Relation index)
{
	const SiftColRel *rel = sift_col_rel_private(index, CurrentMemoryContext);	/* the relcache frees the cached state on a reload, and the loop calls into readers */
	const SiftCPaggSpec *spec = &rel->pagg;
	SiftDirSnapshot *snap;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col auto keys", ALLOCSET_DEFAULT_SIZES);
	uint32		units = 0,
				have[SIFT_COL_MAX_COLS] = {0},
				cubed[SIFT_COL_MAX_COLS] = {0};
	StringInfoData kept,
				left;

	snap = sift_col_dir_read(index, false);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		SiftCPaggCensus c;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || (snap->segs[i].flags & SIFT_SEG_SCOPE))	/* the choices are of the rows of the entities */
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], cx);
		units += seg->nsb;
		sift_cseg_pre_census(seg, spec, sift_col_format_minor, 0, &c);
		for (int d = 0; d < spec->ndims; d++)
			if (!spec->dim[d].cal)
				cubed[spec->dim[d].leaf] += c.dim[d];
		for (int l = 0; l < seg->schema->ncols; l++)
		{
			if (!spec->pres[l])
				continue;
			for (uint32 u = 0; u < seg->nsb; u++)
			{
				uint32		len;
				const uint8 *blob = sift_cseg_set(seg, l, u, &len);

				if (blob != NULL && len > 0 && (blob[0] == SIFT_CSET_PRES || blob[0] == SIFT_CSET_PLIST || blob[0] == SIFT_CSET_RUNS || blob[0] == SIFT_CSET_BLOOM))
					have[l]++;
			}
		}
		sift_cseg_close(seg);
		MemoryContextReset(cx);
	}
	initStringInfo(&kept);
	initStringInfo(&left);
	for (int l = 0; l < rel->schema->ncols; l++)
	{
		if (!spec->pres[l])
			continue;
		if (units > 0 && ((uint64) cubed[l] * 100 >= (uint64) units * SIFT_COL_AUTO_KEEP_PCT || (uint64) have[l] * 100 < (uint64) units * SIFT_COL_AUTO_KEEP_PCT))
			keycols_append_name(&left, keycols_leaf_name(index, &rel->schema->cols[l]));
		else
			keycols_append_name(&kept, keycols_leaf_name(index, &rel->schema->cols[l]));
	}
	if (left.len > 0)
		set_chosen(index, &kept);
	MemoryContextDelete(cx);
	pfree(snap);
}


/*
 * Before a build: an index with jsonb columns and no paths option (a jsonb column needs paths, so that was an error), or with paths = 'auto', has its paths
 * decided here, once, from a sample of the documents (sift.suggest_paths, col/suggest.c): the spec is written to the option as 'auto: spec', so that the schema
 * of the build, of its workers and of every later REINDEX is the same (an 'auto: spec' of an earlier build is kept), and a NOTICE says what was read. When no
 * key qualifies nothing is written and the build refuses the jsonb column as before.
 */
void
sift_col_auto_paths(Relation heap, Relation index)
{
	const char *opt = pstrdup(sift_col_options_paths_raw(index));
	StringInfoData spec;
	int			ncols = 0,
				ngroups = 0;

	/* paths by hand, and the paths an earlier build chose, stay */
	if (opt[0] != '\0' && (!sift_col_paths_auto(opt) || opt[4] == ':'))
		return;
	initStringInfo(&spec);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		AttrNumber	hatt = index->rd_index->indkey.values[i];
		char	   *one;

		if (att->atttypid != JSONBOID || hatt == 0)
			continue;
		one = sift_col_suggest_spec(heap, hatt, NameStr(att->attname), SIFT_COL_AUTO_PATHS_SAMPLE, &ncols, &ngroups);
		if (one != NULL)
			appendStringInfo(&spec, "%s%s", spec.len ? ", " : "", one);
	}
	if (opt[0] == '\0' || opt[4] == '\0')
	{
		StringInfoData arrs,
					only,
					both;

		initStringInfo(&arrs);
		initStringInfo(&only);
		initStringInfo(&both);
		keycols_auto_array_specs(heap, index, &arrs, &only, &both);
		if (arrs.len > 0)
			appendStringInfo(&spec, "%s%s", spec.len ? ", " : "", arrs.data);
	}
	if (spec.len == 0)
		return;
	{
		StringInfoData v;

		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", spec.data);
		sift_col_set_option(index, "paths", v.data);
	}
}




