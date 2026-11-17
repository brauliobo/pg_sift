/*
 * col/names.c - what is kept under the name of a table, of an index or of a column follows the name when it changes (the event trigger sift.col_names_follow, after ALTER TABLE and ALTER INDEX)
 *
 * A column that is renamed:
 *   - the index attribute of a sift_col index that holds it takes the name, as one made now would have it (PostgreSQL leaves it as it was, and pg_dump writes the name the table has now, so
 *     the index of a restore has it: the options of the index name its columns, and must name the same in both);
 *   - the options of the indexes that name it as text are written again with the new name (col/names_text.c): the column lists of the index of the table (key_cut, key_columns, paths,
 *     track_changes, ..) and the connect and scope options of any index that names the column, its own or the key of the parent table or a column of the pairs;
 *   - a column whose leaves are kept under its name in the segments (the lengths of an array, the paths of a jsonb column, the flag of a connection) keeps them: the metapage takes the hash
 *     of the leaves with the new name and the segments go on with the names they were written with (sift_cschema_same_leaves);
 *   - the names in sift.scope_follows (the source column, the columns of the pairs), in sift.projections (the columns, the filter and the rollups of a definition) and in the options of a
 *     sift_table (sift.table_paths, sift.table_option_values) follow.
 * A table that is renamed or moved to another schema, and an index that is renamed: the options connect and scope of every sift_col index that name it follow, the definitions of
 * sift.projections, and the value stores of the table (col/vstore/vstore_rel.c), as the relations a restore adopts are found by the name the table gives them.
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/enc.h"
#include "col/names.h"
#include "col/pg.h"
#include "col/vstore/vstore.h"
#include "core/restrict.h"
#include "core/store.h"
#include "core/catname.h"
#include "core/inspect.h"

/* The table that ALTER TABLE .. SET SCHEMA is about to move, with the schema and the name it has until then: the event after the command does not tell */
static struct
{
	Oid			relid;
	char	   *schema;
	char	   *name;
}			before;

static bool following;

PG_FUNCTION_INFO_V1(sift_col_names_before);

/* The event trigger before ALTER TABLE: notes the schema and the name of the table that a RENAME or a SET SCHEMA is about to change */
Datum
sift_col_names_before(PG_FUNCTION_ARGS)
{
	Node	   *stmt;
	RangeVar   *rv = NULL;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED), errmsg("not fired by the event trigger manager")));
	stmt = ((EventTriggerData *) fcinfo->context)->parsetree;
	if (IsA(stmt, AlterObjectSchemaStmt) && (((AlterObjectSchemaStmt *) stmt)->objectType == OBJECT_TABLE || ((AlterObjectSchemaStmt *) stmt)->objectType == OBJECT_MATVIEW))
		rv = ((AlterObjectSchemaStmt *) stmt)->relation;
	before.relid = rv != NULL ? RangeVarGetRelid(rv, NoLock, true) : InvalidOid;
	if (OidIsValid(before.relid))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		before.schema = sift_namespace_name(get_rel_namespace(before.relid));
		before.name = sift_rel_name(before.relid);
		MemoryContextSwitchTo(old);
	}
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(sift_names_following);

/* Is the event trigger that follows a name at work: the guards of the catalog tables it keeps let its changes through */
Datum
sift_names_following(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(following);
}

static void
rename_attribute(Oid indexoid, int pos, const char *to)
{
	Relation	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	HeapTuple	tp = SearchSysCacheCopy2(ATTNUM, ObjectIdGetDatum(indexoid), Int16GetDatum(pos + 1));

	if (!HeapTupleIsValid(tp))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column %d of relation with OID %u does not exist", pos + 1, indexoid)));
	namestrcpy(&((Form_pg_attribute) GETSTRUCT(tp))->attname, to);
	sift_cat_update(attrel, tp);
	heap_freetuple(tp);
	table_close(attrel, RowExclusiveLock);
	CommandCounterIncrement();
}

/*
 * The column at key position pos of the index is now called c->to in its table. The relcache entry of an index that is open keeps its attributes when the catalog changes (an index does
 * not change them), so the attribute is renamed with the index closed, which drops the entry, and the options are written to the index opened again. The leaves the segments keep under
 * the name of the column stay the leaves of the index: its metapage is written for the new names.
 */
static void
follow_attribute(Oid indexoid, int pos, const SiftNameChange *c)
{
	Relation	index = index_open(indexoid, AccessExclusiveLock);
	bool		built = index->rd_rel->relkind == RELKIND_INDEX;
	List	   *renamed = sift_names_renamed_options(index, c);

	index_close(index, NoLock);
	rename_attribute(indexoid, pos, c->to);
	index = index_open(indexoid, NoLock);
	sift_col_set_options(index, renamed);
	if (built && !sift_col_rekey(index))
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": its segments cannot follow the rename of column \"%s\" to \"%s\"", sift_col_final_name(index), c->from, c->to),
				 errdetail("They do not hold the leaves the index names now, so it stays stale: reads refuse it until it is built again."),
				 errhint("CREATE INDEX CONCURRENTLY a new index like this one, then DROP INDEX %s: no scan is lost.",
						 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(index)), RelationGetRelationName(index)))));
	CommandCounterIncrement();
	index_close(index, NoLock);
}

/* The sift_col indexes of the table whose key columns are named as the table names them now: the column attno is called c->to */
static void
follow_column(Oid heapoid, AttrNumber attno, const SiftNameChange *c)
{
	Relation	heap = table_open(heapoid, NoLock);

	foreach_oid(indexoid, RelationGetIndexList(heap))
	{
		Relation	index = index_open(indexoid, AccessExclusiveLock);
		List	   *positions = NIL;

		if (index->rd_rel->relam == sift_col_am_oid())
			for (int pos = 0; pos < IndexRelationGetNumberOfKeyAttributes(index); pos++)
				if (index->rd_index->indkey.values[pos] == attno && strcmp(NameStr(TupleDescAttr(RelationGetDescr(index), pos)->attname), c->to) != 0)
					positions = lappend_int(positions, pos);
		index_close(index, NoLock);
		foreach_int(pos, positions)
			follow_attribute(indexoid, pos, c);
	}
	table_close(heap, NoLock);
}

/* The sift_col indexes with an option connect or scope: they name other tables, and their columns, as text */
static List *
referring_indexes(void)
{
	List	   *out = NIL;

	SIFT_INVARIANT(SPI_execute("SELECT c.oid FROM pg_catalog.pg_class c JOIN pg_catalog.pg_am a ON a.oid = c.relam AND a.amname = 'sift_col' WHERE c.relkind IN ('i', 'I') AND "
							   "EXISTS (SELECT 1 FROM unnest(c.reloptions) o WHERE o ~ '^(connect|scope)=') ORDER BY c.oid", true, 0) == SPI_OK_SELECT,
				   "sift: referring indexes failed");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;

		out = lappend_oid(out, DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull)));
	}
	return out;
}

/* The connect and scope options of every index, which name the table, its columns or its indexes as text */
static void
follow_references(const SiftNameChange *c)
{
	foreach_oid(indexoid, referring_indexes())
	{
		Relation	index = index_open(indexoid, ShareUpdateExclusiveLock);
		List	   *renamed = sift_names_renamed_options(index, c);

		if (renamed != NIL)
			sift_col_set_options(index, renamed);
		index_close(index, NoLock);
	}
}

/* Is the catalog table of the extension there (the library may be newer than the SQL of the database: before ALTER EXTENSION UPDATE) */
static bool
catalog_exists(const char *name)
{
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1] = {CStringGetTextDatum(name)};

	return SPI_execute_with_args("SELECT 1 FROM pg_catalog.pg_class WHERE oid = pg_catalog.to_regclass($1)", 1, argtypes, values, NULL, true, 1) == SPI_OK_SELECT && SPI_processed > 0;
}

static void
update_with(const char *sql, int nargs, Oid *argtypes, Datum *values)
{
	SIFT_INVARIANT(SPI_execute_with_args(sql, nargs, argtypes, values, NULL, false, 0) >= 0, "sift: the update of a name failed");
}

/*
 * The names a column renamed has in the catalog tables of the extension: sift.scope_follows (the column of the source that names the members or the keys, the column that expires pairs,
 * the columns of the pairs), the definitions of sift.projections (the columns, the filter and the rollups, written with the new name by the functions below) and the options of a
 * sift_table (the paths declared for a jsonb column, the order of its keys and the compression of a column).
 */
static void
follow_catalogs_column(Oid heapoid, const SiftNameChange *c)
{
	Oid			argtypes[3] = {OIDOID, TEXTOID, TEXTOID};
	Datum		values[3] = {ObjectIdGetDatum(heapoid), CStringGetTextDatum(c->from), CStringGetTextDatum(c->to)};

	if (catalog_exists("sift.scope_follows"))
	{
		update_with("UPDATE sift.scope_follows SET source_column = $3 WHERE source::pg_catalog.oid = $1 AND source_column = $2", 3, argtypes, values);
		update_with("UPDATE sift.scope_follows SET source_member = $3 WHERE source::pg_catalog.oid = $1 AND source_member = $2", 3, argtypes, values);
		update_with("UPDATE sift.scope_follows SET expires = $3 WHERE source::pg_catalog.oid = $1 AND expires = $2", 3, argtypes, values);
		update_with("UPDATE sift.scope_follows SET member_column = $3 WHERE pairs::pg_catalog.oid = $1 AND member_column = $2", 3, argtypes, values);
		update_with("UPDATE sift.scope_follows SET key_column = $3 WHERE pairs::pg_catalog.oid = $1 AND key_column = $2", 3, argtypes, values);
	}
	if (catalog_exists("sift.projections"))
		update_with("UPDATE sift.projections p SET leaves = sift._names_exprs(p.leaves, $2, $3), canonical = sift._names_exprs(p.canonical, $2, $3), "
					"rollups = sift._names_option('rollup_columns', p.rollups, $2, $3), filter = sift._names_expr(p.filter, $2, $3), "
					"canonical_filter = sift._names_expr(p.canonical_filter, $2, $3), "
					"options = COALESCE((SELECT jsonb_object_agg(o.key, sift._names_option(o.key, o.value, $2, $3)) FROM jsonb_each_text(p.options) o), p.options) "
					"WHERE p.source = sift.scope_qualified($1::pg_catalog.regclass)", 3, argtypes, values);
	if (catalog_exists("sift.table_paths"))
		update_with("UPDATE sift.table_paths SET spec = sift._names_option('paths', spec, $2, $3), attname = $3 WHERE relid::pg_catalog.oid = $1 AND attname = $2", 3, argtypes, values);
	if (catalog_exists("sift.table_option_values"))
		update_with("UPDATE sift.table_option_values SET value = sift._names_option(name, value, $2, $3) "
					"WHERE rel::pg_catalog.oid = $1 AND name IN ('key_order', 'compression')", 3, argtypes, values);
}

/* The definitions of sift.projections of a table that was renamed or moved name it as schema.name */
static void
follow_catalogs_table(Oid heapoid, const SiftNameChange *c)
{
	Oid			argtypes[3] = {OIDOID, TEXTOID, TEXTOID};
	Datum		values[3] = {ObjectIdGetDatum(heapoid), CStringGetTextDatum(quote_qualified_identifier(c->from_schema, c->from)), 0};

	values[2] = CStringGetTextDatum(quote_qualified_identifier(c->to_schema, c->to));
	if (catalog_exists("sift.projections"))
		update_with("UPDATE sift.projections SET source = $3 WHERE source = $2", 3, argtypes, values);
}

/* The statement of the event, as a change of a name; false when it changes none */
static bool
change_of(Node *stmt, Oid relid, SiftNameChange *c)
{
	RenameStmt *rs = IsA(stmt, RenameStmt) ? (RenameStmt *) stmt : NULL;
	AlterObjectSchemaStmt *as = IsA(stmt, AlterObjectSchemaStmt) ? (AlterObjectSchemaStmt *) stmt : NULL;
	char	   *schema = sift_namespace_name(get_rel_namespace(relid));

	*c = (SiftNameChange) {.relid = relid,.from_schema = schema,.to_schema = schema};
	if (rs != NULL && rs->renameType == OBJECT_COLUMN)
		c->kind = SIFT_NAME_COLUMN, c->from = rs->subname, c->to = rs->newname;
	else if (rs != NULL && (rs->renameType == OBJECT_TABLE || rs->renameType == OBJECT_MATVIEW))
		c->kind = SIFT_NAME_TABLE, c->from = rs->relation->relname, c->to = rs->newname;
	else if (rs != NULL && rs->renameType == OBJECT_INDEX)
		c->kind = SIFT_NAME_INDEX, c->relid = IndexGetRelation(relid, false), c->from = rs->relation->relname, c->to = rs->newname;
	else if (as != NULL && (as->objectType == OBJECT_TABLE || as->objectType == OBJECT_MATVIEW))
	{
		SIFT_INVARIANT(before.relid == relid, "sift: the schema of table %u before SET SCHEMA is not known", relid);
		c->kind = SIFT_NAME_TABLE, c->from_schema = before.schema, c->from = c->to = before.name, c->to_schema = as->newschema;
	}
	else
		return false;
	return true;
}

/* The objects of the commands the event is about */
static List *
command_objects(void)
{
	List	   *relids = NIL;

	SIFT_INVARIANT(SPI_execute("SELECT objid FROM pg_catalog.pg_event_trigger_ddl_commands() WHERE classid = 'pg_catalog.pg_class'::pg_catalog.regclass", true, 0) == SPI_OK_SELECT,
				   "sift: pg_event_trigger_ddl_commands failed");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;

		relids = lappend_oid(relids, DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull)));
	}
	return relids;
}

static void
follow(Node *stmt)
{
	foreach_oid(relid, command_objects())
	{
		SiftNameChange c;

		if (!change_of(stmt, relid, &c))
			return;
		if (c.kind == SIFT_NAME_COLUMN)
			foreach_oid(child, find_all_inheritors(relid, NoLock, NULL))
			{
				SiftNameChange cc = c;

				cc.relid = child;
				follow_column(child, get_attnum(child, c.to), &cc);
				follow_catalogs_column(child, &cc);
				follow_references(&cc);
			}
		else
		{
			if (c.kind == SIFT_NAME_TABLE)
			{
				sift_vstore_follow_names(c.relid);
				follow_catalogs_table(c.relid, &c);
			}
			follow_references(&c);
		}
	}
}

PG_FUNCTION_INFO_V1(sift_col_names_follow);

/* The event trigger after ALTER TABLE, ALTER MATERIALIZED VIEW and ALTER INDEX (sift.col_names_follow) */
Datum
sift_col_names_follow(PG_FUNCTION_ARGS)
{
	Node	   *stmt;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED), errmsg("not fired by the event trigger manager")));
	stmt = ((EventTriggerData *) fcinfo->context)->parsetree;
	if (!IsA(stmt, RenameStmt) && !IsA(stmt, AlterObjectSchemaStmt))
		PG_RETURN_NULL();
	SIFT_INVARIANT(SPI_connect() == SPI_OK_CONNECT, "sift: SPI_connect failed");
	following = true;
	PG_TRY();
	{
		follow(stmt);
	}
	PG_FINALLY();
	{
		following = false;
	}
	PG_END_TRY();
	SPI_finish();
	PG_RETURN_NULL();
}

typedef struct ExprRefs
{
	const char *from;
	List	   *locations;
} ExprRefs;

static bool
collect_refs(Node *node, void *context)
{
	ExprRefs   *refs = context;

	if (node == NULL)
		return false;
	if (IsA(node, ColumnRef))
	{
		Node	   *last = llast(((ColumnRef *) node)->fields);

		if (IsA(last, String) && strcmp(strVal(last), refs->from) == 0)
			refs->locations = lappend_int(refs->locations, ((ColumnRef *) node)->location);
		return false;
	}
	return raw_expression_tree_walker(node, collect_refs, refs);
}

static int
cmp_int(const ListCell *a, const ListCell *b)
{
	return (lfirst_int(a) > lfirst_int(b)) - (lfirst_int(a) < lfirst_int(b));
}

/* The end of the identifier at p (a quoted one or a run of the characters of a name) */
static const char *
ident_end(const char *p)
{
	if (*p == '"')
	{
		for (p++; *p != '\0'; p++)
			if (*p == '"' && p[1] != '"')
				return p + 1;
			else if (*p == '"')
				p++;
		return p;
	}
	while (isalnum((unsigned char) *p) || *p == '_' || *p == '$' || IS_HIGHBIT_SET(*p))
		p++;
	return p;
}

/*
 * The expression (an index column, a filter) with the column from written as to: the references to the column that the parser finds, not the words of the text (a string, a function or a
 * type of the same name stays). A reference may be qualified: the last name of it is the column.
 */
static char *
rewrite_expr(const char *text, const char *from, const char *to)
{
	static const char prefix[] = "SELECT ";
	ExprRefs	refs = {.from = from};
	StringInfoData out;
	char	   *sql = psprintf("%s%s", prefix, text);
	int			at = 0;
	List	   *stmts = raw_parser(sql, RAW_PARSE_DEFAULT);

	if (list_length(stmts) != 1 || !IsA(linitial_node(RawStmt, stmts)->stmt, SelectStmt))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("sift._names_expr: the argument is not one expression")));
	(void) raw_expression_tree_walker(linitial_node(RawStmt, stmts)->stmt, collect_refs, &refs);
	list_sort(refs.locations, cmp_int);
	initStringInfo(&out);
	foreach_int(loc, refs.locations)
	{
		const char *p = sql + loc,
				   *end = ident_end(p),
				   *last = p;

		for (; *end == '.'; end = ident_end(last))
			last = end + 1;
		appendBinaryStringInfo(&out, sql + at, last - sql - at);
		appendStringInfoString(&out, quote_identifier(to));
		at = end - sql;
	}
	appendStringInfoString(&out, sql + at);
	return out.data + sizeof(prefix) - 1;
}

PG_FUNCTION_INFO_V1(sift_names_expr);

/* sift._names_expr(expression text, from name, to name): the expression with the column from named to; NULL for a NULL expression */
Datum
sift_names_expr(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	SIFT_ARG_REQUIRED(1, "sift._names_expr", "from_name");
	SIFT_ARG_REQUIRED(2, "sift._names_expr", "to_name");
	PG_RETURN_TEXT_P(cstring_to_text(rewrite_expr(text_to_cstring(PG_GETARG_TEXT_PP(0)), text_to_cstring(PG_GETARG_TEXT_PP(1)), text_to_cstring(PG_GETARG_TEXT_PP(2)))));
}

PG_FUNCTION_INFO_V1(sift_names_exprs);

/* ... of each element of an array of expressions */
Datum
sift_names_exprs(PG_FUNCTION_ARGS)
{
	ArrayType  *a;
	Datum	   *elems;
	int			n;
	char	   *from,
			   *to;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	SIFT_ARG_REQUIRED(1, "sift._names_exprs", "from_name");
	SIFT_ARG_REQUIRED(2, "sift._names_exprs", "to_name");
	a = PG_GETARG_ARRAYTYPE_P(0);
	from = text_to_cstring(PG_GETARG_TEXT_PP(1));
	to = text_to_cstring(PG_GETARG_TEXT_PP(2));
	deconstruct_array_builtin(a, TEXTOID, &elems, NULL, &n);
	for (int i = 0; i < n; i++)
		elems[i] = CStringGetTextDatum(rewrite_expr(TextDatumGetCString(elems[i]), from, to));
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(elems, n, TEXTOID));
}

PG_FUNCTION_INFO_V1(sift_names_option);

/* sift._names_option(option text, value text, from name, to name): the value of the sift_col option with the column from named to; NULL for a NULL value */
Datum
sift_names_option(PG_FUNCTION_ARGS)
{
	char	   *value,
			   *written;

	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	SIFT_ARG_REQUIRED(0, "sift._names_option", "option");
	SIFT_ARG_REQUIRED(2, "sift._names_option", "from_name");
	SIFT_ARG_REQUIRED(3, "sift._names_option", "to_name");
	value = text_to_cstring(PG_GETARG_TEXT_PP(1));
	written = sift_names_list_option(text_to_cstring(PG_GETARG_TEXT_PP(0)), value, text_to_cstring(PG_GETARG_TEXT_PP(2)), text_to_cstring(PG_GETARG_TEXT_PP(3)));
	PG_RETURN_TEXT_P(cstring_to_text(written != NULL ? written : value));
}
