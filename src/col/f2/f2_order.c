/*
 * col/f2/f2_order.c - key_order = 'auto' of an index of format major 2: CREATE INDEX and REINDEX read a sample of the rows and write the order they choose to the option as
 * 'auto: entity[, time]', so that every later rebuild keeps it. The entity is a column the rows belong to (a foreign key or a column named *_id first) that is not unique and has
 * at least F2_AUTO_ROWS_PER_ENTITY rows per value; the time is the first date or timestamp column with more than one value. A table with fewer than F2_AUTO_MIN_SAMPLE rows leaves the
 * option as 'auto' and the index unkeyed (the function says it was deferred), unless the catalog alone names a foreign key and a time column.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "col/am.h"

#define F2_AUTO_SAMPLE				4096
#define F2_AUTO_MIN_SAMPLE			64
#define F2_AUTO_ROWS_PER_ENTITY		4.0
#define F2_AUTO_SLOTS				(2 * F2_AUTO_SAMPLE)

typedef struct AutoCand
{
	AttrNumber	hatt;
	bool		time;
	bool		entity;
	bool		fk;
	bool		unique;
	uint32		nn;
	uint32		ndv;
	int64	   *seen;
	bool	   *used;
} AutoCand;

static bool
is_time_type(Oid type)
{
	return type == DATEOID || type == TIMESTAMPOID || type == TIMESTAMPTZOID;
}

static bool
key_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID || is_time_type(type);
}

static int64
key_value(Oid type, Datum d)
{
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		case DATEOID:
			return DatumGetDateADT(d);
		default:
			return DatumGetInt64(d);
	}
}

static bool
ends_in_id(const char *name)
{
	size_t		n = strlen(name);

	return n > 3 && strcmp(name + n - 3, "_id") == 0;
}

static AutoCand *
candidates(Relation heap, Relation index, int *n)
{
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);
	AutoCand   *c = palloc0(natts * sizeof(AutoCand));

	*n = 0;
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		AttrNumber	hatt = index->rd_index->indkey.values[i];
		AutoCand   *a = &c[*n];

		if (hatt == 0 || !key_type(att->atttypid) || strchr(NameStr(att->attname), ',') != NULL)
			continue;
		a->hatt = hatt;
		a->time = is_time_type(att->atttypid);
		a->fk = sift_attr_is_fk(heap, hatt);
		a->entity = a->fk || ends_in_id(NameStr(att->attname));
		a->unique = sift_attr_is_unique(heap, hatt);
		(*n)++;
	}
	return c;
}

static void
note(AutoCand *a, int64 v)
{
	uint32		h = (uint32) (((uint64) v * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) % F2_AUTO_SLOTS;

	a->nn++;
	if (a->seen == NULL)
	{
		a->seen = palloc(F2_AUTO_SLOTS * sizeof(int64));
		a->used = palloc0(F2_AUTO_SLOTS);
	}
	while (a->used[h] && a->seen[h] != v)
		h = (h + 1) % F2_AUTO_SLOTS;
	if (!a->used[h])
	{
		a->used[h] = true;
		a->seen[h] = v;
		a->ndv++;
	}
}

static uint32
sample(Relation heap, Relation index, AutoCand *c, int n)
{
	Snapshot	snap = RegisterSnapshot(GetTransactionSnapshot());
	TableScanDesc scan = table_beginscan_strat(heap, snap, 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	uint32		rows = 0;

	while (rows < F2_AUTO_SAMPLE && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		rows++;
		for (int i = 0; i < n; i++)
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, c[i].hatt, &isnull);

			if (!isnull)
				note(&c[i], key_value(TupleDescAttr(RelationGetDescr(heap), c[i].hatt - 1)->atttypid, d));
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snap);
	return rows;
}

static int
pick_entity(const AutoCand *c, int n, uint32 rows, bool catalog_only)
{
	int			best = -1;
	double		score,
				top = -1;

	for (int i = 0; i < n; i++)
	{
		if (c[i].time || c[i].unique)
			continue;
		if (catalog_only)
			score = c[i].fk ? 1 : -1;
		else if (c[i].nn >= rows / 2 && c[i].ndv >= 2 && (double) c[i].nn / c[i].ndv >= F2_AUTO_ROWS_PER_ENTITY)
			score = (c[i].entity ? 1000000.0 : 0.0) + c[i].ndv;
		else
			score = -1;
		if (score > top)
		{
			top = score;
			best = i;
		}
	}
	return top >= 0 ? best : -1;
}

static int
pick_time(const AutoCand *c, int n, uint32 rows, bool catalog_only)
{
	for (int i = 0; i < n; i++)
		if (c[i].time && (catalog_only || (c[i].nn >= rows / 2 && c[i].ndv >= 2)))
			return i;
	return -1;
}

bool
sift_f2_auto_key_order(Relation heap, Relation index)
{
	int			n;
	AutoCand   *c;
	uint32		rows;
	bool		catalog_only;
	int			entity,
				time;
	StringInfoData v;

	if (!sift_col_key_order_auto_pending(index))
		return false;
	c = candidates(heap, index, &n);
	rows = sample(heap, index, c, n);
	catalog_only = rows < F2_AUTO_MIN_SAMPLE;
	entity = pick_entity(c, n, rows, catalog_only);
	time = pick_time(c, n, rows, catalog_only);
	if (catalog_only && (entity < 0 || time < 0))
		return true;
	if (entity < 0 && time < 0)
		return false;
	initStringInfo(&v);
	if (entity >= 0)
		appendStringInfoString(&v, get_attname(RelationGetRelid(heap), c[entity].hatt, false));
	if (time >= 0)
		appendStringInfo(&v, "%s%s", v.len ? ", " : "", get_attname(RelationGetRelid(heap), c[time].hatt, false));
	sift_col_set_option(index, "key_order", psprintf("auto: %s", v.data));
	return false;
}
