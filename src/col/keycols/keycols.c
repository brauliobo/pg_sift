/*
 * col/keycols/keycols.c - the default of the option key_columns: an index created without it, or with key_columns = 'auto', keeps the keys of its
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
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "bm25/maint.h"
#include "col/am.h"
#include "col/gorder.h"
#include "col/keycols/keycols_int.h"
#include "col/ktab.h"
#include "col/setsum.h"

static bool
is_fk_column(Relation heap, AttrNumber attno)
{
	ListCell   *lc;

	foreach(lc, RelationGetFKeyList(heap))
	{
		const ForeignKeyCacheInfo *fk = lfirst(lc);

		for (int i = 0; i < fk->nkeys; i++)
			if (fk->conkey[i] == attno)
				return true;
	}
	return false;
}

/* the column alone is unique (a primary key, a unique constraint or index of it, not a partial one): its count(DISTINCT) is its count */
static bool
is_unique_column(Relation heap, AttrNumber attno)
{
	bool		found = false;
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(heap))
	{
		HeapTuple	t = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index ix;

		if (!HeapTupleIsValid(t))
			continue;
		ix = (Form_pg_index) GETSTRUCT(t);
		found |= ix->indisunique && ix->indnatts == 1 && ix->indkey.values[0] == attno && heap_attisnull(t, Anum_pg_index_indpred, NULL);
		ReleaseSysCache(t);
	}
	return found;
}

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
	return is_fk_column(heap, hatt) || ends_in_id(name);
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

/* The option name of index set to value (NULL: reset); visible to the relcache entry at once */
void
sift_col_set_option(Relation index, const char *name, const char *value)
{
	Relation	pgclass = table_open(RelationRelationId, RowExclusiveLock);
	HeapTuple	tuple,
				newtuple;
	Datum		old,
				newopts,
				values[Natts_pg_class] = {0};
	bool		isnull,
				nulls[Natts_pg_class] = {false},
				repl[Natts_pg_class] = {false};
	List	   *def = list_make1(makeDefElem((char *) name, value ? (Node *) makeString(pstrdup(value)) : NULL, -1));

	/* the lock an update of an index's pg_class row expects on its table (checked in assert builds); the ShareLock of the build already excludes every other holder of it */
	LockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock);
	CommandCounterIncrement();
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(index)));
	SIFT_INVARIANT(HeapTupleIsValid(tuple), "sift_col: no pg_class row for index %u", RelationGetRelid(index));
	old = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	newopts = transformRelOptions(isnull ? (Datum) 0 : old, def, NULL, NULL, false, value == NULL);
	(void) index_reloptions(index->rd_indam->amoptions, newopts, true);
	values[Anum_pg_class_reloptions - 1] = newopts;
	nulls[Anum_pg_class_reloptions - 1] = newopts == (Datum) 0;
	repl[Anum_pg_class_reloptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass), values, nulls, repl);
	CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	ReleaseSysCache(tuple);
	table_close(pgclass, RowExclusiveLock);
	CommandCounterIncrement();
	sift_maint_options_written(index);
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

	return hatt != 0 && is_unique_column(heap, hatt);
}

/*
 * A key the point directory could list (col/keycols/point.c): an integer of nearly unique values (POINT_AUTO_UNIQUE of the rows) that no unique b-tree of the table takes for the
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
	return keycols_col_stats(heap, hatt, &distinct, corr, &width) && distinct >= POINT_AUTO_UNIQUE * keycols_heap_rows(heap) && !is_unique_column(heap, hatt);
}

bool
sift_col_key_attr(Relation heap, AttrNumber hatt, const SiftColSchema *schema, const SiftColDef *d)
{
	const char *name = NameStr(TupleDescAttr(RelationGetDescr(heap), hatt - 1)->attname);

	/* the option is a list split at commas, and its names are not quoted */
	return sift_cset_pres_leaf_ok(schema, d) && strchr(name, ',') == NULL && keycols_is_entity(heap, hatt, name) && !is_unique_column(heap, hatt);
}

bool
sift_col_key_shaped(Relation heap, Relation index, const SiftColSchema *schema, const SiftColDef *d)
{
	AttrNumber	hatt = keycols_heap_attno(index, d);

	return hatt != 0 && sift_col_key_attr(heap, hatt, schema, d);
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

	if (hatt == 0 || !sift_cset_pres_leaf_ok(rel->schema, d) || !is_unique_column(heap, hatt))
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

/* key_cut as the option holds it ("auto: d"), not as the reader returns it (" d") */
static char *
auto_cut_text(Relation index)
{
	const char *c = sift_col_options_key_cut(index);

	while (*c == ' ')
		c++;
	return *c && sift_col_options_key_cut_auto(index) ? psprintf("auto: %s", c) : pstrdup(c);
}

/* Whether the choice of sift_col_auto_redecide could be made but for the heap lock it does not wait for (someone else holds it): a step that needs the choice is left to the next call */
bool
sift_col_redecide_busy(Relation index)
{
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || IsInParallelMode())
		return false;
	return !ConditionalLockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock);
}

/*
 * Before a step of the rewrite, with every immutable segment in hand (col/maint/merge.c takes them all when the key changes): what a build chooses from the data and keeps in an option it wrote itself (key_cut, order_array_columns, point_tid_columns,
 * each 'auto' or 'auto: list') is chosen again, in the order of the build, so the segments about to be written are those a REINDEX would write: rows of every tenant appended
 * at the end of a heap built in the order of its tenants make a cut pay, the cut makes the ends of an order array and a point directory the better choices, and the index
 * a merge kept at the choices of the day of its build was a third larger. The rewrite is due for its own reasons (dead or fragmented units, the buffer), so the decision
 * adds the sample of the cut (at most a quarter of the heap, the build's) and no rewrite. An option the user wrote is left as it is. Returns whether an option changed; the
 * caller reads the schema again after it. Returns whether key_cut changed (the merge then sorts the rows of its inputs again).
 */
bool
sift_col_auto_redecide(Relation index, const SiftSegEntry *in, int n)
{
	Relation	heap;
	char	   *cut,
			   *arr,
			   *pt;
	const char *o;
	bool		pushed = false,
				changed;
	uint64		loaded = 0;
	MemoryContext mcx;
	StringInfoData what;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || IsInParallelMode())
		return false;
	/* the lock and the reasons of sift_col_auto_keys_merge: the merge holds the metapage, a VACUUM the heap */
	if (!ConditionalLockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock))
		return false;
	heap = table_open(index->rd_index->indrelid, NoLock);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	/* a merge that cuts by another key than the segments' holds their rows whole and sorts them: a cut is chosen only if they fit (col/order.c) */
	mcx = AllocSetContextCreate(CurrentMemoryContext, "sift_col redecide", ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < n; i++)
	{
		SiftColSegment *seg = sift_cseg_open(index, &in[i], mcx);

		loaded += sift_cseg_live_rows(seg);
		sift_cseg_close(seg);
		MemoryContextReset(mcx);
	}
	loaded *= sift_colk_row_bytes(sift_col_rel_private(index, mcx)->schema);
	MemoryContextDelete(mcx);
	cut = auto_cut_text(index);
	o = sift_col_options_order_array_columns(index);
	arr = pstrdup(o ? o : "");
	pt = pstrdup(sift_col_options_point_tid_columns(index));
	changed = sift_col_auto_cut_again(heap, index, loaded <= (uint64) maintenance_work_mem * 1024);
	sift_col_auto_gorder(heap, index, false);
	sift_col_auto_point(heap, index);
	if (pushed)
		PopActiveSnapshot();
	table_close(heap, NoLock);
	initStringInfo(&what);
	if (changed)
		appendStringInfo(&what, "key_cut = '%s' (was '%s'); ", auto_cut_text(index), cut);
	o = sift_col_options_order_array_columns(index);
	if (strcmp(arr, o ? o : "") != 0)
		appendStringInfo(&what, "order_array_columns = '%s' (was '%s'); ", o ? o : "", arr);
	if (strcmp(pt, sift_col_options_point_tid_columns(index)) != 0)
		appendStringInfo(&what, "point_tid_columns = '%s' (was '%s'); ", sift_col_options_point_tid_columns(index), pt);
	if (what.len == 0)
		return false;
	what.data[what.len - 2] = '\0';
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": the rewrite chooses again from the data: %s", RelationGetRelationName(index), what.data),
			 errdetail("Rows appended since the build change what the defaults choose; an option you wrote is never changed."),
			 errhint("ALTER INDEX %s SET (key_cut = 'off') keeps a choice off, and the same for the other options.", quote_identifier(RelationGetRelationName(index)))));
	return changed;
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
	 * A VACUUM that vacuums the other indexes of the table with workers runs this index in its leader, in parallel mode, where the write of the option cannot
	 * assign a transaction id (nor increment the command counter): the next merge decides.
	 */
	if (IsInParallelMode())
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
	heap = table_open(index->rd_index->indrelid, NoLock);
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col auto keys", ALLOCSET_DEFAULT_SIZES);
	uint32		units = 0,
				have[SIFT_COL_MAX_COLS] = {0},
				cubed[SIFT_COL_MAX_COLS] = {0};
	StringInfoData kept,
				left;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		SiftCPaggCensus c;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
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




