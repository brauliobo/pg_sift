/*
 * col/keycols.c - the default of the option key_columns: an index created without it, or with key_columns = 'auto', keeps the keys of its
 * foreign key columns and of its columns named *_id, as far as they are int2, int4, int8 or date, not unique, and not held by the cube. CREATE INDEX is the only
 * place it is decided: the columns are written to the option as 'auto: a, b', so that a rebuild keeps them, and the build keeps those that hold a key list or
 * a bitmap in half of the units or more. The same file decides the other defaults that read the columns' data before a build: the jsonb paths and the plain
 * columns that take the dimensions of the cube when there are more than it holds.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "catalog/index.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "col/am.h"
#include "col/gorder.h"
#include "col/ktab.h"
#include "col/pagg.h"
#include "col/preagg.h"
#include "col/seg.h"
#include "col/point.h"
#include "col/setsum.h"
#include "col/shred_pg.h"

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

static void
append_name(StringInfo s, const char *name)
{
	if (s->len > 0)
		appendStringInfoString(s, ", ");
	appendStringInfoString(s, name);
}

static const char *
leaf_name(Relation index, const SiftColDef *d)
{
	return NameStr(TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attname);
}

/* the heap column of a leaf (0: a path of a document, which no constraint names) */
static AttrNumber
heap_attno(Relation index, const SiftColDef *d)
{
	return index->rd_index->indkey.values[d->attno - 1];
}

bool
sift_col_leaf_unique(Relation heap, Relation index, const SiftColDef *d)
{
	AttrNumber	hatt = heap_attno(index, d);

	return hatt != 0 && is_unique_column(heap, hatt);
}

bool
sift_col_key_attr(Relation heap, AttrNumber hatt, const SiftColSchema *schema, const SiftColDef *d)
{
	const char *name = NameStr(TupleDescAttr(RelationGetDescr(heap), hatt - 1)->attname);

	/* the option is a list split at commas, and its names are not quoted */
	return sift_cset_pres_leaf_ok(schema, d) && strchr(name, ',') == NULL && (is_fk_column(heap, hatt) || ends_in_id(name)) && !is_unique_column(heap, hatt);
}

bool
sift_col_key_shaped(Relation heap, Relation index, const SiftColSchema *schema, const SiftColDef *d)
{
	AttrNumber	hatt = heap_attno(index, d);

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
	AttrNumber	hatt = heap_attno(index, d);
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
			append_name(&list, leaf_name(index, &rel->schema->cols[c]));
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
			append_name(&list, leaf_name(index, &rel->schema->cols[c]));
		else if (units > 0 && cubed[c] * 100 < (uint64) units * SIFT_COL_AUTO_ADD_PCT && sift_col_key_shaped(heap, index, rel->schema, &rel->schema->cols[c]))
		{
			append_name(&list, leaf_name(index, &rel->schema->cols[c]));
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
			append_name(&left, leaf_name(index, &rel->schema->cols[l]));
			appendStringInfo(&why, "%s%s (the cube holds it in %u%% of the units)", why.len ? "; " : "", leaf_name(index, &rel->schema->cols[l]),
							 (uint32) ((uint64) cubed[l] * 100 / units));
		}
		else if (units > 0 && have[l] * 100 < (uint64) units * SIFT_COL_AUTO_KEEP_PCT)
		{
			/* what a filter of the column would take at most in a unit: a bit for every integer of its range, or 10 bits for every row */
			double		rows = Max(ls[l].nvals - ls[l].nnull, 0) / Max(units, 1);
			double		range = ls[l].has_range ? (double) ls[l].max - (double) ls[l].min + 1 : rows * 8 * 2;
			uint64		per_unit = (uint64) Min(range / 8, rows * SIFT_CSET_BLOOM_BITS / 8) + SIFT_CSET_HDR;

			append_name(&left, leaf_name(index, &rel->schema->cols[l]));
			appendStringInfo(&why, "%s%s (no key list or bitmap within %d%% of a unit in %u%% of the units; a filter would take up to about " UINT64_FORMAT " bytes)",
							 why.len ? "; " : "", leaf_name(index, &rel->schema->cols[l]), SIFT_CSET_PRES_PCT,
							 (uint32) ((uint64) (units - have[l]) * 100 / units), per_unit * units);
		}
		else
		{
			append_name(&kept, leaf_name(index, &rel->schema->cols[l]));
			if (filters[l] > 0)
			{
				append_name(&filtered, leaf_name(index, &rel->schema->cols[l]));
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

static void auto_array_specs(Relation heap, Relation index, StringInfo spec, StringInfo only, StringInfo both);

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
		auto_array_specs(heap, index, &arrs, &only, &both);
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

/*
 * Before a build: the columns that get the global order array (order_array_columns, col/gorder.h) when the option is unset or 'auto' (not 'off', not a list). A
 * range of a column of up to SIFT_CORDER_MAX_ROWS rows reads the entries between its bounds, where the units are all read when the values are scattered over the
 * heap (their min and max cover the range): 18.4M instructions against 2.6M for 790 of 800,000 rows. It costs the array (4.4 bytes a row for an int4, 5.6 for a
 * timestamp) and the sorted run it extends (2.2 and 3.5), so it is built where it pays only, from the statistics of the table: a column of GORD_AUTO_DISTINCT or
 * more distinct values (fewer are answered by a key list, a point directory or the units) whose correlation with the heap order is under GORD_AUTO_CORR (a
 * column in heap order has narrow zones), on a table of GORD_AUTO_ROWS or more rows (below it a scan of the units is within 100k instructions), not a column
 * that cuts the units, and an array of no more than GORD_AUTO_GROWTH_PCT% of the index: the bytes of the
 * index a row are estimated from the statistics of its columns (the bits of their distinct values, the values of a dictionary (strings, uuids), at most their width), so a narrow index
 * of a few small columns gets none. At most GORD_AUTO_MAX columns: the dates and timestamps first (a range and an ORDER BY .. LIMIT of a time column is the common
 * use of an order), then the text columns (ORDER BY .. LIMIT of strings up to GORD_TEXT_WIDTH bytes, by the default formula like a time column), then the integers, each the
 * least correlated first. A table of GORD_AUTO_ROWS rows or more with no statistics is analyzed first (ANALYZE, once, with a
 * NOTICE), because the choice reads them. A REINDEX decides again.
 */
#define GORD_AUTO_ROWS		100000
#define GORD_AUTO_DISTINCT	1000
#define GORD_AUTO_CORR		0.99
#define GORD_AUTO_GROWTH_PCT 25
#define GORD_AUTO_MAX		2
#define GORD_TIME_SPEEDUP	25.0	/* S of the shape a time column's array is for: ts >= x ORDER BY ts LIMIT 10 on scattered values, 9.39 M to 0.38 M instructions at 500,000 rows (ORDER BY ts DESC LIMIT 10 10x, OFFSET 1000 11x) */
#define GORD_TIME_WRITE_PCT	35.0	/* W: CREATE INDEX of a one-column timestamp index 1.031 G to 1.387 G instructions (500,000 rows) */
#define GORD_TEXT_SPEEDUP	300.0	/* S of ORDER BY txt LIMIT 10 on a table at the 100,000-row floor: 82.8 M to 0.27 M instructions (3,000,000 rows 2,429 M to 0.86 M, so the floor is the least) */
#define GORD_TEXT_WRITE_C	62.0	/* W: CREATE INDEX of (id, text, timestamp, int4) with the text array under the C collation 2.84 G to 4.62 G instructions (500,000 rows) */
#define GORD_TEXT_WRITE_LOC	420.0	/* ... under a locale (pt_BR.UTF-8), 2.83 G to 14.73 G: the sort of the strings by strcoll. Both for 4 columns; the share of a wider index is smaller */
#define GORD_TEXT_WIDTH		64		/* the array holds the strings again (8.8 bytes a row for 9-byte strings, 38 for 61, 100 for 101: the index grows 77%, 158%, 330%): a column of longer strings is left to order_array_columns by name */
#define GORD_ROW_OVERHEAD	1.3		/* the bytes a row of an index beyond the bits of its distinct values (units, zones, presence): 3.73 measured against 2.4 estimated for one timestamp column */
#define GORD_GROUP_DISTINCT	100		/* the group of a time column's arrays: an integer of this many distinct values or more (an entity), at most one in GORD_GROUP_DEPTH rows of the table apiece (the 24 bytes of a group stay under a tenth of the 5 of an entry) */
#define GORD_GROUP_DEPTH	64
#define GORD_GROUP_BYTES	6.0		/* the second order of a timestamp: its 5.6 bytes a row and the groups (the structure's share of the room, as an array's) */

/* the statistics of heap column hatt: distinct values, the correlation with the heap order (1 when unknown) and the average width; false without them */
static bool
col_stats(Relation heap, AttrNumber hatt, double *ndistinct, double *corr, int32 *width)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false));
	AttStatsSlot sslot;

	if (tp == NULL)
		return false;
	*ndistinct = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
	if (*ndistinct < 0)
		*ndistinct = -*ndistinct * heap->rd_rel->reltuples;
	*width = ((Form_pg_statistic) GETSTRUCT(tp))->stawidth;
	*corr = 1.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		if (sslot.nnumbers > 0)
			*corr = fabs(sslot.numbers[0]);
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return true;
}

/* about how many bytes a row of the index takes, from the statistics of its columns: the bits of their distinct values, and the strings of a dictionary, at most their width */
static double
est_index_bytes(Relation heap, Relation index)
{
	double		bytes = 0;

	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	hatt = index->rd_index->indkey.values[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		double		d,
					corr;
		int32		width;

		if (hatt == 0 || !col_stats(heap, hatt, &d, &corr, &width))
		{
			bytes += att->attlen > 0 ? att->attlen : 32;
			continue;
		}
		bytes += Min(width, log2(Max(d, 2)) / 8 + (att->attbyval ? 0 : width * d / Max(heap->rd_rel->reltuples, 1)));
	}
	return bytes;
}

/* The table has a row estimate of at least GORD_AUTO_ROWS and no statistics of the first column of the index: the choice would decide nothing, so the indexed columns of the table are analyzed, as ANALYZE (columns) does (the NOTICE of the arrays it chooses says so). A REINDEX cannot: ANALYZE opens the index it rebuilds */
static void
analyze_if_unknown(Relation heap, Relation index)
{
	BlockNumber pages;
	double		tuples,
				allvis;
	AttrNumber	hatt = index->rd_index->indkey.values[0];
	StringInfoData cmd;
	const char *sep = "";

	if (hatt == 0 || ReindexIsProcessingIndex(RelationGetRelid(index)) || ReindexIsProcessingHeap(RelationGetRelid(heap)) || SearchSysCacheExists3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false)))
		return;
	table_relation_estimate_size(heap, NULL, &pages, &tuples, &allvis);
	if (tuples < GORD_AUTO_ROWS)
		return;
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "ANALYZE %s.%s (", quote_identifier(get_namespace_name(RelationGetNamespace(heap))), quote_identifier(RelationGetRelationName(heap)));
	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	att = index->rd_index->indkey.values[i];

		if (att == 0)
			continue;
		appendStringInfo(&cmd, "%s%s", sep, quote_identifier(get_attname(RelationGetRelid(heap), att, false)));
		sep = ", ";
	}
	appendStringInfoChar(&cmd, ')');
	if (SPI_connect() != SPI_OK_CONNECT)
		return;
	(void) SPI_execute(cmd.data, false, 0);
	SPI_finish();
	CommandCounterIncrement();
}

/*
 * The point directory by the default formula. An equality on an integer column of nearly unique values that does not follow the order of the heap
 * reads every unit, whatever the zones and the key lists: the keys are spread over the whole range of each unit. The directory of heap addresses (option point_tid_columns: an equality or
 * an IN list returns the addresses with no unit read) answers it in a flat 0.2 M instructions, and a 16-key IN list in 0.49 M (a b-tree 0.50 M; the directory of
 * rows, point_columns, reads a unit for each key: 0.74 M). Measured on 200,000 to 1,000,000 random unique int4, int8 rows, one column: the equality 1.17 M and 5.17 M instructions without it (about
 * 0.17 M + 5 instructions a row), 0.2 M with it (a b-tree 0.14 M); the index 16.5 bytes a row larger (8.3 to 25.4 MB at 1,000,000 int8 rows, a b-tree 22.5 MB); CREATE INDEX
 * +21% instructions (1,756 to 2,126 a row, 1,719 to 2,092 for int4). B = log2(S) / log2(1.05) against C = G / 3 + W / 5.
 */
#define POINT_AUTO_FIXED		170000.0	/* instructions of an equality that reads the units, whatever the rows */
#define POINT_AUTO_ROW			5.0			/* ... and for each row of the table */
#define POINT_AUTO_LOOKUP		200000.0	/* instructions of an equality through the directory */
#define POINT_AUTO_MIN_SAVED	100000.0
#define POINT_AUTO_DIR_BYTES	16.5
#define POINT_AUTO_BUILD_PCT	21.0
#define POINT_AUTO_OVERHEAD		0.4			/* the bytes a row of an index beyond its values: units, zones, headers */
#define POINT_AUTO_UNIQUE		0.5			/* the distinct values of the column over the rows of the table: nearly unique */

/* the bits of the range of the values of an integer column, from the bounds of its histogram (the width of its type when it has none) */
static double
int_range_bits(Relation heap, AttrNumber hatt, Form_pg_attribute att)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false));
	AttStatsSlot sslot;
	double		bits = att->attlen * 8.0;

	if (tp == NULL)
		return bits;
	if (att->attbyval && get_attstatsslot(&sslot, tp, STATISTIC_KIND_HISTOGRAM, InvalidOid, ATTSTATSSLOT_VALUES) && sslot.nvalues > 1)
	{
		int64		lo,
					hi;

		if (att->attlen == 2)
		{
			lo = DatumGetInt16(sslot.values[0]);
			hi = DatumGetInt16(sslot.values[sslot.nvalues - 1]);
		}
		else if (att->attlen == 4)
		{
			lo = DatumGetInt32(sslot.values[0]);
			hi = DatumGetInt32(sslot.values[sslot.nvalues - 1]);
		}
		else
		{
			lo = DatumGetInt64(sslot.values[0]);
			hi = DatumGetInt64(sslot.values[sslot.nvalues - 1]);
		}
		bits = Min(bits, log2((double) hi - (double) lo + 2));
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return bits;
}

/* about how many bytes a row of the index takes without the directory: an integer that follows the heap takes a byte or two, one that does not takes the bits of its range, the other columns about half their width */
static double
point_base_bytes(Relation heap, Relation index)
{
	double		bytes = POINT_AUTO_OVERHEAD;

	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	hatt = index->rd_index->indkey.values[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		double		d,
					corr;
		int32		width;

		if (hatt == 0 || !col_stats(heap, hatt, &d, &corr, &width))
			bytes += att->attlen > 0 ? att->attlen : 16;
		else if (att->attbyval && att->attlen > 0 && att->atttypid != FLOAT4OID && att->atttypid != FLOAT8OID)
			bytes += corr >= 0.9 ? 1.5 : Min(int_range_bits(heap, hatt, att) / 8, att->attlen);
		else
			bytes += Max(1.0, width * 0.5);
	}
	return bytes;
}

/* whether the comma separated list names the leaf ('a, b'; 'auto: a, b' as written by a build) */
static bool
in_list(const char *list, const char *name)
{
	bool		autolist;
	const char *p = sift_cpagg_auto_list(list, &autolist);
	size_t		len = strlen(name);

	for (; *p == ' '; p++)
		;
	while (*p != '\0')
	{
		const char *end = strchr(p, ',');
		size_t		n = end ? (size_t) (end - p) : strlen(p);

		while (n > 0 && p[n - 1] == ' ')
			n--;
		if (n == len && strncmp(p, name, len) == 0)
			return true;
		if (end == NULL)
			break;
		for (p = end + 1; *p == ' '; p++)
			;
	}
	return false;
}

void
sift_col_auto_point(Relation heap, Relation index)
{
	const char *opt;
	const char *ordopt;
	const char *plain;
	const SiftColRel *rel;
	StringInfoData list;
	bool		autolist;
	double		rows,
				base;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return;
	rel = sift_col_rel_private(index, CurrentMemoryContext);	/* the options an earlier default wrote reach the relation here */
	opt = sift_col_options_point_tid_columns(index);
	plain = sift_col_options_point_columns(index);
	ordopt = sift_col_options_order_array_columns(index);
	(void) sift_cpagg_auto_list(opt, &autolist);
	if (plain[0] != '\0' || (opt[0] != '\0' && !autolist))
		return;
	rows = heap->rd_rel->reltuples;
	initStringInfo(&list);
	base = point_base_bytes(heap, index);
	for (int c = 0; c < rel->schema->ncols && rows > 0; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = heap_attno(index, d);
		double		distinct,
					corr,
					S,
					B,
					C,
					G;
		int32		width;

		if (hatt == 0 || d->parent >= 0 || d->store != SIFT_COLS_INT || d->lane != 0 || in_list(ordopt, leaf_name(index, d)) || !sift_cset_leaf_ok(rel->schema, d) || strchr(leaf_name(index, d), ',') != NULL)
			continue;
		if (!col_stats(heap, hatt, &distinct, &corr, &width) || distinct < POINT_AUTO_UNIQUE * rows || corr >= 0.9)
			continue;
		S = (POINT_AUTO_FIXED + POINT_AUTO_ROW * rows) / POINT_AUTO_LOOKUP;
		G = 100.0 * POINT_AUTO_DIR_BYTES / base;
		B = log2(Max(S, 1.0)) / log2(1.05);
		C = G / 3 + POINT_AUTO_BUILD_PCT / 5;
		if (S < 1.05 || (S - 1) * POINT_AUTO_LOOKUP < POINT_AUTO_MIN_SAVED || B < C)
			continue;
		append_name(&list, leaf_name(index, d));
		base += POINT_AUTO_DIR_BYTES;
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": point_tid_columns includes \"%s\", chosen by the default formula: an equality on its nearly unique values, which do not follow the order of the table, would read every unit (about %.1f M instructions against %.1f M with the directory; predicted index size %+.0f%%, benefit %.0f against cost %.0f)",
						RelationGetRelationName(index), leaf_name(index, d), (POINT_AUTO_FIXED + POINT_AUTO_ROW * rows) / 1e6, POINT_AUTO_LOOKUP / 1e6, G, B, C),
				 errhint("ALTER INDEX %s SET (point_tid_columns = 'off') and REINDEX keep the index without it.", RelationGetRelationName(index))));
	}
	if (list.len == 0)
	{
		if (opt[0] != '\0')
			sift_col_set_option(index, "point_tid_columns", "auto");
		return;
	}
	{
		StringInfoData v;

		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", list.data);
		sift_col_set_option(index, "point_tid_columns", v.data);
		pfree(v.data);
	}
}

typedef struct GordCand
{
	int			leaf;
	bool		time;			/* a date or a timestamp: a range or an ORDER BY .. LIMIT is what it is read by */
	bool		text;			/* a text or varchar column: ORDER BY .. LIMIT is what it is read by */
	double		corr;
	double		bytes;			/* a text column: the bytes a row of its array, the share of the build it adds, and its collation */
	double		write;
	double		benefit;		/* B and C of the choice of a text column, said by the NOTICE */
	double		cost;
} GordCand;

static int
gord_cand_cmp(const void *a, const void *b)
{
	const GordCand *x = a,
			   *y = b;

	if (x->time != y->time)
		return x->time ? -1 : 1;
	if (x->text != y->text)
		return x->text ? -1 : 1;
	if (x->text)
		return x->leaf - y->leaf;	/* the order of the index: the statistics of two columns of strings differ run to run */
	if (x->corr != y->corr)
		return x->corr < y->corr ? -1 : 1;
	return x->leaf - y->leaf;
}

void
sift_col_auto_gorder(Relation heap, Relation index)
{
	const char *opt = sift_col_options_order_array_columns(index);
	const char *gopt = sift_col_options_order_array_group(index);
	const SiftColRel *rel;
	bool		autolist,
				gauto,
				timepick = false;
	int			grp = -1;
	double		gdist = 0;
	GordCand   *cand;
	bool	   *fit;
	int			ncand = 0,
				npick = 0,
				pick[GORD_AUTO_MAX];
	double		room,
				index_bytes;
	StringInfoData v;

	(void) sift_cpagg_auto_list(opt, &autolist);
	if (opt != NULL && !autolist)
		return;
	(void) sift_cpagg_auto_list(gopt, &gauto);
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || (gopt[0] != '\0' && !gauto && strcmp(gopt, "off") != 0))
		return;
	analyze_if_unknown(heap, index);
	if (heap->rd_rel->reltuples < GORD_AUTO_ROWS)
		return;
	index_bytes = est_index_bytes(heap, index) + GORD_ROW_OVERHEAD;
	room = index_bytes * GORD_AUTO_GROWTH_PCT / 100;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	cand = palloc(rel->schema->ncols * sizeof(GordCand));
	fit = palloc(rel->schema->ncols * sizeof(bool));
	for (int c = 0; c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = heap_attno(index, d);
		double		ndistinct,
					cr;
		int32		width;
		bool		cut = false;

		bool		text = sift_cgord_leaf_text(d);

		if (d->parent >= 0 || hatt == 0 || !(text || sift_cgord_leaf_ok(rel->schema, d)))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		if (cut && sift_col_key_cut(rel))
			continue;
		if (!col_stats(heap, hatt, &ndistinct, &cr, &width) || ndistinct < GORD_AUTO_DISTINCT || cr >= GORD_AUTO_CORR || (text && width > GORD_TEXT_WIDTH))
			continue;
		cand[ncand] = (GordCand) {c, sift_cgord_leaf_time(d), text, cr, 0, 0, 0, 0};
		if (text)
		{
			/* the keys and addresses (4.4 bytes) and the strings, which front coding shortens by a tenth; the build adds the sort of the strings, by strcoll under a locale */
			cand[ncand].bytes = 4.4 + Min(width, 255) * Min(1.0, ndistinct / Max(heap->rd_rel->reltuples, 1)) * 0.9;
			cand[ncand].write = (varstr_cmp("a", 1, "B", 1, TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attcollation) < 0 ? GORD_TEXT_WRITE_LOC : GORD_TEXT_WRITE_C) * 4.0 / index->rd_index->indnatts;
		}
		ncand++;
	}
	qsort(cand, ncand, sizeof(GordCand), gord_cand_cmp);
	/* each array is a structure of its own: it must fit the room, and a column that does not leaves it to the next; the sorted run of a column not in order_columns comes with it */
	for (int i = 0; i < ncand; i++)
	{
		const SiftColDef *d = &rel->schema->cols[cand[i].leaf];
		bool		narrow = d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_DATE;
		double		bytes = cand[i].text ? cand[i].bytes : narrow ? 4.4 : 5.6;

		/* a date, timestamp or text column: the default formula decides, B = log2(S) / log2(1.05) against C = G / 3 + W / 5, with no ceiling; the others fit the room */
		if (cand[i].text)
		{
			double		b = log2(GORD_TEXT_SPEEDUP) / log2(1.05),
						cost = bytes / index_bytes * 100.0 / 3.0 + cand[i].write / 5.0;

			fit[i] = b > cost;
			cand[i].benefit = b;
			cand[i].cost = cost;
		}
		else
			fit[i] = cand[i].time ? log2(GORD_TIME_SPEEDUP) / log2(1.05) > bytes / index_bytes * 100.0 / 3.0 + GORD_TIME_WRITE_PCT / 5.0 : bytes <= room;
		timepick |= fit[i] && cand[i].time;
	}
	/* the arrays of a time column are grouped by the entity column that has the most values: WHERE g = x ORDER BY ts LIMIT n then reads n entries of x in each segment, where the plain order has the units read (3.3 to 12.2 M instructions against 0.27 M for a b-tree, 500,000 to 3,000,000 rows). The entity column is not an array of its own then */
	for (int c = 0; timepick && sift_col_format_minor >= SIFT_COL_FORMAT_PRESENCE && strcmp(gopt, "off") != 0 && GORD_GROUP_BYTES <= room && c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = heap_attno(index, d);
		double		ndistinct,
					cr;
		int32		width;
		bool		cut = false;

		if (d->parent >= 0 || hatt == 0 || !sift_cgord_leaf_ok(rel->schema, d) || sift_cgord_leaf_time(d))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		if (cut || !col_stats(heap, hatt, &ndistinct, &cr, &width) || cr >= GORD_AUTO_CORR || ndistinct < GORD_GROUP_DISTINCT || heap->rd_rel->reltuples / ndistinct < GORD_GROUP_DEPTH)
			continue;
		/* a foreign key or a column named *_id is the entity the rows belong to; among those, or among the rest, the one of most values */
		if (is_fk_column(heap, hatt) || ends_in_id(leaf_name(index, d)))
			ndistinct += heap->rd_rel->reltuples;
		if (ndistinct > gdist)
		{
			gdist = ndistinct;
			grp = c;
		}
	}
	for (int i = 0; i < ncand && npick < GORD_AUTO_MAX; i++)
		if (fit[i] && cand[i].leaf != grp)
		{
			pick[npick++] = cand[i].leaf;
			if (cand[i].text)
			{
				const char *name = leaf_name(index, &rel->schema->cols[cand[i].leaf]);

				ereport(NOTICE, (errmsg("sift_col index \"%s\": order_array_columns = 'auto' chose the text column \"%s\": benefit %.0f against cost %.0f", RelationGetRelationName(index), name, cand[i].benefit, cand[i].cost),
								 errdetail("ORDER BY %s LIMIT n reads n entries of each segment, where the units compare the strings of every row (100,000 rows: 82.8 M to 0.27 M instructions). The array takes about %.0f bytes a row (%.0f%% of the index, G) and CREATE INDEX does about %.0f%% more instructions (W)%s: the benefit is log2(%.0f) / log2(1.05), the cost G / 3 + W / 5.",
										   name, cand[i].bytes, cand[i].bytes / index_bytes * 100.0, cand[i].write, cand[i].write > GORD_TEXT_WRITE_C * 4.0 / index->rd_index->indnatts ? ", the sort of the strings under the collation" : "", GORD_TEXT_SPEEDUP),
								 errhint("ALTER INDEX %s SET (order_array_columns = 'off'), then REINDEX, drops it; order_array_columns = '%s' keeps it by name.", quote_identifier(RelationGetRelationName(index)), name)));
			}
		}
	/* a text column the formula rejected, the cheapest of them: said once, with the setting that builds it anyway (a narrow index under a locale pays the sort of the strings in a large share of its build) */
	{
		int			rej = -1;

		for (int i = 0; i < ncand; i++)
			if (cand[i].text && !fit[i] && (rej < 0 || cand[i].cost < cand[rej].cost))
				rej = i;
		if (rej >= 0)
		{
			const char *name = leaf_name(index, &rel->schema->cols[cand[rej].leaf]);

			ereport(NOTICE, (errmsg("sift_col index \"%s\": order_array_columns = 'auto' did not choose the text column \"%s\": benefit %.0f against cost %.0f", RelationGetRelationName(index), name, cand[rej].benefit, cand[rej].cost),
							 errdetail("ORDER BY %s LIMIT n would read n entries of each segment, where the units compare the strings of every row. The array takes about %.0f bytes a row (%.0f%% of the index, G) and CREATE INDEX would do about %.0f%% more instructions (W)%s: the cost is G / 3 + W / 5 against the benefit log2(%.0f) / log2(1.05).",
									   name, cand[rej].bytes, cand[rej].bytes / index_bytes * 100.0, cand[rej].write, cand[rej].write > GORD_TEXT_WRITE_C * 4.0 / index->rd_index->indnatts ? ", the sort of the strings under the collation, in a share of the build that falls as the index has more columns" : "", GORD_TEXT_SPEEDUP),
							 errhint("CREATE INDEX .. WITH (order_array_columns = '%s'), or ALTER INDEX %s SET (order_array_columns = '%s') and REINDEX, if ORDER BY %s LIMIT n matters more than the build.", name, quote_identifier(RelationGetRelationName(index)), name, name)));
		}
	}
	if (grp >= 0)
	{
		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", leaf_name(index, &rel->schema->cols[grp]));
		sift_col_set_option(index, "order_array_group", v.data);
	}
	else if (gauto)
		sift_col_set_option(index, "order_array_group", "auto");
	if (npick == 0)
	{
		if (opt != NULL)
			sift_col_set_option(index, "order_array_columns", "auto");
		return;
	}
	initStringInfo(&v);
	appendStringInfoString(&v, "auto: ");
	for (int i = 0; i < npick; i++)
		appendStringInfo(&v, "%s%s", i ? ", " : "", leaf_name(index, &rel->schema->cols[pick[i]]));
	sift_col_set_option(index, "order_array_columns", v.data);
}

/*
 * Which plain columns take the dimensions of the cube. It holds SIFT_PAGG_MAX_DIMS of them (the calendar cells of the dates among them) and the default gives
 * the slots to the columns in their order; a column with more than SIFT_PAGG_DEF_GROUPS values in a unit has no histogram in it, so a slot given to such a column
 * holds nothing. An index with more columns than slots and preagg = 'auto' has them ranked here, once: CUBE_STRETCHES stretches of CUBE_STRETCH_BLOCKS blocks
 * (about a unit), evenly spaced over the table, are read, the columns that fit the groups in most of them come first, then the others, each in the order of the
 * schema, and the list is written to the option as 'auto: a, b' so that a rebuild keeps it (the cube takes the first of it, the calendar cells their room). A
 * NOTICE says so when the ranking changed which columns hold a slot. The columns of jsonb paths are not looked at: the index keeps the order of the schema.
 * It is not the default: the histograms it gives the columns that fit grow the index by more than the 3% a default may.
 */
#define CUBE_STRETCHES		8
#define CUBE_STRETCH_BLOCKS	256
#define CUBE_SET_SLOTS		1024	/* a power of two over twice the groups of a histogram */

typedef struct CubeCand
{
	int			leaf;
	AttrNumber	hatt;
	int			rank;			/* its place in the schema */
	uint32		fit;			/* the sampled stretches in which it has at most SIFT_PAGG_DEF_GROUPS values, NULL as one */
	uint32		seen;			/* values met in the stretch being read; over SIFT_PAGG_DEF_GROUPS it is out */
	uint64		groups;			/* the values of the stretches, together (at most SIFT_PAGG_DEF_GROUPS + 1 of each) */
} CubeCand;

static int
cmp_cube_cand(const void *a, const void *b)
{
	const CubeCand *x = a,
			   *y = b;

	return x->fit != y->fit ? (x->fit > y->fit ? -1 : 1) : x->rank - y->rank;
}

/* the key of a value in the set of its stretch (0 marks a free slot) */
static uint64
cube_key(Datum d, bool isnull, const Form_pg_attribute att)
{
	uint64		k;

	if (isnull)
		return 1;
	if (att->attbyval)
		k = (uint64) DatumGetInt64(d) * UINT64CONST(0x9E3779B97F4A7C15);
	else if (att->attlen == -1)
	{
		struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

		k = DatumGetUInt64(hash_any_extended((unsigned char *) VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), 0));
	}
	else
		k = DatumGetUInt64(hash_any_extended((unsigned char *) DatumGetPointer(d), att->attlen, 0));
	return k | 2;
}

/* the stretch st of nst over a table of nblocks: [*start, *start + *len) */
static void
cube_stretch_at(BlockNumber nblocks, int nst, int st, BlockNumber *start, BlockNumber *len)
{
	*start = (BlockNumber) ((uint64) st * nblocks / nst);
	*len = Min(CUBE_STRETCH_BLOCKS, nblocks / nst);
}

/* the stretch [start, start + len) of heap: cand[].seen counts the values of each column up to SIFT_PAGG_DEF_GROUPS + 1; fit and groups of those within it grow */
static void
cube_stretch(Relation heap, BlockNumber start, BlockNumber len, CubeCand *cand, int n, uint64 *sets, MemoryContext row_cx)
{
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);

	memset(sets, 0, (size_t) n * CUBE_SET_SLOTS * sizeof(uint64));
	for (int i = 0; i < n; i++)
		cand[i].seen = 0;
	heap_setscanlimits(scan, start, len);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cx);

		CHECK_FOR_INTERRUPTS();
		for (int i = 0; i < n; i++)
		{
			uint64	   *set = sets + (size_t) i * CUBE_SET_SLOTS;
			bool		isnull;
			Datum		d;
			uint64		k;

			if (cand[i].seen > SIFT_PAGG_DEF_GROUPS)
				continue;
			d = slot_getattr(slot, cand[i].hatt, &isnull);
			k = cube_key(d, isnull, TupleDescAttr(RelationGetDescr(heap), cand[i].hatt - 1));
			for (uint32 at = (uint32) (k >> 40) & (CUBE_SET_SLOTS - 1);; at = (at + 1) & (CUBE_SET_SLOTS - 1))
			{
				if (set[at] == 0)
				{
					set[at] = k;
					cand[i].seen++;
					break;
				}
				if (set[at] == k)
					break;
			}
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(row_cx);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	for (int i = 0; i < n; i++)
	{
		cand[i].fit += cand[i].seen <= SIFT_PAGG_DEF_GROUPS;
		cand[i].groups += cand[i].seen;
	}
}

/*
 * The plain columns of the cube's kind (a dimension of the top level, not a jsonb path) in the order of the schema; -1 when the choice is not made here: an
 * index of jsonb paths, a column that is an expression, or a name the option cannot hold (it is a list split at commas, and its names are not quoted).
 */
static int
cube_cands(Relation index, const SiftColSchema *schema, CubeCand *cand)
{
	int			n = 0;

	if (schema->kinds)
		return -1;
	for (int c = 0; c < schema->ncols; c++)
	{
		const SiftColDef *d = &schema->cols[c];
		const char *name = leaf_name(index, d);

		if (d->parent >= 0 || !sift_cpagg_dim_ok(schema, d))
			continue;
		if (heap_attno(index, d) == 0 || name[0] == '\0' || strpbrk(name, ",:;)") != NULL)
			return -1;
		cand[n] = (CubeCand) {.leaf = c, .hatt = heap_attno(index, d), .rank = n};
		n++;
	}
	return n;
}

/*
 * The elements of the array columns (paths = 'auto'): an array column of text or integers whose table has ARR_AUTO_ROWS rows or more and nearly every
 * array different (over ARR_DISTINCT_PCT% of a sample) gets the list of its elements (the paths 'col[] type', docs/COL-SHRED.md) in place of its image, which
 * cannot share anything there, when a sample says that this does not make the index more than ARR_AUTO_GROWTH_PCT% larger. Arrays that repeat keep the image,
 * which a GROUP BY or equality over whole arrays reads.
 */
#define ARR_AUTO_ROWS		10000
#define ARR_AUTO_GROWTH_PCT	25
#define ARR_DISTINCT_PCT	50
#define ARR_SAMPLE_ROWS		4096
#define ARR_SAMPLE_ELEMS	200000
#define ARR_SET_BITS		19
#define ARR_DIR_BYTES		1.5

typedef struct ArrSet
{
	uint64	   *slot;
	uint64		n;
} ArrSet;

typedef struct ArrCand
{
	int			idx;			/* its place in the index */
	AttrNumber	hatt;
	Oid			elem;
	uint64		rows;			/* arrays read (not NULL) */
	uint64		elems;			/* elements read, NULL ones too */
	uint64		bytes;			/* bytes of the arrays read */
	ArrSet		arrs;			/* hashes of the distinct arrays and elements */
	ArrSet		els;
} ArrCand;

/* adds the hash to the set; false when it was there or the set is full */
static bool
arr_set_add(ArrSet *s, uint64 h)
{
	uint64		mask = ((uint64) 1 << ARR_SET_BITS) - 1;

	h |= 1;
	if (s->n >= (mask >> 1))
		return false;
	for (uint64 p = (h >> 20) & mask;; p = (p + 1) & mask)
	{
		if (s->slot[p] == h)
			return false;
		if (s->slot[p] == 0)
		{
			s->slot[p] = h;
			s->n++;
			return true;
		}
	}
}

static void
arr_stretch(Relation heap, BlockNumber start, BlockNumber len, ArrCand *cand, int n, uint32 quota, MemoryContext row_cx)
{
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	uint32		got = 0;

	heap_setscanlimits(scan, start, len);
	while (got < quota && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cx);

		CHECK_FOR_INTERRUPTS();
		got++;
		for (int k = 0; k < n; k++)
		{
			ArrCand    *c = &cand[k];
			bool		isnull;
			Datum		d = slot_getattr(slot, c->hatt, &isnull);
			ArrayType  *a;
			int16		elen;
			bool		byval;
			char		align;
			Datum	   *vals;
			bool	   *nulls;
			int			ne;

			if (isnull)
				continue;
			a = DatumGetArrayTypeP(d);
			c->rows++;
			c->bytes += VARSIZE(a);
			arr_set_add(&c->arrs, DatumGetUInt64(hash_any_extended((unsigned char *) a, VARSIZE(a), 0)));
			get_typlenbyvalalign(c->elem, &elen, &byval, &align);
			deconstruct_array(a, c->elem, elen, byval, align, &vals, &nulls, &ne);
			for (int i = 0; i < ne && c->els.n < ARR_SAMPLE_ELEMS; i++)
			{
				c->elems++;
				if (nulls[i])
					continue;
				if (byval)
					arr_set_add(&c->els, (uint64) DatumGetInt64(vals[i]) * UINT64CONST(0x9E3779B97F4A7C15));
				else
				{
					text	   *t = DatumGetTextPP(vals[i]);

					arr_set_add(&c->els, DatumGetUInt64(hash_any_extended((unsigned char *) VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), 0)));
				}
			}
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(row_cx);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

/* the type name a path spec gives the elements */
static const char *
arr_elem_type(Oid elem)
{
	return elem == INT2OID ? "int2" : elem == INT4OID ? "int4" : elem == INT8OID ? "int8" : "text";
}

/*
 * An array column whose arrays repeat keeps its image (GROUP BY, DISTINCT and = read one code a row). An element test over the image decodes, hashes and
 * interns each distinct array of the dictionary, ARR_IMAGE_INSTR instructions an array (311.6 M instructions for 196,999 distinct arrays of 1,000,000 rows, 8.0 M
 * with the directory of the elements), where the elements cost about ARR_ELEM_ROW_INSTR a row. The elements are kept beside the image when the default formula
 * says so: speedup S, benefit R log2(S) / log2(1.05) with R = 1 (a common class) against G / 3 + W / 5, W
 * the build's write cost, taken as G (the same bytes are written), G at most ARR_AUTO_GROWTH_PCT% and the saving at least 100,000 instructions. The distinct arrays
 * are the statistics' when the table has them (a sample of a few thousand rows sees few repeats), else the sample's.
 */
#define ARR_IMAGE_INSTR		1500
#define ARR_ELEM_ROW_INSTR	5

static bool
arr_both_pays(Relation heap, const ArrCand *c, double growth)
{
	double		total = heap->rd_rel->reltuples,
				ndistinct,
				corr,
				distinct = (double) c->arrs.n / c->rows * total,
				saved,
				base,
				speedup;
	int32		width;

	if (col_stats(heap, c->hatt, &ndistinct, &corr, &width))
		distinct = Max(distinct, ndistinct < 0 ? -ndistinct * total : ndistinct);
	saved = distinct * ARR_IMAGE_INSTR;
	base = total * ARR_ELEM_ROW_INSTR;
	speedup = (base + saved) / base;
	return growth <= ARR_AUTO_GROWTH_PCT && saved >= 100000 && speedup >= 1.05 &&
		log2(speedup) / log2(1.05) >= growth / 3 + growth / 5;
}

/*
 * The arrays of the index that get their elements, from a sample of the table: appends 'col[] type' to spec and the names to added.
 */
static void
auto_array_specs(Relation heap, Relation index, StringInfo spec, StringInfo only, StringInfo both)
{
	ArrCand		cand[SIFT_COL_MAX_COLS];
	int			n = 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	int			nst = (int) Min(CUBE_STRETCHES, (nblocks + CUBE_STRETCH_BLOCKS - 1) / CUBE_STRETCH_BLOCKS);
	MemoryContext row_cx;
	double		base;

	if (sift_col_options(index)->array_lengths != SIFT_COL_ARRLEN_ON || heap->rd_rel->reltuples < ARR_AUTO_ROWS || nst == 0)
		return;
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index) && n < SIFT_COL_MAX_COLS; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		Oid			elem = sift_col_is_array_type(att->atttypid) ? get_element_type(att->atttypid) : InvalidOid;

		if (!OidIsValid(elem) || !sift_shred_array_elem_ok(elem) || index->rd_index->indkey.values[i] == 0 ||
			strcmp(quote_identifier(NameStr(att->attname)), NameStr(att->attname)) != 0)
			continue;
		cand[n] = (ArrCand) {.idx = i,.hatt = index->rd_index->indkey.values[i],.elem = elem};
		cand[n].arrs.slot = palloc0(sizeof(uint64) << ARR_SET_BITS);
		cand[n].els.slot = palloc0(sizeof(uint64) << ARR_SET_BITS);
		n++;
	}
	if (n == 0)
		return;
	row_cx = AllocSetContextCreate(CurrentMemoryContext, "sift array sample row", ALLOCSET_DEFAULT_SIZES);
	for (int st = 0; st < nst; st++)
	{
		BlockNumber start,
					len;

		cube_stretch_at(nblocks, nst, st, &start, &len);
		arr_stretch(heap, start, len, cand, n, ARR_SAMPLE_ROWS / nst, row_cx);
	}
	MemoryContextDelete(row_cx);
	base = est_index_bytes(heap, index);
	for (int k = 0; k < n; k++)
	{
		const ArrCand *c = &cand[k];
		const char *name = NameStr(TupleDescAttr(RelationGetDescr(index), c->idx)->attname);
		double		total = heap->rd_rel->reltuples,
					elem_row,
					image_row;
		bool		alone = c->arrs.n * 100 > c->rows * ARR_DISTINCT_PCT;

		if (c->rows < 100 || c->elems == 0)
			continue;
		elem_row = (double) c->elems / c->rows * (log2(Max(c->els.n, 2)) / 8 + ARR_DIR_BYTES);
		image_row = (double) c->bytes / c->rows * Min(1.0, (double) c->arrs.n / c->rows);
		if (alone)
		{
			/* nearly every array different: the image shares nothing and the elements, far smaller, stand in its place */
			if ((elem_row - image_row) * 100 / Max(base, 1) > ARR_AUTO_GROWTH_PCT)
				continue;
		}
		else if (!arr_both_pays(heap, c, elem_row * 100 / Max(base, 1)))
			continue;
		appendStringInfo(spec, "%s%s%s[] %s", spec->len ? ", " : "", alone ? "" : "+", name, arr_elem_type(c->elem));
		appendStringInfo(alone ? only : both, "%s%s", (alone ? only : both)->len ? ", " : "", name);
	}
	for (int k = 0; k < n; k++)
	{
		pfree(cand[k].arrs.slot);
		pfree(cand[k].els.slot);
	}
}

/* the stretches read over heap, and what the candidates hold in them (fit, groups); the number of stretches, 0 for an empty table */
static int
cube_sample(Relation heap, CubeCand *cand, int n)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	int			nst = (int) Min(CUBE_STRETCHES, (nblocks + CUBE_STRETCH_BLOCKS - 1) / CUBE_STRETCH_BLOCKS);
	uint64	   *sets;
	MemoryContext row_cx;

	if (nst == 0)
		return 0;
	sets = palloc((size_t) n * CUBE_SET_SLOTS * sizeof(uint64));
	row_cx = AllocSetContextCreate(CurrentMemoryContext, "sift cube ranking row", ALLOCSET_SMALL_SIZES);
	for (int st = 0; st < nst; st++)
	{
		BlockNumber start,
					len;

		cube_stretch_at(nblocks, nst, st, &start, &len);
		cube_stretch(heap, start, len, cand, n, sets, row_cx);
	}
	MemoryContextDelete(row_cx);
	pfree(sets);
	return nst;
}

void
sift_col_auto_cube(Relation heap, Relation index)
{
	const char *opt = sift_col_options_preagg(index);
	const SiftColRel *rel;
	CubeCand	cand[SIFT_COL_MAX_COLS];
	int			n,
				slots,
				nst;
	StringInfoData list,
				first,
				v;
	bool		moved = false;

	/* the default order, off, cube(...) and the list an earlier build chose stay: only preagg = 'auto' asks for the ranking */
	if (pg_strcasecmp(opt, "auto") != 0)
		return;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	n = cube_cands(index, rel->schema, cand);
	slots = SIFT_PAGG_MAX_DIMS - sift_cpagg_cal_count(rel->schema);
	if (n <= slots || slots <= 0 || (nst = cube_sample(heap, cand, n)) == 0)
		return;
	qsort(cand, n, sizeof(CubeCand), cmp_cube_cand);

	initStringInfo(&list);
	initStringInfo(&first);
	for (int i = 0; i < n && i < SIFT_PAGG_MAX_DIMS; i++)
	{
		append_name(&list, leaf_name(index, &rel->schema->cols[cand[i].leaf]));
		if (i < slots && cand[i].fit * 2 > (uint32) nst)
			append_name(&first, leaf_name(index, &rel->schema->cols[cand[i].leaf]));
		moved |= i < slots && cand[i].fit * 2 > (uint32) nst && cand[i].rank >= slots;
	}
	initStringInfo(&v);
	appendStringInfo(&v, "auto: %s", list.data);
	sift_col_set_option(index, "preagg", v.data);
	if (moved)
		ereport(NOTICE, (errmsg("sift_col index %s gives the dimensions of its cube to the columns whose histograms fit: %s", quote_identifier(RelationGetRelationName(index)), first.data),
						 errdetail("The cube holds at most %d dimensions, the calendar cells of the dates among them. A column with more than %d distinct values in a unit has no histogram there, so a dimension given to it holds nothing. Stretches of about a unit (%d of them, spread over the table) were read, and the columns that fit most of them come first.",
								   SIFT_PAGG_MAX_DIMS, SIFT_PAGG_DEF_GROUPS, nst),
						 errhint("ALTER INDEX %s SET (preagg = 'default'), then REINDEX, gives them in the order of the columns; preagg = 'cube(...)' names them.", quote_identifier(RelationGetRelationName(index)))));
}

/*
 * The rows rollup of rollup_dims = 'auto' (docs/COL-PREAGG.md): the pair advice of sift.index_advice, chosen before the build. Of the columns that fit the cube's
 * histograms in ROWS_AGREE_PCT of the sampled stretches (the SIFT_PAGG_MAX_RDIMS with the fewest values), the pair whose combinations fit the cells a unit may
 * keep (a tenth of its rows, at most SIFT_PAGG_ROWS_CELLS) in as many stretches, and has the fewest of them (the cheapest rollup, far from the limit of a unit),
 * is written to the option as 'auto: a, b'; none fits, nothing. It is the default (an empty option): the cells pay by the default formula (docs/OPTIONS.md); 'off' writes none.
 */
#define ROWS_AGREE_PCT	90
#define ROWS_CAP		(1 << 16)		/* rows of a stretch looked at */
#define ROWS_SLOTS		(4 * SIFT_PAGG_ROWS_CELLS)	/* a power of two over the cells a unit may keep */

typedef struct RowsPair
{
	uint32		ok;				/* stretches in which the combinations fit the cells of their rows */
	uint64		cells;			/* the combinations of those stretches, together */
} RowsPair;

/* the distinct pairs of a[] and b[], at most limit + 1 (it stops counting there); tab is ROWS_SLOTS zeroed words */
static uint32
rows_cells(const uint64 *a, const uint64 *b, uint32 rows, uint32 limit, uint64 *tab)
{
	uint32		cells = 0;

	memset(tab, 0, ROWS_SLOTS * sizeof(uint64));
	for (uint32 r = 0; r < rows && cells <= limit; r++)
	{
		uint64		h = ((a[r] ^ (b[r] * UINT64CONST(0xC2B2AE3D27D4EB4F))) * UINT64CONST(0x9E3779B97F4A7C15)) | 1;

		for (uint32 at = (uint32) (h >> 40) & (ROWS_SLOTS - 1);; at = (at + 1) & (ROWS_SLOTS - 1))
		{
			if (tab[at] == 0)
			{
				tab[at] = h;
				cells++;
				break;
			}
			if (tab[at] == h)
				break;
		}
	}
	return cells;
}

static int
cmp_cube_groups(const void *a, const void *b)
{
	const CubeCand *x = a,
			   *y = b;

	return x->groups != y->groups ? (x->groups < y->groups ? -1 : 1) : x->rank - y->rank;
}

/* the pairs of the m columns of cand in the stretch: pr[x * m + y] (x < y) counts the stretch when the combinations of both fit the cells of the rows read */
static void
rows_stretch(Relation heap, BlockNumber start, BlockNumber len, const CubeCand *cand, int m, RowsPair *pr)
{
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	MemoryContext row_cx = AllocSetContextCreate(CurrentMemoryContext, "sift rows rollup row", ALLOCSET_SMALL_SIZES);
	uint64	   *keys[SIFT_PAGG_MAX_RDIMS],
			   *tab = palloc(ROWS_SLOTS * sizeof(uint64));
	uint32		rows = 0;

	for (int k = 0; k < m; k++)
		keys[k] = palloc(ROWS_CAP * sizeof(uint64));
	heap_setscanlimits(scan, start, len);
	while (rows < ROWS_CAP && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cx);

		CHECK_FOR_INTERRUPTS();
		for (int k = 0; k < m; k++)
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, cand[k].hatt, &isnull);

			keys[k][rows] = cube_key(d, isnull, TupleDescAttr(RelationGetDescr(heap), cand[k].hatt - 1));
		}
		rows++;
		MemoryContextSwitchTo(old);
		MemoryContextReset(row_cx);
	}
	for (int x = 0; x < m; x++)
		for (int y = x + 1; y < m; y++)
		{
			uint32		limit = Min(rows / 10, SIFT_PAGG_ROWS_CELLS),
						cells = rows_cells(keys[x], keys[y], rows, limit, tab);

			if (cells <= limit)
			{
				pr[x * m + y].ok++;
				pr[x * m + y].cells += cells;
			}
		}
	for (int k = 0; k < m; k++)
		pfree(keys[k]);
	pfree(tab);
	MemoryContextDelete(row_cx);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

/*
 * array_lengths = auto: an index with an array column keeps the cardinality and the array_length(x, 1) of every array as two integer lanes, so that a test of either, and x IS [NOT] NULL, never decode the arrays: the images cost a hash, an
 * interning and a decompression of their own length for every row. Written as 'on' before the build, so that the build, its workers and a REINDEX share one schema.
 */
void
sift_col_auto_arrlen(Relation index)
{
	StringInfoData names;

	if (sift_col_options(index)->array_lengths != SIFT_COL_ARRLEN_AUTO)
		return;
	initStringInfo(&names);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);

		if (sift_col_is_array_type(att->atttypid))
			append_name(&names, NameStr(att->attname));
	}
	if (names.len == 0)
		return;
	sift_col_set_option(index, "array_lengths", "on");
	ereport(NOTICE, (errmsg("sift_col index %s keeps the lengths of the arrays %s", quote_identifier(RelationGetRelationName(index)), names.data),
					 errdetail("cardinality(x), array_length(x, 1) and x IS [NOT] NULL are read from two integer columns of each array, without the array: about a byte a row for each."),
					 errhint("ALTER INDEX %s SET (array_lengths = 'off'), then REINDEX, drops them.", quote_identifier(RelationGetRelationName(index)))));
}

void
sift_col_auto_rows(Relation heap, Relation index)
{
	const char *opt = sift_col_options_rollup_dims(index);
	const SiftColRel *rel;
	CubeCand	cand[SIFT_COL_MAX_COLS];
	RowsPair	pr[SIFT_PAGG_MAX_RDIMS * SIFT_PAGG_MAX_RDIMS] = {{0}};
	int			n,
				m = 0,
				nst,
				bx = -1,
				by = -1;
	BlockNumber nblocks;
	StringInfoData v;

	if (*opt != '\0' && pg_strcasecmp(opt, "auto") != 0)
		return;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	if ((n = cube_cands(index, rel->schema, cand)) < 2 || (nst = cube_sample(heap, cand, n)) == 0)
		return;
	/* the columns that fit, the fewest values first */
	qsort(cand, n, sizeof(CubeCand), cmp_cube_groups);
	for (int i = 0; i < n && m < SIFT_PAGG_MAX_RDIMS; i++)
		if (cand[i].fit * 100 >= (uint32) nst * ROWS_AGREE_PCT)
			cand[m++] = cand[i];
	if (m < 2)
		return;
	nblocks = RelationGetNumberOfBlocks(heap);
	for (int st = 0; st < nst; st++)
	{
		BlockNumber start,
					len;

		cube_stretch_at(nblocks, nst, st, &start, &len);
		rows_stretch(heap, start, len, cand, m, pr);
	}
	for (int x = 0; x < m; x++)
		for (int y = x + 1; y < m; y++)
			if (pr[x * m + y].ok * 100 >= (uint32) nst * ROWS_AGREE_PCT &&
				(bx < 0 || pr[x * m + y].cells < pr[bx * m + by].cells ||
				 (pr[x * m + y].cells == pr[bx * m + by].cells && cand[x].rank + cand[y].rank < cand[bx].rank + cand[by].rank)))
			{
				bx = x;
				by = y;
			}
	if (bx < 0)
		return;
	initStringInfo(&v);
	if (cand[bx].rank < cand[by].rank)
		appendStringInfo(&v, "auto: %s, %s", leaf_name(index, &rel->schema->cols[cand[bx].leaf]), leaf_name(index, &rel->schema->cols[cand[by].leaf]));
	else
		appendStringInfo(&v, "auto: %s, %s", leaf_name(index, &rel->schema->cols[cand[by].leaf]), leaf_name(index, &rel->schema->cols[cand[bx].leaf]));
	sift_col_set_option(index, "rollup_dims", v.data);
	ereport(NOTICE, (errmsg("sift_col index %s writes a rollup of the rows by %s", quote_identifier(RelationGetRelationName(index)), v.data + 6),
					 errdetail("A GROUP BY or a filter of both columns is read from the cells of each unit. Their combinations fit the cells a unit may keep (a tenth of its rows, at most %d) in at least %d%% of %d sampled stretches of about a unit, about %d of them a unit; the cells add to the index and to its build.",
							   SIFT_PAGG_ROWS_CELLS, ROWS_AGREE_PCT, nst, (int) (pr[bx * m + by].cells / pr[bx * m + by].ok)),
					 errhint("ALTER INDEX %s SET (rollup_dims = 'off'), then REINDEX, drops it; rollup_dims = 'a, b: measure' names the columns.", quote_identifier(RelationGetRelationName(index)))));
}
