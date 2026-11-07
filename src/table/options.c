/*
 * table/options.c - the per-table options of a sift_table (docs/COL-TAM.md section 8)
 *
 * A table access method has no reloptions before PostgreSQL 20, so the options of one sift_table are rows of sift.table_option_values, set by
 * sift.table_set_option and read once with the cache entry of the table (sift_tbl_rel): the entry is replaced when the relcache invalidates the table, which
 * the setters do. The key is a sift.tblref: the OID of the table, written by pg_dump as its name and read back as the new OID (0, and the row skipped, for a
 * table the restore does not have). A value is checked by the setting of the same name, so type, range, unit and hook are the setting's own.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "utils/rel.h"

#include "col/seg.h"
#include "table/table.h"

typedef enum OptKind
{
	OPT_BOOL,
	OPT_INT,
	OPT_STRING
} OptKind;

typedef struct OptDesc
{
	const char *name;			/* the setting is sift.table_<name> */
	OptKind		kind;
	void	   *var;			/* the variable of the setting: where the table sets nothing */
	int			unit;			/* GUC_UNIT_* of an integer, to read a stored value back */
} OptDesc;

static const OptDesc opt_desc[SIFT_TBLOPT_N] = {
	[SIFT_TBLOPT_WRITE_BUFFER_LIMIT] = {"write_buffer_limit", OPT_INT, &sift_tbl_write_buffer_limit, GUC_UNIT_KB},
	[SIFT_TBLOPT_WIDE_ROWS] = {"wide_rows", OPT_BOOL, &sift_tbl_wide_rows, 0},
	[SIFT_TBLOPT_INLINE_PROMOTE] = {"inline_promote", OPT_BOOL, &sift_tbl_inline_promote, 0},
	[SIFT_TBLOPT_ASYNC_PROMOTE] = {"async_promote", OPT_BOOL, &sift_tbl_async_promote, 0},
	[SIFT_TBLOPT_BACKPRESSURE] = {"backpressure", OPT_INT, &sift_tbl_backpressure, 0},
	[SIFT_TBLOPT_HOT_UPDATE] = {"hot_update", OPT_BOOL, &sift_tbl_hot_update, 0},
	[SIFT_TBLOPT_BULK_GROUPS] = {"bulk_groups", OPT_BOOL, &sift_tbl_bulk_groups, 0},
	[SIFT_TBLOPT_DICT_LOOKUP] = {"dict_lookup", OPT_BOOL, &sift_col_table_lookup, 0},
	[SIFT_TBLOPT_KEY_TABLE] = {"key_table", OPT_BOOL, &sift_col_table_key_table, 0},
	[SIFT_TBLOPT_COMPRESSION] = {"compression", OPT_STRING, &sift_tbl_compression_spec, 0},
	[SIFT_TBLOPT_DEAD_REWRITE_PCT] = {"dead_rewrite_pct", OPT_INT, &sift_tbl_dead_rewrite_pct, 0},
	[SIFT_TBLOPT_AUTO_DEFRAG] = {"auto_defrag", OPT_BOOL, &sift_tbl_auto_defrag, 0},
	[SIFT_TBLOPT_AUTO_DEFRAG_FREE_PCT] = {"auto_defrag_free_pct", OPT_INT, &sift_tbl_auto_defrag_free_pct, 0},
	[SIFT_TBLOPT_AUTO_DEFRAG_BUDGET] = {"auto_defrag_budget", OPT_INT, &sift_tbl_auto_defrag_budget, GUC_UNIT_MB},
	[SIFT_TBLOPT_AUTO_DEFRAG_COALESCE] = {"auto_defrag_coalesce", OPT_BOOL, &sift_tbl_auto_defrag_coalesce, 0},
	[SIFT_TBLOPT_AUTO_DEFRAG_INDEXES] = {"auto_defrag_indexes", OPT_BOOL, &sift_tbl_auto_defrag_indexes, 0},
};

/* What the table sets: a bit and a value per option, and the text as stored (the canonical text of the setting) */
typedef struct SiftTblOpts
{
	uint32		set;
	int			val[SIFT_TBLOPT_N];
	char	   *text[SIFT_TBLOPT_N];
} SiftTblOpts;

PG_FUNCTION_INFO_V1(sift_tblref_in);
PG_FUNCTION_INFO_V1(sift_tblref_out);
PG_FUNCTION_INFO_V1(sift_table_option_values_trg);
PG_FUNCTION_INFO_V1(sift_table_option_names);
PG_FUNCTION_INFO_V1(sift_table_set_option);
PG_FUNCTION_INFO_V1(sift_table_get_option);
PG_FUNCTION_INFO_V1(sift_table_option_stored);
PG_FUNCTION_INFO_V1(sift_table_reset_option);

/* The entry of the table in the cache of every backend is replaced (a table that is being dropped has none to replace) */
static void
opt_invalidate(Oid relid)
{
	if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		CacheInvalidateRelcacheByRelid(relid);
}

/* ---- the key: the OID of a table, written as its name ---- */

/* The table of a name as pg_dump writes it (qualified); 0 when there is none, so that a restore without the table skips its rows. A number is a name here, never an OID */
Datum
sift_tblref_in(PG_FUNCTION_ARGS)
{
	List	   *names = stringToQualifiedNameList(PG_GETARG_CSTRING(0), fcinfo->context);

	if (names == NIL)
		PG_RETURN_NULL();
	PG_RETURN_OID(RangeVarGetRelid(makeRangeVarFromNameList(names), NoLock, true));
}

Datum
sift_tblref_out(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(regclassout, PG_GETARG_DATUM(0));
}

/*
 * BEFORE ROW on the rows of sift.table_option_values, for the statements that do not go through the setters (a restore, a repair by hand): the row of a table
 * that is not there (rel 0) is skipped, and the entry of the table in the cache of every backend is replaced.
 */
Datum
sift_table_option_values_trg(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	HeapTuple	ret = td->tg_trigtuple;
	bool		isnull;
	Oid			rel;

	SIFT_INVARIANT(CALLED_AS_TRIGGER(fcinfo) && TRIGGER_FIRED_BEFORE(td->tg_event) && TRIGGER_FIRED_FOR_ROW(td->tg_event), "sift.table_option_values_trg is a BEFORE ROW trigger");
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event))
	{
		rel = DatumGetObjectId(heap_getattr(td->tg_trigtuple, 1, td->tg_relation->rd_att, &isnull));
		if (OidIsValid(rel))
			opt_invalidate(rel);
		ret = td->tg_newtuple;
	}
	rel = DatumGetObjectId(heap_getattr(ret, 1, td->tg_relation->rd_att, &isnull));
	if (!OidIsValid(rel))
		return PointerGetDatum(NULL);
	opt_invalidate(rel);
	return PointerGetDatum(ret);
}

/* ---- the names ---- */

static int
opt_find(const char *name)
{
	for (int i = 0; i < SIFT_TBLOPT_N; i++)
		if (strcmp(opt_desc[i].name, name) == 0)
			return i;
	return -1;
}

static int
opt_require(const char *name)
{
	int			id = opt_find(name);
	StringInfoData list;

	if (id >= 0)
		return id;
	initStringInfo(&list);
	for (int i = 0; i < SIFT_TBLOPT_N; i++)
		appendStringInfo(&list, "%s%s", i ? ", " : "", opt_desc[i].name);
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("\"%s\" is not a per-table option of a sift_table", name),
					errhint("The options are: %s.", list.data)));
	return -1;
}

Datum
sift_table_option_names(PG_FUNCTION_ARGS)
{
	Datum		names[SIFT_TBLOPT_N];

	for (int i = 0; i < SIFT_TBLOPT_N; i++)
		names[i] = CStringGetTextDatum(opt_desc[i].name);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(names, SIFT_TBLOPT_N, TEXTOID));
}

static const char *
opt_setting(int id)
{
	return psprintf("sift.table_%s", opt_desc[id].name);
}

/* ---- the config table ---- */

static Oid
config_relid(void)
{
	Oid			nsp = get_namespace_oid("sift", true);

	return OidIsValid(nsp) ? get_relname_relid("table_option_values", nsp) : InvalidOid;
}

/* The only index of the config table: the unique one on (rel, name), with the operator class of oid */
static Oid
config_index(Relation cfg)
{
	List	   *l = RelationGetIndexList(cfg);
	Oid			idx = l != NIL ? linitial_oid(l) : InvalidOid;

	list_free(l);
	SIFT_INVARIANT(OidIsValid(idx), "sift.table_option_values has no index");
	return idx;
}

/* Reads the stored text of an option back into the value of its kind; false for a text that no longer parses (a row from another version) */
static bool
opt_parse(int id, const char *text, int *val)
{
	const OptDesc *d = &opt_desc[id];
	bool		b;

	switch (d->kind)
	{
		case OPT_BOOL:
			if (!parse_bool(text, &b))
				return false;
			*val = b;
			return true;
		case OPT_INT:
			return parse_int(text, val, d->unit, NULL);
		case OPT_STRING:
			*val = 0;
			return true;
	}
	return false;
}

void
sift_tbl_opts_load(SiftTblRel *st)
{
	SiftTblOpts *o = MemoryContextAllocZero(st->cxt, sizeof(SiftTblOpts));
	Oid			cfgid = config_relid();

	st->opts = o;
	if (!OidIsValid(cfgid))
		return;
	{
		Relation	cfg = table_open(cfgid, AccessShareLock);
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tup;

		ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(st->relid));
		scan = systable_beginscan(cfg, config_index(cfg), true, NULL, 1, &key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			bool		isnull;
			char	   *name = TextDatumGetCString(heap_getattr(tup, 2, RelationGetDescr(cfg), &isnull));
			char	   *txt = TextDatumGetCString(heap_getattr(tup, 3, RelationGetDescr(cfg), &isnull));
			int			id = opt_find(name);
			int			v;

			if (id >= 0 && opt_parse(id, txt, &v))
			{
				o->set |= (uint32) 1 << id;
				o->val[id] = v;
				o->text[id] = MemoryContextStrdup(st->cxt, txt);
			}
			else
				ereport(LOG, (errmsg("ignoring the option \"%s\" = \"%s\" of table %u in sift.table_option_values", name, txt, st->relid)));
		}
		systable_endscan(scan);
		table_close(cfg, AccessShareLock);
	}
}

bool
sift_tbl_opt_bool(const SiftTblRel *st, SiftTblOptId id)
{
	if (st->opts->set & ((uint32) 1 << id))
		return st->opts->val[id] != 0;
	return *(bool *) opt_desc[id].var;
}

int
sift_tbl_opt_int(const SiftTblRel *st, SiftTblOptId id)
{
	if (st->opts->set & ((uint32) 1 << id))
		return st->opts->val[id];
	return *(int *) opt_desc[id].var;
}

const char *
sift_tbl_opt_str(const SiftTblRel *st, SiftTblOptId id)
{
	if (st->opts->set & ((uint32) 1 << id))
		return st->opts->text[id];
	return *(char **) opt_desc[id].var;
}

/* ---- the SQL functions ---- */

static void
opt_errctx(void *arg)
{
	errcontext("while checking the option \"%s\" of a sift_table", (const char *) arg);
}

/* The canonical text of value under the setting of the option: its own check of type, range, unit and hook, then rolled back */
static char *
opt_check(int id, const char *value)
{
	int			nest = NewGUCNestLevel();
	ErrorContextCallback ctx = {.previous = error_context_stack,.callback = opt_errctx,.arg = (void *) opt_desc[id].name};
	char	   *canon;

	error_context_stack = &ctx;
	(void) set_config_option(opt_setting(id), value, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, ERROR, false);
	error_context_stack = ctx.previous;
	canon = GetConfigOptionByName(opt_setting(id), NULL, false);
	AtEOXact_GUC(false, nest);
	return canon;
}

static Relation
opt_open(Oid relid, LOCKMODE mode)
{
	Relation	rel = relation_open(relid, mode);

	sift_tbl_require(rel);
	return rel;
}

static void
opt_require_owner(Relation rel)
{
	if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
}

static void
opt_require_privilege(Relation rel)
{
	if (pg_class_aclmask(RelationGetRelid(rel), GetUserId(), ACL_SELECT | ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE | ACL_REFERENCES | ACL_TRIGGER, ACLMASK_ANY) == 0)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
}

/* The config table, opened for a change; the extension has to be 4.2 */
static Relation
config_open(void)
{
	Oid			cfgid = config_relid();

	if (!OidIsValid(cfgid))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("sift.table_option_values does not exist"),
						errhint("Run ALTER EXTENSION pg_sift UPDATE.")));
	return table_open(cfgid, RowExclusiveLock);
}

/* Deletes the row (rel, name) of cfg, or every row of rel when name is NULL; true when there was one */
static bool
config_delete(Relation cfg, Oid relid, const char *name)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(cfg, config_index(cfg), true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;

		if (name != NULL && strcmp(TextDatumGetCString(heap_getattr(tup, 2, RelationGetDescr(cfg), &isnull)), name) != 0)
			continue;
		CatalogTupleDelete(cfg, &tup->t_self);
		found = true;
	}
	systable_endscan(scan);
	return found;
}

/* A change takes effect for the statements that begin after it commits: the entry of the table is replaced in every backend */
static void
opt_changed(Oid relid)
{
	opt_invalidate(relid);
	CommandCounterIncrement();
}

Datum
sift_table_set_option(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel,
				cfg;
	int			id;
	char	   *canon;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("the table and the name of the option must not be NULL")));
	if (PG_ARGISNULL(2))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("the value of an option must not be NULL"),
						errhint("sift.table_reset_option takes the option off the table.")));
	rel = opt_open(relid, ShareUpdateExclusiveLock);
	opt_require_owner(rel);
	id = opt_require(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	canon = opt_check(id, text_to_cstring(PG_GETARG_TEXT_PP(2)));
	cfg = config_open();
	(void) config_delete(cfg, relid, opt_desc[id].name);
	values[0] = ObjectIdGetDatum(relid);
	values[1] = CStringGetTextDatum(opt_desc[id].name);
	values[2] = CStringGetTextDatum(canon);
	CatalogTupleInsert(cfg, heap_form_tuple(RelationGetDescr(cfg), values, nulls));
	table_close(cfg, NoLock);
	opt_changed(relid);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

Datum
sift_table_reset_option(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel,
				cfg;
	const char *name = NULL;

	if (PG_ARGISNULL(0))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("the table must not be NULL")));
	rel = opt_open(relid, ShareUpdateExclusiveLock);
	opt_require_owner(rel);
	if (!PG_ARGISNULL(1))
		name = opt_desc[opt_require(text_to_cstring(PG_GETARG_TEXT_PP(1)))].name;
	cfg = config_open();
	if (config_delete(cfg, relid, name))
		opt_changed(relid);
	table_close(cfg, NoLock);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

/* The text of the option on the table as stored, NULL when the table sets none */
static text *
opt_stored(Oid relid, const char *name, bool effective)
{
	Relation	rel = opt_open(relid, AccessShareLock);
	int			id = opt_require(name);
	SiftTblRel *st;
	const char *t;

	opt_require_privilege(rel);
	st = sift_tbl_rel(rel);
	t = st->opts->text[id];
	if (t == NULL && effective)
		t = GetConfigOptionByName(opt_setting(id), NULL, false);
	relation_close(rel, AccessShareLock);
	return t ? cstring_to_text(t) : NULL;
}

/* The value in force on the table: its own option, else the setting */
Datum
sift_table_get_option(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(opt_stored(PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), true));
}

/* The option as the table stores it; NULL when the table sets none */
Datum
sift_table_option_stored(PG_FUNCTION_ARGS)
{
	text	   *t = opt_stored(PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), false);

	if (t == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(t);
}
