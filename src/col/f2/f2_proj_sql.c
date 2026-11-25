/*
 * col/f2/f2_proj_sql.c - the SQL surface of the projections of a relation of format major 2: sift.create_projection, sift.drop_projection and the list behind sift.secondary_projections, and the
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
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/f2/col_meta.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_store.h"
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

#define DEFS_NATTS	3

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
			d->hash = sift_proj_spec_hash(canon);
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
	Datum		v[DEFS_NATTS] = {ObjectIdGetDatum(relid), CStringGetTextDatum(name), CStringGetTextDatum(canonical)};
	bool		n[DEFS_NATTS] = {false, false, false};

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

static Oid
heap_of(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX ? IndexGetRelation(RelationGetRelid(rel), false) : RelationGetRelid(rel);
}

static void
check_acl(Relation rel, AclMode mode)
{
	if (pg_class_aclcheck(heap_of(rel), GetUserId(), mode) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
}

static bool
is_f2_target(Relation rel)
{
	if (rel->rd_rel->relkind == RELKIND_INDEX)
		return sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR;
	return sift_tbl_is_table(rel) && sift_meta_resolve_major(rel) == SIFT_FORMAT2_MAJOR;
}

static Relation
open_target(Oid relid, LOCKMODE lock)
{
	Relation	rel = relation_open(sift_rel_must_exist(relid), lock);

	if (!is_f2_target(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift index or a sift_table of format major 2", RelationGetRelationName(rel)),
						errhint("Projections belong to a relation of format major 2.")));
	return rel;
}

Datum
sift_create_projection(PG_FUNCTION_ARGS)
{
	Relation	rel = open_target(PG_GETARG_OID(0), ShareUpdateExclusiveLock);
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

	check_acl(rel, ACL_MAINTAIN);
	if (name[0] == '\0' || strlen(name) >= SIFT_PROJ_NAME_LEN)
		ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("the name of a projection is 1 to %d bytes", SIFT_PROJ_NAME_LEN - 1)));
	if (!sift_proj_spec_parse(spec_text, &cols, &spec, err, sizeof(err)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid projection spec: %s", err)));
	sift_proj_spec_canonical(&spec, &cols, canonical);
	meta = sift_col_meta_read(rel);
	refs = sift_manifest_read_projs(rel, &meta, &nrefs);
	if (def_exists(RelationGetRelid(rel), name))
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("relation \"%s\" already has a projection \"%s\"", RelationGetRelationName(rel), name)));
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
	Relation	rel = open_target(PG_GETARG_OID(0), ShareUpdateExclusiveLock);
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		dropped;

	check_acl(rel, ACL_MAINTAIN);
	dropped = def_delete(RelationGetRelid(rel), name);
	dropped |= sift_proj_drop(rel, name);
	relation_close(rel, NoLock);
	PG_RETURN_BOOL(dropped);
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
		char		canonical[SIFT_PROJ_SPEC_MAX] = "";

		for (int k = 0; k < ndefs; k++)
			if (strcmp(defs[k].name, refs[i].name) == 0 && defs[k].spec_ok && defs[k].hash == refs[i].spec_hash)
				def = &defs[k];
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
		rel = open_target(relid, AccessShareLock);
		if (pg_class_aclcheck(heap_of(rel), GetUserId(), ACL_SELECT) == ACLCHECK_OK)
			list_relation(rsi, rel);
		relation_close(rel, AccessShareLock);
	}
	return (Datum) 0;
}
