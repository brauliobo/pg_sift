/*
 * col/keystore/rel.c - the relation of a scope key store: find, adopt, create, depend, drop, follow the owner of the table
 *
 * One relation <table>_<column>_sift_values for each (table, column) that a scope index of another table joins by, text, varchar or uuid. It depends on the
 * table (DEPENDENCY_AUTO: DROP TABLE takes it) and each index that uses it depends on it (DEPENDENCY_NORMAL: it cannot be dropped under them). It is found by
 * that dependency; a restored dump has the relation but not the dependency, so the first index that is built adopts the relation of the first
 * free name that holds the column. It is made in the transaction of the index build that needs it, under a lock of the table that ends with the
 * transaction, so that two builds make one. The transaction that drops the last index that used it drops it too (gone_xact, the way the
 * track_changes triggers of col/v1/track/drop.c go).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteDefine.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/scope/scope.h"
#include "col/keystore/keystore.h"
#include "core/meta_peek.h"
#include "core/restrict.h"
#include "core/store/store.h"
#include "core/catname.h"

#define KEYSTORE_LOCK_CLASS	0x5D1C

static const char *keystore_comment =
	"Scope key store of column \"%s\" of table \"%s\", managed by sift: the distinct keys that the scope indexes that join by it name by a code. "
	"It holds keys only, no row ids or row data. Do not modify, truncate, rewrite or drop it: "
	"it goes with the last index that uses it, or with the table.";

static Oid
keystore_am(void)
{
	return get_table_am_oid(SIFT_KEYSTORE_AM_NAME, false);
}

bool
sift_keystore_is_store(Oid relid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		is = false;

	if (HeapTupleIsValid(tp))
	{
		Form_pg_class cls = (Form_pg_class) GETSTRUCT(tp);

		is = cls->relkind == RELKIND_MATVIEW && OidIsValid(cls->relam) && cls->relam == get_table_am_oid(SIFT_KEYSTORE_AM_NAME, true);
		ReleaseSysCache(tp);
	}
	return is;
}

/* Whether the relation is a store of the column (its only attribute has the column's name) */
static bool
store_of_column(Oid relid, const char *col)
{
	char	   *an;

	if (!sift_keystore_is_store(relid))
		return false;
	an = get_attname(relid, 1, true);
	return an != NULL && strcmp(an, col) == 0;
}

/* The stores that depend on the table: calls fn(store, arg) for each; stops when fn returns true */
typedef bool (*StoreVisit) (Oid store, void *arg);

static void
each_store(Oid heapoid, StoreVisit fn, void *arg)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *found = NIL;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(heapoid));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == RelationRelationId && d->deptype == DEPENDENCY_AUTO && d->objsubid == 0 && d->refobjsubid == 0)
			found = lappend_oid(found, d->objid);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	foreach_oid(oid, found)
		if (sift_keystore_is_store(oid) && fn(oid, arg))
			break;
}

typedef struct FindArg
{
	const char *col;
	Oid			found;
} FindArg;

static bool
find_visit(Oid store, void *arg)
{
	FindArg    *f = arg;

	if (!store_of_column(store, f->col))
		return false;
	f->found = store;
	return true;
}

static Oid
keystore_find_oid(Oid heapoid, AttrNumber attno)
{
	FindArg		f = {get_attname(heapoid, attno, false), InvalidOid};

	each_store(heapoid, find_visit, &f);
	return f.found;
}

Oid
sift_keystore_find(Relation heap, AttrNumber attno)
{
	return keystore_find_oid(RelationGetRelid(heap), attno);
}

static void
depend_on(Oid classid_obj, Oid obj, Oid ref, DependencyType type)
{
	ObjectAddress a,
				b;

	ObjectAddressSet(a, classid_obj, obj);
	ObjectAddressSet(b, RelationRelationId, ref);
	sift_depend(&a, &b, type);
}

/* The names a store of the column can have: the first is the plain one, then those that ChooseRelationName picks when it is taken */
static char *
store_name(Relation heap, const char *col, int pass)
{
	char	   *label = pass == 0 ? SIFT_KEYSTORE_LABEL : psprintf("%s%d", SIFT_KEYSTORE_LABEL, pass);

	return makeObjectName(RelationGetRelationName(heap), col, label);
}

static void init_store(Oid store);

/* The relation of the first name that holds the column: the store, when a restored dump has it without its dependency on the table */
static Oid
store_by_name(Relation heap, const char *col)
{
	for (int pass = 0;; pass++)
	{
		Oid			cand = get_relname_relid(store_name(heap, col, pass), RelationGetNamespace(heap));

		if (!OidIsValid(cand) || store_of_column(cand, col))
			return cand;
	}
}

static Oid
adopt_by_name(Relation heap, const char *col)
{
	Oid			cand = store_by_name(heap, col);

	if (OidIsValid(cand))
	{
		depend_on(RelationRelationId, cand, RelationGetRelid(heap), DEPENDENCY_AUTO);
		CommandCounterIncrement();
		init_store(cand);
	}
	return cand;
}

/* The metapage of a store that has none: a new store, or one a restored dump made (the relation without its pages). The store has the layout of core/store/store.h whatever format new indexes have */
static void
init_store(Oid store)
{
	Relation	rel = table_open(store, AccessExclusiveLock);

	if (RelationGetNumberOfBlocks(rel) == 0)
	{
		SiftWriteTarget *wt;

		sift_tbl_push_new_format(SIFT_FORMAT_MAJOR);
		PG_TRY();
		{
			wt = sift_wt_begin(rel, SIFT_WRITE_BULK);
			sift_store_init_meta_ex(wt, 0, 0, 0, SIFT_KEYSTORE_META_FLAG);
			sift_wt_finish(wt);
		}
		PG_FINALLY();
		{
			sift_tbl_pop_new_format();
		}
		PG_END_TRY();
	}
	table_close(rel, NoLock);
}

/*
 * The store is a materialized view that is never populated: a publication (FOR ALL TABLES too) lists tables only, so an index built on a database
 * that publishes every table does not add a relation that subscribers lack, and pg_dump and pg_upgrade restore it empty (the view's query is a constant).
 */
static void
make_unpopulated_view(Oid store, const char *col)
{
	const char *sql = psprintf("SELECT NULL::text AS %s", quote_identifier(col));
	RawStmt    *raw = linitial_node(RawStmt, raw_parser(sql, RAW_PARSE_DEFAULT));
	Relation	rel = table_open(store, AccessExclusiveLock);

	DefineQueryRewrite(pstrdup("_RETURN"), store, NULL, CMD_SELECT, true, false, list_make1(parse_analyze_fixedparams(raw, sql, NULL, 0, NULL)));
	CommandCounterIncrement();
	SetMatViewPopulatedState(rel, false);
	table_close(rel, NoLock);
}

static Oid
create_store(Relation heap, const char *col)
{
	char	   *name = ChooseRelationName(RelationGetRelationName(heap), col, SIFT_KEYSTORE_LABEL, RelationGetNamespace(heap), false);
	TupleDesc	td = CreateTemplateTupleDesc(1);
	Oid			store;

	TupleDescInitEntry(td, 1, col, TEXTOID, -1, 0);
	sift_tupdesc_finalize(td);
	sift_tbl_push_new_format(SIFT_FORMAT_MAJOR);
	PG_TRY();
	{
		store = sift_create_matview(name, RelationGetNamespace(heap), heap->rd_rel->relowner, keystore_am(), td, heap->rd_rel->relpersistence);
	}
	PG_FINALLY();
	{
		sift_tbl_pop_new_format();
	}
	PG_END_TRY();
	CommandCounterIncrement();
	make_unpopulated_view(store, col);
	depend_on(RelationRelationId, store, RelationGetRelid(heap), DEPENDENCY_AUTO);
	CreateComments(store, RelationRelationId, 0,
				   psprintf(keystore_comment, col, RelationGetRelationName(heap)));
	init_store(store);
	return store;
}

static bool
index_depends_on(Oid index, Oid store)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(index));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (!found && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		found = d->refclassid == RelationRelationId && d->refobjid == store && d->deptype == DEPENDENCY_NORMAL;
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return found;
}

Oid
sift_keystore_ensure(Relation heap, Relation index, AttrNumber attno)
{
	const char *col = get_attname(RelationGetRelid(heap), attno, false);
	Oid			store;

	/* one creator at a time for the table, to the end of the transaction; what the one before made is then in the catalog */
	LockDatabaseObject(RelationRelationId, RelationGetRelid(heap), KEYSTORE_LOCK_CLASS, ExclusiveLock);
	AcceptInvalidationMessages();
	InvalidateCatalogSnapshot();
	store = sift_keystore_find(heap, attno);
	if (!OidIsValid(store))
		store = adopt_by_name(heap, col);
	if (!OidIsValid(store))
		store = create_store(heap, col);
	if (!index_depends_on(RelationGetRelid(index), store))
	{
		depend_on(RelationRelationId, RelationGetRelid(index), store, DEPENDENCY_NORMAL);
		CommandCounterIncrement();
	}
	return store;
}

/* The table whose keys the store holds (the one it depends on) */
static Oid
store_table(Oid store)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			heap = InvalidOid;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(store));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (!OidIsValid(heap) && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->deptype == DEPENDENCY_AUTO && d->refclassid == RelationRelationId)
			heap = d->refobjid;
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return heap;
}

/* A store that no index depends on */
static bool
unused_visit(Oid store, void *arg)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		used = false;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(store));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (!used && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		used = d->classid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL;
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	if (!used)
		*(List **) arg = lappend_oid(*(List **) arg, store);
	return false;
}

void
sift_keystore_drop_unused(Relation heap)
{
	List	   *unused = NIL;

	LockDatabaseObject(RelationRelationId, RelationGetRelid(heap), KEYSTORE_LOCK_CLASS, ExclusiveLock);
	AcceptInvalidationMessages();
	InvalidateCatalogSnapshot();
	each_store(RelationGetRelid(heap), unused_visit, &unused);
	foreach_oid(store, unused)
	{
		ObjectAddress obj;

		ObjectAddressSet(obj, RelationRelationId, store);
		sift_drop_object(&obj);
	}
}

PG_FUNCTION_INFO_V1(sift_col_value_store_drop_unused);

/*
 * sift._col_value_store_drop_unused(): the stores that no index uses, in every table of the database, are dropped. It is what the event trigger of the extension
 * runs after a DROP that took an index: a session that dropped the last index of a store without loading the library (DROP INDEX needs none) leaves nobody
 * to drop it (gone_xact is the library's)
 */
Datum
sift_col_value_store_drop_unused(PG_FUNCTION_ARGS)
{
	Oid			am = get_table_am_oid(SIFT_KEYSTORE_AM_NAME, true);
	Relation	drel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *heaps = NIL;

	sift_scope_journal_drop_unused();
	if (!OidIsValid(am))
		PG_RETURN_VOID();
	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(AccessMethodRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(am));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);
		Oid			heap = d->classid == RelationRelationId ? store_table(d->objid) : InvalidOid;

		if (OidIsValid(heap))
			heaps = list_append_unique_oid(heaps, heap);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	foreach_oid(heapoid, heaps)
	{
		Relation	heap = try_table_open(heapoid, AccessShareLock);

		if (heap != NULL)
		{
			sift_keystore_drop_unused(heap);
			table_close(heap, NoLock);
		}
	}
	PG_RETURN_VOID();
}

/* The tables an index that was dropped in this transaction was on, to look at their stores before the commit (gone_xact) */
static Oid *gone;
static int	ngone,
			gone_cap;

/* An index of ours is being dropped: its table is looked at for stores that no index uses before the transaction commits */
void
sift_keystore_note_drop(Oid relid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		ours;

	if (!HeapTupleIsValid(tp))
		return;
	ours = ((Form_pg_class) GETSTRUCT(tp))->relkind == RELKIND_INDEX && ((Form_pg_class) GETSTRUCT(tp))->relam == sift_col_am_oid();
	ReleaseSysCache(tp);
	if (!ours)
		return;
	tp = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		if (ngone == gone_cap)
		{
			gone_cap = gone_cap ? gone_cap * 2 : 16;
			gone = gone ? repalloc(gone, gone_cap * sizeof(Oid)) : MemoryContextAlloc(TopMemoryContext, gone_cap * sizeof(Oid));
		}
		gone[ngone++] = ((Form_pg_index) GETSTRUCT(tp))->indrelid;
		ReleaseSysCache(tp);
	}
}

static void
gone_xact(XactEvent event, void *arg)
{
	List	   *heaps = NIL;

	if (event == XACT_EVENT_ABORT)
		ngone = 0;
	if (event != XACT_EVENT_PRE_COMMIT || ngone == 0 || RecoveryInProgress())
		return;
	for (int i = 0; i < ngone; i++)
		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(gone[i])))
			heaps = list_append_unique_oid(heaps, gone[i]);
	ngone = 0;
	PushActiveSnapshot(GetTransactionSnapshot());	/* the commit of a VACUUM has none, and the catalog deletes want one */
	sift_scope_journal_drop_unused();
	foreach_oid(heapoid, heaps)
	{
		Relation	heap = try_table_open(heapoid, AccessShareLock);

		if (heap == NULL)
			continue;
		sift_keystore_drop_unused(heap);
		table_close(heap, NoLock);
	}
	PopActiveSnapshot();
	CommandCounterIncrement();
}

void
sift_keystore_init(void)
{
	RegisterXactCallback(gone_xact, NULL);
}

/* The owner of the stores of the table is the table's (ALTER TABLE ... OWNER TO), as an index's is */
static bool
owner_visit(Oid store, void *arg)
{
	Oid			owner = *(Oid *) arg;
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(store));
	bool		differs;

	if (!HeapTupleIsValid(tp))
		return false;
	differs = ((Form_pg_class) GETSTRUCT(tp))->relowner != owner;
	ReleaseSysCache(tp);
	if (differs)
		sift_change_owner(store, owner);
	return false;
}

void
sift_keystore_follow_owner(Oid heapoid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(heapoid));
	Oid			owner;

	if (!HeapTupleIsValid(tp))
		return;
	owner = ((Form_pg_class) GETSTRUCT(tp))->relowner;
	ReleaseSysCache(tp);
	each_store(heapoid, owner_visit, &owner);
}

/*
 * The stores of a table take the name its name and schema give them (<table>_<column>_sift_values, in the schema of the table) when it is renamed or moved, as the stores of a table made now
 * have it: a restored dump has the relation of a store without its dependency on the table, and the first index that is built adopts the relation of that name (store_by_name).
 */
static bool
names_visit(Oid store, void *arg)
{
	Relation	heap = arg;
	Oid			nsp = RelationGetNamespace(heap),
				was = get_rel_namespace(store);
	const char *col = get_attname(store, 1, false);
	char	   *name = NULL;

	for (int pass = 0; name == NULL; pass++)
	{
		char	   *cand = store_name(heap, col, pass);

		if (was == nsp && strcmp(cand, sift_rel_name(store)) == 0)
			return false;
		if (!OidIsValid(get_relname_relid(cand, nsp)))
			name = cand;
	}
	if (strcmp(name, sift_rel_name(store)) != 0)
	{
		RenameRelationInternal(store, name, true, false);
		CommandCounterIncrement();
	}
	if (was != nsp)
	{
		Relation	rel = relation_open(store, AccessExclusiveLock);
		ObjectAddresses *moved = new_object_addresses();

		AlterTableNamespaceInternal(rel, was, nsp, moved);
		free_object_addresses(moved);
		relation_close(rel, NoLock);
	}
	CreateComments(store, RelationRelationId, 0, psprintf(keystore_comment, col, RelationGetRelationName(heap)));
	return false;
}

void
sift_keystore_follow_names(Oid heapoid)
{
	Relation	heap = try_table_open(heapoid, AccessShareLock);

	if (heap == NULL)
		return;
	each_store(heapoid, names_visit, heap);
	table_close(heap, AccessShareLock);
}
