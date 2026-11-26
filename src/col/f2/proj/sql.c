/*
 * col/f2/proj/sql.c - the SQL surface of the projections of a relation of format major 2: sift.create_projection, sift.drop_projection and the list behind sift.secondary_projections, and the
 * rows of sift.projection_defs (the definitions the parts of the directory are checked against: a projection whose definition is gone or no longer parses is STALE and never read).
 *
 * The rows are read and written through the heap directly, as the per-table options are (table/v1/options.c): the privilege that matters is the one on the relation, checked here
 * (ACL_MAINTAIN to create and drop, ACL_SELECT to list), not one on the config table.
 */
#include "core/host.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "col/f2/meta.h"
#include "col/f2/proj/proj.h"
#include "col/f2/store/store.h"
#include "col/am.h"
#include "col/pg.h"
#include "core/catname.h"
#include "core/inspect.h"
#include "core/meta_peek.h"
#include "core/restrict.h"
#include "table/dispatch.h"

PG_FUNCTION_INFO_V1(sift_create_projection);
PG_FUNCTION_INFO_V1(sift_drop_projection);
PG_FUNCTION_INFO_V1(sift_projection_list);

#define DEFS_NATTS	4

static Oid
defs_relid(void)
{
	Oid			nsp = get_namespace_oid("sift", true);

	return OidIsValid(nsp) ? get_relname_relid("projection_defs", nsp) : InvalidOid;
}

typedef struct DefRow
{
	Oid			rel;
	char	   *name;
	char	   *spec;
	ItemPointerData tid;
} DefRow;

/* The rows of the config table of one relation (InvalidOid: of every one), in a list of DefRow */
static List *
defs_rows(Oid relid)
{
	Oid			cfgid = defs_relid();
	List	   *rows = NIL;
	Relation	cfg;
	SysScanDesc scan;
	HeapTuple	tup;

	if (!OidIsValid(cfgid))
		return NIL;
	cfg = table_open(cfgid, AccessShareLock);
	scan = systable_beginscan(cfg, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Datum		v[DEFS_NATTS];
		bool		n[DEFS_NATTS];
		DefRow	   *r;

		heap_deform_tuple(tup, RelationGetDescr(cfg), v, n);
		if (relid != InvalidOid && DatumGetObjectId(v[0]) != relid)
			continue;
		r = palloc(sizeof(DefRow));
		r->rel = DatumGetObjectId(v[0]);
		r->name = TextDatumGetCString(v[1]);
		r->spec = TextDatumGetCString(v[2]);
		r->tid = tup->t_self;
		rows = lappend(rows, r);
	}
	systable_endscan(scan);
	table_close(cfg, AccessShareLock);
	return rows;
}

SiftProjDef *
sift_proj_defs_load(Relation rel, const SiftProjColumns *cols, int *n)
{
	List	   *rows = defs_rows(RelationGetRelid(rel));
	/* check-memory: allow one row per projection of the relation */
	SiftProjDef *defs = palloc(Max(list_length(rows), 1) * sizeof(SiftProjDef));
	ListCell   *lc;
	char		canon[SIFT_PROJ_SPEC_MAX];
	char		err[128];

	*n = 0;
	foreach(lc, rows)
	{
		const DefRow *r = lfirst(lc);
		SiftProjDef *d = &defs[(*n)++];

		memset(d, 0, sizeof(*d));
		strlcpy(d->name, r->name, sizeof(d->name));
		d->spec_ok = sift_proj_spec_parse(r->spec, cols, &d->spec, err, sizeof(err));
		if (d->spec_ok)
		{
			sift_proj_spec_canonical(&d->spec, cols, canon);
			d->hash = sift_proj_spec_hash(canon, &d->spec, cols);
		}
	}
	return defs;
}

static bool
def_exists(Oid relid, const char *name)
{
	ListCell   *lc;

	foreach(lc, defs_rows(relid))
		if (strcmp(((DefRow *) lfirst(lc))->name, name) == 0)
			return true;
	return false;
}

static void
def_insert(Oid relid, const char *name, const char *canonical)
{
	Relation	cfg = table_open(defs_relid(), RowExclusiveLock);
	char	   *rel_name = quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid));
	Datum		v[DEFS_NATTS] = {ObjectIdGetDatum(relid), CStringGetTextDatum(name), CStringGetTextDatum(canonical), CStringGetTextDatum(rel_name)};
	bool		n[DEFS_NATTS] = {false, false, false, false};

	sift_cat_insert(cfg, heap_form_tuple(RelationGetDescr(cfg), v, n));
	table_close(cfg, RowExclusiveLock);
	CommandCounterIncrement();
}

static bool
def_delete(Oid relid, const char *name)
{
	Relation	cfg = table_open(defs_relid(), RowExclusiveLock);
	bool		found = false;
	ListCell   *lc;

	foreach(lc, defs_rows(relid))
	{
		DefRow	   *r = lfirst(lc);

		if (strcmp(r->name, name) == 0)
		{
			sift_cat_delete(cfg, &r->tid);
			found = true;
		}
	}
	table_close(cfg, RowExclusiveLock);
	CommandCounterIncrement();
	return found;
}

static bool
is_f2_target(Relation rel)
{
	if (rel->rd_rel->relkind == RELKIND_INDEX)
		return sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR;
	return sift_tbl_is_table(rel) && sift_meta_resolve_major(rel) == SIFT_FORMAT2_MAJOR;
}

/*
 * The definition is a row of the catalog and follows the transaction; the directory of the relation does not. A projection the directory holds without a definition is what a rolled-back
 * create or a committed drop leaves: it is never read nor listed, and the next refresh drops it (f2_proj_build.c). The drops of the transaction are kept until it ends, so that a create
 * of the same name, which would take the projection out of the directory before the drop commits, is refused.
 */
typedef struct PendingDrop
{
	Oid			relid;
	char		name[SIFT_PROJ_NAME_LEN];
	SubTransactionId subid;
} PendingDrop;

static List *pending_drops;
static bool drops_hooked;

static bool
pending_has(Oid relid, const char *name)
{
	foreach_ptr(PendingDrop, p, pending_drops)
		if (p->relid == relid && strcmp(p->name, name) == 0)
			return true;
	return false;
}

static void
drops_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
		pending_drops = NIL;
}

static void
drops_subxact(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid, void *arg)
{
	ListCell   *lc;

	foreach(lc, pending_drops)
	{
		PendingDrop *p = lfirst(lc);

		if (p->subid != mysubid)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
			pending_drops = foreach_delete_current(pending_drops, lc);
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			p->subid = parentsubid;
	}
}

static void
pending_add(Oid relid, const char *name)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	PendingDrop *p = palloc0(sizeof(PendingDrop));

	if (!drops_hooked)
	{
		RegisterXactCallback(drops_xact, NULL);
		RegisterSubXactCallback(drops_subxact, NULL);
		drops_hooked = true;
	}
	p->relid = relid;
	strlcpy(p->name, name, sizeof(p->name));
	p->subid = GetCurrentSubTransactionId();
	pending_drops = lappend(pending_drops, p);
	MemoryContextSwitchTo(old);
}

static void
require_f2_target(Relation rel)
{
	sift_rel_reject_partitioned(rel);
	if (!is_f2_target(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift index or a sift_table of format major 2", RelationGetRelationName(rel)),
						errhint("Projections belong to a relation of format major 2.")));
}

/*
 * The relation of a change of its projections: the caller needs a maintain right, asked before the lock and again after it, and no standby takes the lock. The table of an index is locked
 * first, as VACUUM and the maintenance lock it: every writer of the projection directory then waits for the change to commit, and none drops a projection whose definition is not yet visible.
 */
static Relation
open_for_change(Oid relid)
{
	Relation	rel;

	if (RecoveryInProgress())
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot change projections during recovery")));
	sift_require_acl(relid, ACL_MAINTAIN);
	if (get_rel_relkind(sift_rel_must_exist(relid)) == RELKIND_INDEX)
		LockRelationOid(IndexGetRelation(relid, false), ShareUpdateExclusiveLock);
	rel = relation_open(relid, ShareUpdateExclusiveLock);
	sift_require_acl(relid, ACL_MAINTAIN);
	require_f2_target(rel);
	return rel;
}

/* The relation of a projection listing, locked when the caller may read its table and it is a relation that has projections: a stale or foreign OID is not listed */
static Relation
open_listed(Oid relid)
{
	char		kind = get_rel_relkind(relid);
	Oid			table;
	bool		missing = false;
	Relation	rel;

	if (kind == '\0')
		return NULL;
	table = kind == RELKIND_INDEX ? IndexGetRelation(relid, true) : relid;
	if (!OidIsValid(table) || pg_class_aclcheck_ext(table, GetUserId(), ACL_SELECT, &missing) != ACLCHECK_OK || missing)
		return NULL;
	rel = try_relation_open(relid, AccessShareLock);
	if (rel != NULL && (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE || !is_f2_target(rel)))
	{
		relation_close(rel, AccessShareLock);
		return NULL;
	}
	return rel;
}

/* The definitions of a table that name a column it renamed say the new name; the projection of such a definition is another one (its hash holds the name) and the next refresh builds it again */
void
sift_proj_defs_rename_column(Relation rel, const char *from, const char *to)
{
	SiftProjColumns now;
	SiftProjColumn *before;
	ListCell   *lc;

	if (!is_f2_target(rel))
		return;
	now = sift_proj_columns_of_store(rel, sift_f2_store_open(rel));
	before = palloc(Max(now.n, 1) * sizeof(SiftProjColumn));
	memcpy(before, now.col, now.n * sizeof(SiftProjColumn));
	for (int c = 0; c < now.n; c++)
		if (strcmp(now.col[c].name, to) == 0)
			before[c].name = from;
	foreach(lc, defs_rows(RelationGetRelid(rel)))
	{
		const DefRow *r = lfirst(lc);
		SiftProjSpec spec;
		char		err[128];
		char		canonical[SIFT_PROJ_SPEC_MAX];

		if (!sift_proj_spec_parse(r->spec, &(SiftProjColumns) {now.n, before}, &spec, err, sizeof(err)))
			continue;
		sift_proj_spec_canonical(&spec, &now, canonical);
		if (strcmp(canonical, r->spec) != 0)
		{
			(void) def_delete(RelationGetRelid(rel), r->name);
			def_insert(RelationGetRelid(rel), r->name, canonical);
		}
	}
}

/* ERROR when a projection of the table names the column in its spec (a SORT projection that keeps every column names none of them): the column goes only once the projection does */
void
sift_proj_defs_check_drop_column(Relation rel, const char *column)
{
	SiftProjColumns cols;
	bool	   *used;
	int			ndefs;
	SiftProjDef *defs;

	if (!is_f2_target(rel))
		return;
	cols = sift_proj_columns_of_store(rel, sift_f2_store_open(rel));
	used = palloc(Max(cols.n, 1) * sizeof(bool));
	defs = sift_proj_defs_load(rel, &cols, &ndefs);
	for (int i = 0; i < ndefs; i++)
	{
		SiftProjSpec named = defs[i].spec;

		named.all_columns = false;
		if (!defs[i].spec_ok)
			continue;
		sift_proj_spec_uses(&named, &cols, used);
		for (int c = 0; c < cols.n; c++)
			if (used[c] && strcmp(cols.col[c].name, column) == 0)
				ereport(ERROR, (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST), errmsg("cannot drop column \"%s\" of table \"%s\": the projection \"%s\" names it", column, RelationGetRelationName(rel), defs[i].name),
								errhint("Drop the projection first with SELECT sift.drop_projection('%s'::regclass, '%s').", RelationGetRelationName(rel), defs[i].name)));
	}
}

Datum
sift_create_projection(PG_FUNCTION_ARGS)
{
	Relation	rel = open_for_change(PG_GETARG_OID(0));
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *spec_text = text_to_cstring(PG_GETARG_TEXT_PP(2));
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftProjColumns cols = sift_proj_columns_of_store(rel, store);
	SiftProjSpec spec;
	char		err[256];
	char		canonical[SIFT_PROJ_SPEC_MAX];
	SiftColMetaPageData meta;
	int			nrefs;
	SiftManifestProjRef *refs;

	if (name[0] == '\0' || strlen(name) >= SIFT_PROJ_NAME_LEN)
		ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("the name of a projection is 1 to %d bytes", SIFT_PROJ_NAME_LEN - 1)));
	if (!sift_proj_spec_parse(spec_text, &cols, &spec, err, sizeof(err)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid projection spec: %s", err)));
	sift_proj_spec_canonical(&spec, &cols, canonical);
	meta = sift_col_meta_read(rel);
	refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	if (def_exists(RelationGetRelid(rel), name))
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("relation \"%s\" already has a projection \"%s\"", RelationGetRelationName(rel), name)));
	if (pending_has(RelationGetRelid(rel), name))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("relation \"%s\" dropped the projection \"%s\" in this transaction", RelationGetRelationName(rel), name),
						errhint("Commit the drop of the projection, then create it again.")));
	if (sift_proj_find(refs, nrefs, name) != NULL)
		(void) sift_proj_drop(rel, name);
	def_insert(RelationGetRelid(rel), name, canonical);
	sift_proj_create(rel, store, name, &spec, &cols, canonical);
	relation_close(rel, NoLock);
	PG_RETURN_BOOL(true);
}

Datum
sift_drop_projection(PG_FUNCTION_ARGS)
{
	Relation	rel = open_for_change(PG_GETARG_OID(0));
	Oid			relid = RelationGetRelid(rel);
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		dropped = def_delete(relid, name);

	relation_close(rel, NoLock);
	if (dropped)
		pending_add(relid, name);
	PG_RETURN_BOOL(dropped || pending_has(relid, name));
}

static const char *
kind_name(uint8 kind)
{
	return kind == SIFT_PROJ_SORT ? "sort" : "aggregate";
}

static const char *
state_name(uint8 state)
{
	return state == SIFT_PROJ_READY ? "ready" : state == SIFT_PROJ_BUILDING ? "building" : "stale";
}

static void
put_row(ReturnSetInfo *rsi, Oid relid, const char *name, uint8 kind, uint8 state, const char *spec, const SiftManifestProjRef *ref)
{
	Datum		v[8] = {ObjectIdGetDatum(relid), CStringGetTextDatum(name), CStringGetTextDatum(kind_name(kind)), CStringGetTextDatum(state_name(state)),
		CStringGetTextDatum(spec), Int64GetDatum(ref ? (int64) ref->total_rows : 0), Int64GetDatum(ref ? (int64) ref->total_bytes : 0),
		Int64GetDatum(ref ? (int64) ref->synced_version : 0)};
	bool		n[8] = {false, false, false, false, false, false, false, false};

	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
}

static void
list_relation(ReturnSetInfo *rsi, Relation rel)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftProjColumns cols = sift_proj_columns_of_store(rel, store);
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			nrefs,
				ndefs;
	SiftManifestProjRef *refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	SiftProjDef *defs = sift_proj_defs_load(rel, &cols, &ndefs);

	for (int i = 0; i < nrefs; i++)
	{
		const SiftProjDef *def = NULL;
		bool		named = false;
		char		canonical[SIFT_PROJ_SPEC_MAX] = "";

		for (int k = 0; k < ndefs; k++)
		{
			named |= strcmp(defs[k].name, refs[i].name) == 0;
			if (strcmp(defs[k].name, refs[i].name) == 0 && defs[k].spec_ok && defs[k].hash == refs[i].spec_hash)
				def = &defs[k];
		}
		if (!named)
			continue;
		if (def != NULL)
			sift_proj_spec_canonical(&def->spec, &cols, canonical);
		put_row(rsi, RelationGetRelid(rel), refs[i].name, refs[i].kind, def != NULL ? refs[i].state : SIFT_PROJ_STALE, canonical, &refs[i]);
	}
	for (int k = 0; k < ndefs; k++)
	{
		char		canonical[SIFT_PROJ_SPEC_MAX] = "";

		if (sift_proj_find(refs, nrefs, defs[k].name) != NULL)
			continue;
		if (defs[k].spec_ok)
			sift_proj_spec_canonical(&defs[k].spec, &cols, canonical);
		put_row(rsi, RelationGetRelid(rel), defs[k].name, defs[k].spec.kind, defs[k].spec_ok ? SIFT_PROJ_BUILDING : SIFT_PROJ_STALE, canonical, NULL);
	}
}

/* One row for each projection of each relation the invoker may read that has a definition or a directory entry: a definition the directory does not hold yet (a restored, reindexed or reset relation) is building */
Datum
sift_projection_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *rows = defs_rows(InvalidOid);
	List	   *seen = NIL;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, rows)
	{
		Oid			relid = ((DefRow *) lfirst(lc))->rel;
		Relation	rel;

		if (list_member_oid(seen, relid) || !SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
			continue;
		seen = lappend_oid(seen, relid);
		rel = open_listed(relid);
		if (rel == NULL)
			continue;
		list_relation(rsi, rel);
		relation_close(rel, AccessShareLock);
	}
	return (Datum) 0;
}
