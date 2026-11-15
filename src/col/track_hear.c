/*
 * col/track_hear.c - the triggers of an index with track_changes as a set: which tables must be heard (the table of the index, and for a statement trigger every table above it), which triggers the index
 * has, what they lack, and the locks that making and dropping them take. The completeness of the log that they make is decided in col/track.c.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "col/am.h"
#include "col/track_hear.h"
#include "core/restrict.h"

/* Every table above relid, through partitions and inheritance, appended to *parents */
static void
add_parents(Oid relid, List **parents)
{
	Relation	rel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *direct = NIL;

	ScanKeyInit(&key, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, InheritsRelidSeqnoIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		direct = lappend_oid(direct, ((Form_pg_inherits) GETSTRUCT(tup))->inhparent);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(parent, direct)
		if (!list_member_oid(*parents, parent))
		{
			*parents = lappend_oid(*parents, parent);
			add_parents(parent, parents);
		}
}

/* The tables whose statements the triggers of an index on heap must hear: the table, and (a statement trigger) the tables above it */
List *
sift_col_hear_tables(Oid heapoid, bool row)
{
	List	   *tables = list_make1_oid(heapoid);

	if (!row)
		add_parents(heapoid, &tables);
	return tables;
}

static void
read_trigger(Oid funcoid, Oid toid, List **trigs, bool row)
{
	Relation	trel = table_open(TriggerRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tp;

	ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(toid));
	scan = systable_beginscan(trel, TriggerOidIndexId, true, NULL, 1, &key);
	if (HeapTupleIsValid(tp = systable_getnext(scan)))
	{
		Form_pg_trigger tg = (Form_pg_trigger) GETSTRUCT(tp);

		if (tg->tgtype & (TRIGGER_TYPE_DELETE | TRIGGER_TYPE_UPDATE))
		{
			HearTrig   *h = palloc0(sizeof(HearTrig));
			bool		shape = tg->tgfoid == funcoid && !tg->tgdeferrable && (tg->tgtype & (TRIGGER_TYPE_BEFORE | TRIGGER_TYPE_INSTEAD)) == 0 &&
				((tg->tgtype & TRIGGER_TYPE_ROW) != 0) == row && tg->tgattr.dim1 == 0;

			h->oid = toid;
			h->relid = tg->tgrelid;
			h->name = tg->tgname;
			h->event = (tg->tgtype & TRIGGER_TYPE_DELETE) ? 0 : 1;
			h->witness = HeapTupleHeaderGetRawXmin(tp->t_data);
			h->ok = shape && tg->tgenabled == TRIGGER_FIRES_ALWAYS;
			h->origin = shape && tg->tgenabled == TRIGGER_FIRES_ON_ORIGIN;
			*trigs = lappend(*trigs, h);
		}
	}
	systable_endscan(scan);
	table_close(trel, AccessShareLock);
}

/* The triggers of the index, whatever table they are on (NIL while the extension is not updated to the release that has the function) */
List *
sift_col_hear_find(Relation index, bool row)
{
	Relation	drel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *trigs = NIL;
	Oid			funcoid = sift_col_track_function(true);

	/* the extension is not updated to the release that has the function yet (the library is: ALTER INDEX of any option asks): no trigger can be its own */
	if (!OidIsValid(funcoid))
		return NIL;
	drel = table_open(DependRelationId, AccessShareLock);
	/* this release: the index depends on its triggers */
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(index)));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->refclassid == TriggerRelationId && d->deptype == DEPENDENCY_NORMAL && d->refobjsubid == 0)
			read_trigger(funcoid, d->refobjid, &trigs, row);
	}
	systable_endscan(scan);
	/* an earlier release (until ALTER EXTENSION UPDATE converts them): the triggers depend on the index */
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(index)));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == TriggerRelationId && d->deptype == DEPENDENCY_AUTO && d->refobjsubid == 0)
			read_trigger(funcoid, d->objid, &trigs, row);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return trigs;
}

static int
trig_cmp(const ListCell *a, const ListCell *b)
{
	const HearTrig *x = lfirst(a);
	const HearTrig *y = lfirst(b);

	return x->relid != y->relid ? (x->relid < y->relid ? -1 : 1) : (x->oid < y->oid ? -1 : x->oid > y->oid);
}

/* The identity of the triggers of one event: the xmin of the only one, and for several the combination of theirs in the order of their tables */
uint32
sift_col_hear_witness(List *trigs, int event)
{
	uint32		w = 0;
	bool		first = true;
	List	   *sorted = list_copy(trigs);

	list_sort(sorted, trig_cmp);
	foreach_ptr(HearTrig, t, sorted)
		if (t->event == event)
		{
			w = first ? t->witness : hash_combine(w, t->witness);
			first = false;
		}
	return w;
}

/* Each trigger of the event is as the option made it (or as ENABLE TRIGGER left it), and the identity they have together: what a log that was complete names, whichever tables they are on */
bool
sift_col_hear_all_ok(List *trigs, int event, uint32 *witness)
{
	bool		ok = true;

	foreach_ptr(HearTrig, t, trigs)
		ok &= t->event != event || t->ok;
	*witness = sift_col_hear_witness(trigs, event);
	return ok;
}

/* What the triggers are for each event, against the tables that must be heard */
void
sift_col_hear_read(Oid heapoid, Relation index, List *trigs, Hear h[2])
{
	List	   *tables = sift_col_hear_tables(heapoid, sift_col_track_ctid(index));

	for (int e = 0; e < 2; e++)
	{
		List	   *mine = NIL;
		bool		whole,
					ok = true,
					enabled = true,
					any_origin = false;

		foreach_ptr(HearTrig, t, trigs)
			if (t->event == e)
				mine = lappend(mine, t);
		whole = list_length(mine) == list_length(tables);
		foreach_ptr(HearTrig, t, mine)
		{
			whole &= list_member_oid(tables, t->relid);
			ok &= t->ok;
			enabled &= t->ok || t->origin;
			any_origin |= t->origin;
		}
		/* one for each table: as many as the tables, each on one of them, and none twice */
		foreach_oid(tbl, tables)
		{
			int			on = 0;

			foreach_ptr(HearTrig, t, mine)
				on += t->relid == tbl;
			whole &= on == 1;
		}
		h[e] = (Hear) {.n = list_length(mine), .whole = whole, .ok = whole && ok, .origin = whole && enabled && any_origin, .witness = sift_col_hear_witness(mine, e)};
	}
}

/*
 * The lock a trigger of a table takes to be made (ShareRowExclusiveLock) or dropped (AccessExclusiveLock), for a table above that of the index: taken without waiting for long, and never for good. A
 * statement on the parent locks the parent, then its partitions, and the build or the DDL on one of them holds that table first: waiting for the parent then would be a deadlock with a writer that
 * waits for the table, which the lock manager ends by cancelling one of them, the writer as likely as not. The caller leaves what it could not do to the maintenance worker (sift_col_track_rearm).
 */
#define PARENT_LOCK_TRIES	20
#define PARENT_LOCK_NAP_US	10000

bool
sift_col_hear_lock_briefly(Oid relid, LOCKMODE mode)
{
	for (int tries = 0; !ConditionalLockRelationOid(relid, mode); tries++)
	{
		if (tries == PARENT_LOCK_TRIES)
			return false;
		pg_usleep(PARENT_LOCK_NAP_US);
		CHECK_FOR_INTERRUPTS();
	}
	return true;
}

/* The locks that dropping the triggers an index has and making those it needs take, on the tables other than that of the index */
bool
sift_col_hear_lock_all(Relation heap, Relation index)
{
	foreach_ptr(HearTrig, t, sift_col_hear_find(index, false))
		if (t->relid != RelationGetRelid(heap) && !sift_col_hear_lock_briefly(t->relid, AccessExclusiveLock))
			return false;
	foreach_oid(tbl, sift_col_hear_tables(RelationGetRelid(heap), sift_col_track_ctid(index)))
		if (tbl != RelationGetRelid(heap) && !sift_col_hear_lock_briefly(tbl, ShareRowExclusiveLock))
			return false;
	return true;
}

static void
create_trigger(Oid relid, Relation index, bool update)
{
	CreateTrigStmt *stmt = makeNode(CreateTrigStmt);
	TriggerTransition *tt = makeNode(TriggerTransition);
	Oid			funcoid = sift_col_track_function(false);
	bool		row = sift_col_track_ctid(index);
	ObjectAddress trig,
				idx;

	tt->name = "old_rows";
	tt->isNew = false;
	tt->isTable = true;
	stmt->trigname = psprintf(update ? "sift_track_update_%s" : "sift_track_delete_%s", RelationGetRelationName(index));
	stmt->relation = makeRangeVar(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid), -1);
	stmt->funcname = list_make2(makeString("sift"), makeString(HEAR_FUNCTION));
	stmt->args = list_make1(makeString(psprintf("%u", RelationGetRelid(index))));
	stmt->row = row;
	stmt->timing = TRIGGER_TYPE_AFTER;
	stmt->events = update ? TRIGGER_TYPE_UPDATE : TRIGGER_TYPE_DELETE;
	stmt->transitionRels = row ? NIL : list_make1(tt);
	trig = sift_create_trigger(stmt, relid, funcoid);
	ObjectAddressSet(idx, RelationRelationId, RelationGetRelid(index));
	sift_depend(&idx, &trig, DEPENDENCY_NORMAL);
}

void
sift_col_hear_create(Relation heap, Relation index)
{
	foreach_oid(tbl, sift_col_hear_tables(RelationGetRelid(heap), sift_col_track_ctid(index)))
	{
		create_trigger(tbl, index, false);
		create_trigger(tbl, index, true);
	}
}

static void
drop_trigger(Relation index, const HearTrig *t)
{
	sift_undepend(RelationRelationId, RelationGetRelid(index), DEPENDENCY_NORMAL, TriggerRelationId, t->oid);
	CommandCounterIncrement();
	sift_col_track_delete_trigger(t->oid);
}

/* All the triggers of the index, on whatever tables: the caller holds the locks that dropping them takes */
void
sift_col_hear_drop_all(Relation index)
{
	foreach_ptr(HearTrig, t, sift_col_hear_find(index, false))
		drop_trigger(index, t);
	CommandCounterIncrement();
}

HearPlan
sift_col_hear_plan(Relation heap, Relation index, List *trigs)
{
	HearPlan	plan = {0};
	List	   *tables = sift_col_hear_tables(RelationGetRelid(heap), sift_col_track_ctid(index));

	foreach_oid(tbl, tables)
		for (int e = 0; e < 2; e++)
		{
			int			on = 0;

			foreach_ptr(HearTrig, t, trigs)
				if (t->relid == tbl && t->event == e)
				{
					on++;
					if (t->origin)
						plan.enable = lappend(plan.enable, t);
					else
						plan.bad |= !t->ok;
				}
			if (on == 0)
				plan.make = lappend(plan.make, memcpy(palloc(sizeof(HearWant)), &(HearWant) {tbl, e}, sizeof(HearWant)));
			plan.bad |= on > 1;
		}
	foreach_ptr(HearTrig, t, trigs)
		if (!list_member_oid(tables, t->relid))
			plan.drop = lappend(plan.drop, t);
	return plan;
}

bool
sift_col_hear_plan_empty(const HearPlan *plan)
{
	return plan->make == NIL && plan->enable == NIL && plan->drop == NIL;
}

/* The plan, under the locks it takes (a table above the heap is waited for briefly): false, and nothing done, when one stays busy */
bool
sift_col_hear_apply(Relation heap, Relation index, const HearPlan *plan)
{
	foreach_ptr(HearWant, w, plan->make)
		if (!sift_col_hear_lock_briefly(w->relid, ShareRowExclusiveLock))
			return false;
	foreach_ptr(HearTrig, t, plan->enable)
		if (!sift_col_hear_lock_briefly(t->relid, ShareRowExclusiveLock))
			return false;
	foreach_ptr(HearTrig, t, plan->drop)
		if (!sift_col_hear_lock_briefly(t->relid, AccessExclusiveLock))
			return false;
	foreach_ptr(HearWant, w, plan->make)
		create_trigger(w->relid, index, w->event == 1);
	foreach_ptr(HearTrig, t, plan->enable)
	{
		Relation	rel = t->relid == RelationGetRelid(heap) ? heap : table_open(t->relid, NoLock);

		EnableDisableTrigger(rel, NameStr(t->name), InvalidOid, TRIGGER_FIRES_ALWAYS, false, false, ShareRowExclusiveLock);
		if (rel != heap)
			table_close(rel, NoLock);
	}
	foreach_ptr(HearTrig, t, plan->drop)
		drop_trigger(index, t);
	CommandCounterIncrement();
	return true;
}

