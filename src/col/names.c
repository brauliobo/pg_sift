/*
 * col/names.c - what is kept under the name of a table or of a column follows the name when it changes (the event trigger sift.col_names_follow, after ALTER TABLE)
 *
 * A column that is renamed:
 *   - the index attribute of a sift_col index that holds it takes the name, as one made now would have it (PostgreSQL leaves it as it was, and pg_dump writes the name the table has now, so
 *     the index of a restore has it: the options of the index name its columns, and must name the same in both);
 *   - the column lists of the options of the index (key_cut, key_columns, rollup_columns, track_changes, ...) are written again with the new name, the one place a column of the table is
 *     named as text (sift.scope_follows names its columns too);
 *   - a column whose leaves are kept under its name in the segments (the documents of a jsonb column with paths) cannot follow: the ALTER TABLE is refused, and so is one that would
 *     leave the segments written with other leaf names than the index has now (the paths of a lane carry the name: tags#1).
 * A table that is renamed or moved to another schema: its value stores follow (col/vstore/vstore_rel.c), as the relations a restore adopts are found by the name the table gives them.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/vstore/vstore.h"
#include "core/restrict.h"
#include "core/store.h"

/* The options that hold a list of the columns of the index (or a column, or a per column setting) */
static const char *const column_options[] = {
	"key_cut", "key_columns", "rollup_columns", "rollup_leaves", "rollup_dims", "calendar_columns", "count_columns", "point_columns", "point_tid_columns",
	"order_columns", "order_array_columns", "order_array_group", "key_order", "hll_columns", "return_columns", "track_changes", "preagg", "compression"
};

/* Words of those options that are not a column's name */
static const char *const option_words[] = {"auto", "off", "on", "all", "ctid", "cube", "none", "default"};

static bool
is_column_option(const char *name)
{
	for (int i = 0; i < lengthof(column_options); i++)
		if (strcmp(column_options[i], name) == 0)
			return true;
	return false;
}

static bool
is_word(const char *tok, size_t len)
{
	for (int i = 0; i < lengthof(option_words); i++)
		if (strlen(option_words[i]) == len && strncmp(option_words[i], tok, len) == 0)
			return true;
	return false;
}

/* The text of an option with the names that are `from` written as `to`, NULL when it has none (a name is a run between the separators of the lists) */
static char *
rewrite_names(const char *text, const char *from, const char *to)
{
	StringInfoData out;
	size_t		flen = strlen(from);
	bool		changed = false;

	initStringInfo(&out);
	for (const char *p = text; *p != '\0';)
	{
		size_t		len = strcspn(p, ", \t:;()=");

		if (len == 0)
			appendStringInfoChar(&out, *p++);
		else
		{
			if (len == flen && strncmp(p, from, len) == 0 && !is_word(p, len))
			{
				appendStringInfoString(&out, quote_identifier(to));
				changed = true;
			}
			else
				appendBinaryStringInfo(&out, p, len);
			p += len;
		}
	}
	return changed ? out.data : NULL;
}

/* The options of the index that name the column `from`, with their new text: a list of DefElem */
static List *
renamed_options(Relation index, const char *from, const char *to)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(index)));
	bool		isnull;
	Datum		d = SysCacheGetAttr(RELOID, tp, Anum_pg_class_reloptions, &isnull);
	List	   *out = NIL;

	if (!isnull)
		foreach_node(DefElem, opt, untransformRelOptions(d))
		{
			char	   *text = is_column_option(opt->defname) && opt->arg != NULL ? rewrite_names(defGetString(opt), from, to) : NULL;

			if (text != NULL)
				out = lappend(out, makeDefElem(pstrdup(opt->defname), (Node *) makeString(text), -1));
		}
	ReleaseSysCache(tp);
	return out;
}

static void
rename_attribute(Oid indexoid, int pos, const char *to)
{
	Relation	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	HeapTuple	tp = SearchSysCacheCopy2(ATTNUM, ObjectIdGetDatum(indexoid), Int16GetDatum(pos + 1));

	namestrcpy(&((Form_pg_attribute) GETSTRUCT(tp))->attname, to);
	sift_cat_update(attrel, tp);
	heap_freetuple(tp);
	table_close(attrel, RowExclusiveLock);
	CommandCounterIncrement();
}

/*
 * The column at key position pos of the index is now called `to` in its table. The relcache entry of an index that is open keeps its attributes when the catalog changes (an index does not
 * change them), so the attribute is renamed with the index closed, which drops the entry, and the options are written to the index opened again.
 */
static void
follow_attribute(Oid indexoid, int pos, const char *to)
{
	Relation	index = index_open(indexoid, AccessExclusiveLock);
	char	   *from = pstrdup(NameStr(TupleDescAttr(RelationGetDescr(index), pos)->attname));
	char	   *name = pstrdup(RelationGetRelationName(index));
	bool		built = index->rd_rel->relkind == RELKIND_INDEX;
	bool		was_stale = built && sift_col_rel(index)->stale;
	List	   *renamed;

	if (TupleDescAttr(RelationGetDescr(index), pos)->atttypid == JSONBOID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot rename column \"%s\": sift_col index \"%s\" keeps the paths of its documents under that name", from, name),
						errhint("CREATE INDEX CONCURRENTLY the index again after the rename, then DROP INDEX %s.", quote_identifier(name))));
	renamed = renamed_options(index, from, to);
	index_close(index, NoLock);
	rename_attribute(indexoid, pos, to);
	index = index_open(indexoid, NoLock);
	sift_col_set_options(index, renamed);
	if (built && !was_stale && sift_col_rel(index)->stale)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot rename column \"%s\": the segments of sift_col index \"%s\" keep leaves under that name", from, name),
						errhint("CREATE INDEX CONCURRENTLY the index again after the rename, then DROP INDEX %s.", quote_identifier(name))));
	index_close(index, NoLock);
}

/* The sift_col indexes of the table whose key columns are named as the table names them now: the column attno is called `to` */
static void
follow_column(Oid heapoid, AttrNumber attno, const char *to)
{
	Relation	heap = table_open(heapoid, NoLock);

	foreach_oid(indexoid, RelationGetIndexList(heap))
	{
		Relation	index = index_open(indexoid, AccessExclusiveLock);
		List	   *positions = NIL;

		if (index->rd_rel->relam == sift_col_am_oid())
			for (int pos = 0; pos < IndexRelationGetNumberOfKeyAttributes(index); pos++)
				if (index->rd_index->indkey.values[pos] == attno && strcmp(NameStr(TupleDescAttr(RelationGetDescr(index), pos)->attname), to) != 0)
					positions = lappend_int(positions, pos);
		index_close(index, NoLock);
		foreach_int(pos, positions)
			follow_attribute(indexoid, pos, to);
	}
	table_close(heap, NoLock);
}

/* The columns of sift.scope_follows that name a column of the source table (the catalog table is not there before ALTER EXTENSION UPDATE) */
static void
follow_scope_follows(Oid heapoid, const char *from, const char *to)
{
	Oid			argtypes[3] = {OIDOID, TEXTOID, TEXTOID};
	Datum		values[3] = {ObjectIdGetDatum(heapoid), CStringGetTextDatum(from), CStringGetTextDatum(to)};

	if (SPI_execute("SELECT 1 FROM pg_catalog.pg_class WHERE oid = pg_catalog.to_regclass('sift.scope_follows')", true, 1) != SPI_OK_SELECT || SPI_processed == 0)
		return;
	(void) SPI_execute_with_args("UPDATE sift.scope_follows SET source_column = $3 WHERE source::pg_catalog.oid = $1 AND source_column = $2", 3, argtypes, values, NULL, false, 0);
	(void) SPI_execute_with_args("UPDATE sift.scope_follows SET expires = $3 WHERE source::pg_catalog.oid = $1 AND expires = $2", 3, argtypes, values, NULL, false, 0);
}

PG_FUNCTION_INFO_V1(sift_col_names_follow);

/* The event trigger after ALTER TABLE and ALTER MATERIALIZED VIEW (sift.col_names_follow) */
Datum
sift_col_names_follow(PG_FUNCTION_ARGS)
{
	Node	   *stmt;
	List	   *relids = NIL;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED), errmsg("not fired by the event trigger manager")));
	stmt = ((EventTriggerData *) fcinfo->context)->parsetree;
	if (!(IsA(stmt, RenameStmt) && (((RenameStmt *) stmt)->renameType == OBJECT_COLUMN || ((RenameStmt *) stmt)->renameType == OBJECT_TABLE ||
								   ((RenameStmt *) stmt)->renameType == OBJECT_MATVIEW)) &&
		!(IsA(stmt, AlterObjectSchemaStmt) && (((AlterObjectSchemaStmt *) stmt)->objectType == OBJECT_TABLE || ((AlterObjectSchemaStmt *) stmt)->objectType == OBJECT_MATVIEW)))
		PG_RETURN_NULL();
	SIFT_INVARIANT(SPI_connect() == SPI_OK_CONNECT, "sift: SPI_connect failed");
	SIFT_INVARIANT(SPI_execute("SELECT objid FROM pg_catalog.pg_event_trigger_ddl_commands() WHERE classid = 'pg_catalog.pg_class'::pg_catalog.regclass", true, 0) == SPI_OK_SELECT,
				   "sift: pg_event_trigger_ddl_commands failed");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;

		relids = lappend_oid(relids, DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull)));
	}
	foreach_oid(relid, relids)
	{
		if (IsA(stmt, RenameStmt) && ((RenameStmt *) stmt)->renameType == OBJECT_COLUMN)
		{
			const char *from = ((RenameStmt *) stmt)->subname;
			const char *to = ((RenameStmt *) stmt)->newname;

			foreach_oid(child, find_all_inheritors(relid, NoLock, NULL))
			{
				follow_column(child, get_attnum(child, to), to);
				follow_scope_follows(child, from, to);
			}
		}
		else
			sift_vstore_follow_names(relid);
	}
	SPI_finish();
	PG_RETURN_NULL();
}
