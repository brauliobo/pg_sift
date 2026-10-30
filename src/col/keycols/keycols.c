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
#include "catalog/pg_statistic.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/keycols/keycols_int.h"
#include "col/ktab.h"
#include "col/setsum.h"

bool
keycols_is_fk_column(Relation heap, AttrNumber attno)
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

bool
keycols_ends_in_id(const char *name)
{
	size_t		n = strlen(name);

	return n > 3 && strcmp(name + n - 3, "_id") == 0;
}

/* The option name of index set to value; visible to the relcache entry at once */
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
	List	   *def = list_make1(makeDefElem((char *) name, (Node *) makeString(pstrdup(value)), -1));

	/* the lock an update of an index's pg_class row expects on its table (checked in assert builds); the ShareLock of the build already excludes every other holder of it */
	LockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock);
	CommandCounterIncrement();
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(index)));
	SIFT_INVARIANT(HeapTupleIsValid(tuple), "sift_col: no pg_class row for index %u", RelationGetRelid(index));
	old = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	newopts = transformRelOptions(isnull ? (Datum) 0 : old, def, NULL, NULL, false, false);
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

/* the heap column of a leaf (0: a path of a document, which no constraint names) */
AttrNumber
keycols_heap_attno(Relation index, const SiftColDef *d)
{
	return index->rd_index->indkey.values[d->attno - 1];
}

bool
sift_col_leaf_unique(Relation heap, Relation index, const SiftColDef *d)
{
	AttrNumber	hatt = keycols_heap_attno(index, d);

	return hatt != 0 && is_unique_column(heap, hatt);
}

bool
sift_col_key_attr(Relation heap, AttrNumber hatt, const SiftColSchema *schema, const SiftColDef *d)
{
	const char *name = NameStr(TupleDescAttr(RelationGetDescr(heap), hatt - 1)->attname);

	/* the option is a list split at commas, and its names are not quoted */
	return sift_cset_pres_leaf_ok(schema, d) && strchr(name, ',') == NULL && (keycols_is_fk_column(heap, hatt) || keycols_ends_in_id(name)) && !is_unique_column(heap, hatt);
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
		else if (units > 0 && cubed[c] * 100 < (uint64) units * SIFT_COL_AUTO_ADD_PCT && sift_col_key_shaped(heap, index, rel->schema, &rel->schema->cols[c]))
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

#define SIFT_POINT_BYTES_ROW	20		/* what the point directory of a unique column takes a row: 8-byte slots, a power of two of them for the keys */

static void
append_bytes(StringInfo s, uint64 n)
{
	appendStringInfo(s, UINT64_FORMAT " bytes", n);
}

/*
 * After a build that is the build's choice: the columns that hold a key list or a bitmap in SIFT_COL_AUTO_KEEP_PCT of the units stay in the option, the others go out
 * of it (the cube answers a column it holds in as many units; a column whose keys fit no share of a unit is scanned), and a NOTICE says which, how many bytes
 * the lists take, what a filter of a column left out would take at most, and how to turn the default off.
 */
void
sift_col_auto_keys_report(Relation index)
{
	const SiftColRel *rel = sift_col_rel_private(index, CurrentMemoryContext);	/* the relcache frees the cached state on a reload, and the loop calls into readers */
	const SiftCPaggSpec *spec = &rel->pagg;
	const char *name = quote_identifier(RelationGetRelationName(index));
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col auto keys", ALLOCSET_DEFAULT_SIZES);
	uint32		units = 0,
				have[SIFT_COL_MAX_COLS] = {0},
				filters[SIFT_COL_MAX_COLS] = {0},
				cubed[SIFT_COL_MAX_COLS] = {0};
	uint64		bytes[SIFT_COL_MAX_COLS] = {0};
	SiftColLeafStats ls[SIFT_COL_MAX_COLS];
	StringInfoData kept,
				left,
				why,
				filtered;
	uint64		kept_bytes = 0,
				point_bytes = 0;
	bool		cube_left = false;

	for (int l = 0; l < SIFT_COL_MAX_COLS; l++)
		sift_col_leaf_stats_init(&ls[l]);
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
			sift_cseg_leaf_stats(seg, l, &ls[l]);
			for (uint32 u = 0; u < seg->nsb; u++)
			{
				uint32		len;
				const uint8 *blob = sift_cseg_set(seg, l, u, &len);

				if (blob != NULL && len > 0 && (blob[0] == SIFT_CSET_PRES || blob[0] == SIFT_CSET_PLIST || blob[0] == SIFT_CSET_RUNS || blob[0] == SIFT_CSET_BLOOM))
				{
					have[l]++;
					bytes[l] += len;
					filters[l] += blob[0] == SIFT_CSET_BLOOM;
				}
			}
		}
		sift_cseg_close(seg);
		MemoryContextReset(cx);
	}
	initStringInfo(&kept);
	initStringInfo(&left);
	initStringInfo(&why);
	initStringInfo(&filtered);
	for (int l = 0; l < rel->schema->ncols; l++)
	{
		if (!spec->pres[l])
			continue;
		if (units > 0 && cubed[l] * 100 >= (uint64) units * SIFT_COL_AUTO_KEEP_PCT)
		{
			cube_left = true;
			keycols_append_name(&left, keycols_leaf_name(index, &rel->schema->cols[l]));
			appendStringInfo(&why, "%s%s (the cube holds it in %u%% of the units)", why.len ? "; " : "", keycols_leaf_name(index, &rel->schema->cols[l]),
							 (uint32) ((uint64) cubed[l] * 100 / units));
		}
		else if (units > 0 && have[l] * 100 < (uint64) units * SIFT_COL_AUTO_KEEP_PCT)
		{
			/* what a filter of the column would take at most in a unit: a bit for every integer of its range, or 10 bits for every row */
			double		rows = Max(ls[l].nvals - ls[l].nnull, 0) / Max(units, 1);
			double		range = ls[l].has_range ? (double) ls[l].max - (double) ls[l].min + 1 : rows * 8 * 2;
			uint64		per_unit = (uint64) Min(range / 8, rows * SIFT_CSET_BLOOM_BITS / 8) + SIFT_CSET_HDR;

			keycols_append_name(&left, keycols_leaf_name(index, &rel->schema->cols[l]));
			appendStringInfo(&why, "%s%s (no key list or bitmap within %d%% of a unit in %u%% of the units; a filter would take up to about " UINT64_FORMAT " bytes)",
							 why.len ? "; " : "", keycols_leaf_name(index, &rel->schema->cols[l]), SIFT_CSET_PRES_PCT,
							 (uint32) ((uint64) (units - have[l]) * 100 / units), per_unit * units);
		}
		else
		{
			keycols_append_name(&kept, keycols_leaf_name(index, &rel->schema->cols[l]));
			if (filters[l] > 0)
			{
				keycols_append_name(&filtered, keycols_leaf_name(index, &rel->schema->cols[l]));
				point_bytes += (uint64) Max(ls[l].nvals - ls[l].nnull, 0) * SIFT_POINT_BYTES_ROW;
			}
			kept_bytes += bytes[l];
		}
	}
	if (left.len > 0)
		set_chosen(index, &kept);
	/* an empty index has measured nothing, and an index that keeps nothing costs nothing: neither says anything */
	if (kept.len > 0 && units > 0)
	{
		StringInfoData detail;

		initStringInfo(&detail);
		appendStringInfoString(&detail, "They are foreign key columns and columns named *_id of type " SIFT_KEY_TYPES " that are not unique, and a unique one that the units are cut by or the heap does not follow: a key list, bitmap, list of runs or filter of each unit, ");
		append_bytes(&detail, kept_bytes);
		appendStringInfoString(&detail, " together, which a count(DISTINCT) of one reads instead of its rows.");
		if (filtered.len > 0)
			appendStringInfo(&detail, " %s: where no list fits, a bloom filter of what the share has left (%d to 10 bits a key, within %d%% of a unit with the lists) makes an equality or IN skip the units that lack the key; it serves no count(DISTINCT). point_tid_columns = '%s' lists the rows of each key, so that an equality reads one unit, for about ", filtered.data, SIFT_CSET_BLOOM_MIN_BITS, SIFT_CSET_FILTER_PCT, filtered.data);
		if (filtered.len > 0)
		{
			appendStringInfo(&detail, "%.1f MB", point_bytes / 1048576.0);
			appendStringInfoString(&detail, " more (a b-tree is about as large).");
		}
		if (left.len > 0)
			appendStringInfo(&detail, " Left out: %s.%s", why.data,
							 cube_left ? " The units written now keep the lists of a column the cube holds; a REINDEX drops them." : "");
		ereport(NOTICE, (errmsg("sift_col index %s keeps the keys of %s in every unit", name, kept.data), errdetail_internal("%s", detail.data),
						 errhint("ALTER INDEX %s SET (key_columns = 'off'), then REINDEX, turns it off; key_columns = 'a, b' on CREATE INDEX names the columns.", name)));
	}
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
	const char *opt = sift_col_options_paths_raw(index);
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
					both,
					detail;

		initStringInfo(&arrs);
		initStringInfo(&only);
		initStringInfo(&both);
		initStringInfo(&detail);
		keycols_auto_array_specs(heap, index, &arrs, &only, &both);
		if (arrs.len > 0)
		{
			const char *name = quote_identifier(RelationGetRelationName(index));

			appendStringInfo(&spec, "%s%s", spec.len ? ", " : "", arrs.data);
			appendStringInfoString(&detail, "An element test (@>, &&, <@, = ANY) of an array column of text or integers reads a directory of the rows that own each element, where it read every array.");
			if (only.len > 0)
				appendStringInfo(&detail, " The arrays of %s are nearly all different in a sample, so the index keeps their elements in place of the arrays: equality with an array reads the rows that own its elements and their order, GROUP BY and DISTINCT of an array and the array itself read the table.", only.data);
			if (both.len > 0)
				appendStringInfo(&detail, " The arrays of %s repeat, so the index keeps them as well (a '+' in paths).", both.data);
			ereport(NOTICE, (errmsg("sift_col index %s keeps the elements of the arrays %s", name, only.len > 0 && both.len > 0 ? psprintf("%s, %s", only.data, both.data) : only.len > 0 ? only.data : both.data),
							 errdetail_internal("%s", detail.data),
							 errhint("ALTER INDEX %s SET (paths = ''), then REINDEX, leaves the elements out.", name)));
		}
	}
	if (spec.len == 0)
		return;
	{
		StringInfoData v;
		const char *name = quote_identifier(RelationGetRelationName(index));

		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", spec.data);
		sift_col_set_option(index, "paths", v.data);
		if (ncols + ngroups > 0)
			ereport(NOTICE, (errmsg("sift_col index %s reads the jsonb paths %s", name, spec.data),
							 errdetail("They are the keys that are a scalar of one type in most of a sample of %d documents, a map of more than 4 keys of one type as a wildcard, an array of objects as a group; a REINDEX keeps them, and ALTER INDEX ... SET (paths = 'auto') then REINDEX reads the documents again.",
									   SIFT_COL_AUTO_PATHS_SAMPLE),
							 errhint("Write paths = '...' to fix them by hand.")));
	}
}




