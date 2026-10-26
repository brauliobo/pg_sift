/*
 * col/keycols.c - the default of the option key_columns: an index created without it, or with key_columns = 'auto', keeps the keys of its
 * foreign key columns and of its columns named *_id, as far as they are int2, int4 or date, not unique, and not held by the cube. CREATE INDEX is the only
 * place it is decided: the columns are written to the option as 'auto: a, b', so that a rebuild keeps them, and the build keeps those that hold a key list or
 * a bitmap in half of the units or more. The same file decides the other defaults that read the columns' data before a build: the jsonb paths and the plain
 * columns that take the dimensions of the cube when there are more than it holds.
 */
#include "postgres.h"

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
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/ktab.h"
#include "col/pagg.h"
#include "col/preagg.h"
#include "col/seg.h"
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
		if (sift_col_key_shaped(heap, index, rel->schema, &rel->schema->cols[c]))
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
				cubed[SIFT_COL_MAX_COLS] = {0};
	uint64		bytes[SIFT_COL_MAX_COLS] = {0};
	SiftColLeafStats ls[SIFT_COL_MAX_COLS];
	StringInfoData kept,
				left,
				why;
	uint64		kept_bytes = 0;
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

				if (blob != NULL && len > 0 && (blob[0] == SIFT_CSET_PRES || blob[0] == SIFT_CSET_PLIST))
				{
					have[l]++;
					bytes[l] += len;
				}
			}
		}
		sift_cseg_close(seg);
		MemoryContextReset(cx);
	}
	initStringInfo(&kept);
	initStringInfo(&left);
	initStringInfo(&why);
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
		appendStringInfoString(&detail, "They are foreign key columns and columns named *_id of type int2, int4 or date that are not unique: a key list or bitmap of each unit, ");
		append_bytes(&detail, kept_bytes);
		appendStringInfoString(&detail, " together, which a count(DISTINCT) of one reads instead of its rows.");
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
	if (spec.len == 0)
		return;
	{
		StringInfoData v;
		const char *name = quote_identifier(RelationGetRelationName(index));

		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", spec.data);
		sift_col_set_option(index, "paths", v.data);
		ereport(NOTICE, (errmsg("sift_col index %s reads the jsonb paths %s", name, spec.data),
						 errdetail("They are the keys that are a scalar of one type in most of a sample of %d documents, a map of more than 4 keys of one type as a wildcard, an array of objects as a group; a REINDEX keeps them, and ALTER INDEX ... SET (paths = 'auto') then REINDEX reads the documents again.",
								   SIFT_COL_AUTO_PATHS_SAMPLE),
						 errhint("Write paths = '...' to fix them by hand.")));
	}
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
 * is written to the option as 'auto: a, b'; none fits, nothing. It is not the default: the cells grew the index by more than 3% and the build by more than 5%.
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

	if (pg_strcasecmp(opt, "auto") != 0)
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
