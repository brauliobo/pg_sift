/*
 * col/vstore/vstore_rel.c - the relation of a shared value store: find, adopt, create, depend, drop, follow the owner of the table
 *
 * One relation <table>_<column>_sift_values for each (table, column) whose values several sift_col indexes of the table share. It depends on the
 * table (DEPENDENCY_AUTO: DROP TABLE takes it) and each index that uses it depends on it (DEPENDENCY_NORMAL: it cannot be dropped under them). It is found by
 * that dependency; a restored dump has the relation but not the dependency, so the first index that is built adopts the relation of the first
 * free name that holds the column. It is made in the transaction of the index build that needs it, under a lock of the table that ends with the
 * transaction, so that two builds make one. The transaction that drops the last index that used it drops it too (gone_xact, the way the
 * track_changes triggers of col/track_drop.c go).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/vstore/vstore.h"
#include "core/store.h"

#define VSTORE_LOCK_CLASS	0x5D1C

static const char *vstore_comment =
	"Shared value store of column \"%s\" of table \"%s\", managed by sift: the distinct values that the sift_col indexes of the table share. "
	"It holds values only, no row ids or row data, and no more than those indexes hold. Do not modify, truncate, rewrite or drop it: "
	"it goes with the last index that uses it, or with the table. See sift.col_value_store_status.";

static Oid
vstore_am(void)
{
	return get_table_am_oid(SIFT_VSTORE_AM_NAME, false);
}

bool
sift_vstore_is_store(Oid relid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		is = false;

	if (HeapTupleIsValid(tp))
	{
		Form_pg_class cls = (Form_pg_class) GETSTRUCT(tp);

		is = cls->relkind == RELKIND_RELATION && OidIsValid(cls->relam) && cls->relam == get_table_am_oid(SIFT_VSTORE_AM_NAME, true);
		ReleaseSysCache(tp);
	}
	return is;
}

/* Whether the relation is a store of the column (its only attribute has the column's name) */
static bool
store_of_column(Oid relid, const char *col)
{
	char	   *an;

	if (!sift_vstore_is_store(relid))
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
		if (sift_vstore_is_store(oid) && fn(oid, arg))
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

Oid
sift_vstore_find_oid(Oid heapoid, AttrNumber attno)
{
	FindArg		f = {get_attname(heapoid, attno, false), InvalidOid};

	each_store(heapoid, find_visit, &f);
	return f.found;
}

Oid
sift_vstore_find(Relation heap, AttrNumber attno)
{
	return sift_vstore_find_oid(RelationGetRelid(heap), attno);
}

/* Whether the index depends on the store (it built or merged a segment that names it) */
static bool
index_uses(Oid index, Oid store)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		uses = false;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(index));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (!uses && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		uses = d->refclassid == RelationRelationId && d->refobjid == store && d->deptype == DEPENDENCY_NORMAL;
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return uses;
}

/* A string leaf that is a column of the table as it is: not a path of a jsonb, a carried or found leaf, a child of a group or a lane */
AttrNumber
sift_vstore_leaf_attno(Relation index, const SiftColDef *d)
{
	if (d->store != SIFT_COLS_STR || d->parent >= 0 || (d->path != NULL && d->path[0] != 0) || d->lanes > 1 || d->attno < 1 ||
		d->attno > index->rd_index->indnatts)
		return InvalidAttrNumber;
	return index->rd_index->indkey.values[d->attno - 1];
}

Oid
sift_vstore_leaf_store(Relation index, const SiftColDef *leaf)
{
	AttrNumber	attno = sift_vstore_leaf_attno(index, leaf);
	Oid			store;

	if (attno == InvalidAttrNumber)
		return InvalidOid;
	store = sift_vstore_find_oid(index->rd_index->indrelid, attno);
	return OidIsValid(store) && index_uses(RelationGetRelid(index), store) ? store : InvalidOid;
}

Oid *
sift_vstore_leaf_stores(Relation index, const SiftColSchema *schema)
{
	Oid		   *stores = NULL;

	if (index->rd_rel->relkind != RELKIND_INDEX)
		return NULL;
	for (int c = 0; c < schema->ncols; c++)
	{
		Oid			store = sift_vstore_leaf_store(index, &schema->cols[c]);

		if (OidIsValid(store))
		{
			if (stores == NULL)
				stores = palloc0(schema->ncols * sizeof(Oid));
			stores[c] = store;
		}
	}
	return stores;
}

static void
depend_on(Oid classid_obj, Oid obj, Oid ref, DependencyType type)
{
	ObjectAddress a,
				b;

	ObjectAddressSet(a, classid_obj, obj);
	ObjectAddressSet(b, RelationRelationId, ref);
	recordDependencyOn(&a, &b, type);
}

/* The names a store of the column can have: the first is the plain one, then those that ChooseRelationName picks when it is taken */
static char *
store_name(Relation heap, const char *col, int pass)
{
	char	   *label = pass == 0 ? SIFT_VSTORE_LABEL : psprintf("%s%d", SIFT_VSTORE_LABEL, pass);

	return makeObjectName(RelationGetRelationName(heap), col, label);
}

/* A restored dump has the relation and not its dependency on the table: the relation of the first name that holds the column is the store */
static Oid
adopt_by_name(Relation heap, const char *col)
{
	for (int pass = 0;; pass++)
	{
		Oid			cand = get_relname_relid(store_name(heap, col, pass), RelationGetNamespace(heap));

		if (!OidIsValid(cand))
			return InvalidOid;
		if (store_of_column(cand, col))
		{
			depend_on(RelationRelationId, cand, RelationGetRelid(heap), DEPENDENCY_AUTO);
			CommandCounterIncrement();
			return cand;
		}
	}
}

static Oid
create_store(Relation heap, const char *col)
{
	char	   *name = ChooseRelationName(RelationGetRelationName(heap), col, SIFT_VSTORE_LABEL, RelationGetNamespace(heap), false);
	TupleDesc	td = CreateTemplateTupleDesc(1);
	Oid			store;
	Relation	rel;
	SiftWriteTarget *wt;

	TupleDescInitEntry(td, 1, col, TEXTOID, -1, 0);
	store = heap_create_with_catalog(name, RelationGetNamespace(heap), InvalidOid, InvalidOid, InvalidOid, InvalidOid, heap->rd_rel->relowner, vstore_am(),
									 td, NIL, RELKIND_RELATION, heap->rd_rel->relpersistence, false, false, ONCOMMIT_NOOP, (Datum) 0, false, false, true,
									 InvalidOid, NULL);
	CommandCounterIncrement();
	depend_on(RelationRelationId, store, RelationGetRelid(heap), DEPENDENCY_AUTO);
	CreateComments(store, RelationRelationId, 0,
				   psprintf(vstore_comment, col, RelationGetRelationName(heap)));
	rel = table_open(store, AccessExclusiveLock);
	wt = sift_wt_begin(rel, SIFT_WRITE_BULK);
	sift_store_init_meta_ex(wt, 0, 0, 0, SIFT_VSTORE_META_FLAG);
	sift_wt_finish(wt);
	table_close(rel, NoLock);
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
sift_vstore_ensure(Relation heap, Relation index, AttrNumber attno)
{
	const char *col = get_attname(RelationGetRelid(heap), attno, false);
	Oid			store;

	/* one creator at a time for the table, to the end of the transaction; what the one before made is then in the catalog */
	LockDatabaseObject(RelationRelationId, RelationGetRelid(heap), VSTORE_LOCK_CLASS, ExclusiveLock);
	AcceptInvalidationMessages();
	InvalidateCatalogSnapshot();
	store = sift_vstore_find(heap, attno);
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
sift_vstore_drop_unused(Relation heap)
{
	List	   *unused = NIL;

	LockDatabaseObject(RelationRelationId, RelationGetRelid(heap), VSTORE_LOCK_CLASS, ExclusiveLock);
	AcceptInvalidationMessages();
	InvalidateCatalogSnapshot();
	each_store(RelationGetRelid(heap), unused_visit, &unused);
	foreach_oid(store, unused)
	{
		ObjectAddress obj;

		ObjectAddressSet(obj, RelationRelationId, store);
		performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
	}
}

/* The tables an index that was dropped in this transaction was on, to look at their stores before the commit (col/vstore/vstore_rel.c gone_xact) */
typedef struct Gone
{
	Oid			index;
	Oid			heap;
} Gone;

static Gone *gone;
static int	ngone,
			gone_cap;

static void
sift_vstore_dropping(Oid index, Oid heap)
{
	if (ngone == gone_cap)
	{
		gone_cap = gone_cap ? gone_cap * 2 : 16;
		gone = gone ? repalloc(gone, gone_cap * sizeof(Gone)) : MemoryContextAlloc(TopMemoryContext, gone_cap * sizeof(Gone));
	}
	gone[ngone++] = (Gone) {index, heap};
}

/* An index of ours is being dropped: its table is looked at for stores that no index uses before the transaction commits */
void
sift_vstore_note_drop(Oid relid)
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
		sift_vstore_dropping(relid, ((Form_pg_index) GETSTRUCT(tp))->indrelid);
		ReleaseSysCache(tp);
	}
}

static void
gone_xact(XactEvent event, void *arg)
{
	int			kept = 0;
	List	   *heaps = NIL;

	if (event == XACT_EVENT_ABORT)
		ngone = 0;
	if (event != XACT_EVENT_PRE_COMMIT || ngone == 0 || RecoveryInProgress())
		return;
	/* DROP INDEX CONCURRENTLY names its index in the first of its transactions and deletes it in the last: the ones still there wait for the next */
	for (int i = 0; i < ngone; i++)
		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(gone[i].index)))
			gone[kept++] = gone[i];
		else if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(gone[i].heap)))
			heaps = list_append_unique_oid(heaps, gone[i].heap);
	ngone = kept;
	foreach_oid(heapoid, heaps)
	{
		Relation	heap = try_table_open(heapoid, AccessShareLock);

		if (heap == NULL)
			continue;
		sift_vstore_drop_unused(heap);
		table_close(heap, NoLock);
	}
	if (heaps != NIL)
		CommandCounterIncrement();
}

void
sift_vstore_init(void)
{
	RegisterXactCallback(gone_xact, NULL);
	sift_vstore_ref_install();
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
		ATExecChangeOwner(store, owner, false, AccessExclusiveLock);
	return false;
}

void
sift_vstore_follow_owner(Oid heapoid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(heapoid));
	Oid			owner;

	if (!HeapTupleIsValid(tp))
		return;
	owner = ((Form_pg_class) GETSTRUCT(tp))->relowner;
	ReleaseSysCache(tp);
	each_store(heapoid, owner_visit, &owner);
}
