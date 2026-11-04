/*
 * col/rowkey.c - the option track_changes of a heap column index: the column that identifies a row, the rule that makes the index able to say which older entry
 * an updated row replaces. The checks run at CREATE INDEX and at ALTER INDEX; 'auto' is the primary key and is written back as the column.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "executor/tuptable.h"
#include "utils/timestamp.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "core/compat.h"
#include "core/tomb.h"

bool
sift_col_row_key_on(Relation index)
{
	const char *rk = sift_col_options_row_key(index);

	return rk[0] != '\0' && strcmp(rk, "off") != 0 && strcmp(rk, "auto") != 0;
}

/* A valid, immediate unique index of the table on exactly this column, not partial, not on an expression: its oid, InvalidOid when none */
static Oid
unique_on(Relation heap, AttrNumber att, bool primary_only, AttrNumber *pk)
{
	List	   *oids = RelationGetIndexList(heap);
	Oid			found = InvalidOid;

	foreach_oid(ioid, oids)
	{
		HeapTuple	tp = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix;

		if (!HeapTupleIsValid(tp))
			continue;
		ix = (Form_pg_index) GETSTRUCT(tp);
		if (ix->indisvalid && ix->indisunique && ix->indimmediate && ix->indnkeyatts == 1 && ix->indnatts == 1 && ix->indkey.values[0] != 0 &&
			heap_attisnull(tp, Anum_pg_index_indpred, NULL) && (!primary_only || ix->indisprimary))
		{
			if (pk != NULL && ix->indisprimary)
				*pk = ix->indkey.values[0];
			if (!OidIsValid(found) && (att == InvalidAttrNumber || ix->indkey.values[0] == att))
				found = ioid;
		}
		ReleaseSysCache(tp);
	}
	list_free(oids);
	return found;
}

/* The unique index the key is found by: InvalidOid when the table has none now (a constraint dropped after the index was built) */
Oid
sift_col_row_key_unique(Relation heap, AttrNumber att)
{
	return unique_on(heap, att, false, NULL);
}

/* The primary key of the table when it is one column, else InvalidAttrNumber */
static AttrNumber
primary_key_att(Relation heap)
{
	AttrNumber	pk = InvalidAttrNumber;

	(void) unique_on(heap, InvalidAttrNumber, true, &pk);
	return pk;
}

/* The copy of pg_repack (repack.table_<oid>) is built without the constraints and the NOT NULL of its table, and takes them by the swap: its indexes cannot ask for them */
static bool
repack_copy(Relation heap)
{
	return strcmp(get_namespace_name(RelationGetNamespace(heap)), "repack") == 0 && strncmp(RelationGetRelationName(heap), "table_", 6) == 0;
}

/* The oid of the table or index that a table (repack.table_<oid>) or an index (repack.index_<oid>) of pg_repack replaces by the swap, InvalidOid for any other relation */
Oid
sift_col_repack_origin(Relation rel)
{
	const char *name = RelationGetRelationName(rel);
	const char *prefix = rel->rd_rel->relkind == RELKIND_INDEX ? "index_" : "table_";
	char	   *end;
	unsigned long oid;

	if (strcmp(get_namespace_name(RelationGetNamespace(rel)), "repack") != 0 || strncmp(name, prefix, strlen(prefix)) != 0)
		return InvalidOid;
	oid = strtoul(name + strlen(prefix), &end, 10);
	return *end == '\0' && oid != 0 ? (Oid) oid : InvalidOid;
}

/*
 * build: the key may come later. A dump restores the index before the primary key when pg_restore runs in parallel and when pg_upgrade does, and the dumps of 4.0.0 name the
 * column ('auto' is stored as the key's name), so CREATE INDEX and REINDEX accept a column without a unique index, with a NOTICE. That cannot give a wrong row: a read takes
 * the log only with a valid unique index of the column (sift_col_track_ok), and a statement that finds none ends the completeness (sift_col_tomb_dead_keys). ALTER INDEX is
 * the user's word and keeps the error.
 */
static void
check_column(Relation heap, Relation index, const char *name, bool build)
{
	AttrNumber	att = get_attnum(RelationGetRelid(heap), name);
	bool		covered = false;

	if (att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("track_changes: table \"%s\" has no column \"%s\"", RelationGetRelationName(heap), name)));
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
		covered |= index->rd_index->indkey.values[i] == att;
	if (!covered)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("track_changes: column \"%s\" is not a plain column of the index", name),
						errhint("Name a column the index holds, or add it to the column list of the index.")));
	if (repack_copy(heap))
		return;
	if (!TupleDescAttr(RelationGetDescr(heap), att - 1)->attnotnull)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("track_changes: column \"%s\" can be NULL", name),
						errhint("A row is found by its key: declare the column NOT NULL, or use its primary key.")));
	if (build && !OidIsValid(unique_on(heap, att, false, NULL)))
	{
		ereport(NOTICE, (errmsg("sift_col index \"%s\": track_changes column \"%s\" has no unique index of its own yet", RelationGetRelationName(index), name),
						 errdetail("Reads check the heap until the table has a UNIQUE or PRIMARY KEY constraint on the column alone, and a row written before that is not in the log, so the log stays incomplete after it."),
						 errhint("REINDEX INDEX %s gives it a baseline once the constraint exists.", RelationGetRelationName(index))));
		return;
	}
	if (!OidIsValid(unique_on(heap, att, false, NULL)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("track_changes: column \"%s\" has no unique index of its own", name),
						errdetail("The index replaces the older entries of a key when the row is updated, which is exact only when no two live rows share the key."),
						errhint("Create a UNIQUE or PRIMARY KEY constraint on the column alone: not deferrable, not partial, not on an expression, not on several columns.")));
}

/* ALTER INDEX and CREATE INDEX: the option names a column the index can use */
void
sift_col_row_key_check(Relation index)
{
	Relation	heap;

	if (!sift_col_row_key_on(index))
		return;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	check_column(heap, index, sift_col_options_row_key(index), false);
	table_close(heap, NoLock);
}

/* The partitions of one CREATE INDEX on a partitioned table have the same reason: said for the first only (by the start of the statement) */
static bool
quiet_partition(Relation index, const char *why)
{
	static TimestampTz stmt;
	static const char *last;
	bool		same = index->rd_rel->relispartition && stmt == GetCurrentStatementStartTimestamp() && last == why;

	stmt = GetCurrentStatementStartTimestamp();
	last = why;
	return same;
}

/*
 * Before a build: 'auto' (the default) becomes the primary key, or off with a NOTICE that says why when there is none of one column, the index does not hold it, or a statement trigger
 * cannot hear the table (a column the user named has no such way out: a table the triggers cannot hear is an error, in sift_col_track_check). The column is checked, and the point directory of heap addresses that finds its entries lists it (the option
 * point_tid_columns gets the column when it does not name it, and keeps its automatic choice).
 */
void
sift_col_row_key_prepare(Relation heap, Relation index)
{
	const char *rk = sift_col_options_row_key(index);
	const char *tid;
	StringInfoData v;

	const char *why = NULL;
	bool		autokey = strcmp(rk, "auto") == 0;
	AttrNumber	pk = InvalidAttrNumber;

	if (rk[0] == '\0' || strcmp(rk, "off") == 0)
		return;
	if (autokey)
	{
		pk = primary_key_att(heap);
		if (pk == InvalidAttrNumber)
			why = "the table has no primary key of one column";
		else
		{
			bool		held = false;

			for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
				held |= index->rd_index->indkey.values[i] == pk;
			if (!held)
				why = "the index does not hold the primary key column";
			else if (strcmp(sift_col_options_point_tid_columns(index), "off") == 0)
				why = "point_tid_columns is 'off', and the key is found through its directory";
			else
				why = sift_col_track_deaf(heap);
		}
	}
	/* the log is a directory kind that a library before 4.0 refuses: a rewrite at a lowered minor (the rollback of docs/UPGRADING.md) drops what an earlier build chose */
	if (why == NULL && sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		why = "sift.col_format_minor is below 15, so that an older library can read the index";
	if (why != NULL)
	{
		if (!quiet_partition(index, why))
			ereport(NOTICE, (errmsg("sift_col index \"%s\" does not track changes: %s", RelationGetRelationName(index), why),
							 errhint("Name a column that has a unique index of its own and that the index holds: WITH (track_changes = 'id'); WITH (track_changes = 'off') silences this.")));
		if (!autokey)
			sift_col_set_option(index, "track_changes", "off");
		return;
	}
	if (autokey)
	{
		sift_col_set_option(index, "track_changes", get_attname(RelationGetRelid(heap), pk, false));
		rk = sift_col_options_row_key(index);
	}
	check_column(heap, index, rk, true);
	tid = sift_col_options_point_tid_columns(index);
	if (sift_col_list_has(tid, rk))
		return;
	initStringInfo(&v);
	/* the key joins what the default formula chooses ('auto: b, id', which a rewrite chooses again and keeps the key in), and the user's own list gains it; 'off' gives it up for the key alone */
	if (tid[0] == '\0' || strcmp(tid, "auto") == 0)
		appendStringInfo(&v, "auto: %s", rk);
	else if (strcmp(tid, "off") == 0)
		appendStringInfoString(&v, rk);
	else
		appendStringInfo(&v, "%s, %s", tid, rk);
	sift_col_set_option(index, "point_tid_columns", v.data);
}

/* Did this transaction tombstone the address already (the supersede of an UPDATE did, at its aminsert)? */
static bool
own_entry(const SiftTombLog *log, uint64 key)
{
	int			n;
	const SiftTomb *e = sift_tomb_find(log, key, &n);

	for (int i = 0; i < n; i++)
		if (TransactionIdIsCurrentTransactionId(e[i].xid))
			return true;
	return false;
}

#define DEAD_KEYS_CHUNK	8192

/*
 * A tombstone for each address the table's unique index lists for the key of each old row that no version is live for (a dirty snapshot: a version of an open transaction,
 * this one included, is live): the entries a delete or a key change left. An address that holds no entry of this index is harmless, VACUUM drops its tombstone when it
 * frees the address. The index is opened once for all the rows, which are looked up a chunk at a time with one array key (the b-tree sorts it and walks the leaves
 * once: the keys of a page share its descent), and the tombstones of a chunk go to the log by the page.
 */
void
sift_col_tomb_dead_keys(Relation index, Relation heap, Tuplestorestate *old_rows)
{
	AttrNumber	hatt = index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1];
	Oid			uoid = sift_col_row_key_unique(heap, hatt);
	SiftTombLog *log;
	Relation	uidx;
	IndexScanDesc scan;
	IndexFetchTableData *fetch;
	ScanKeyData sk;
	SnapshotData dirty;
	Oid			intype;
	Oid			eq;
	TupleTableSlot *old,
			   *seen;
	MemoryContext chunk_cxt;
	SiftTomb   *ents;
	int			cap = 1024;
	bool		more = true;

	if (!OidIsValid(uoid))
	{
		/* no unique index to find the rows by (not made yet, dropped, being built): what this statement removed is not heard, so the log is no longer complete */
		if ((log = sift_col_log(index)) != NULL && log->chan.flags != 0)
			sift_tomb_set_chan(index, log, &(SiftTombChan) {0});
		return;
	}
	if ((log = sift_col_log(index)) == NULL)
	{
		sift_col_log_create(index);
		log = sift_col_log(index);
	}
	uidx = index_open(uoid, AccessShareLock);
	intype = uidx->rd_opcintype[0];
	eq = get_opcode(get_opfamily_member(uidx->rd_opfamily[0], intype, intype, BTEqualStrategyNumber));
	InitDirtySnapshot(dirty);
	scan = sift_index_beginscan(heap, uidx, SnapshotAny, 1);
	fetch = sift_table_index_fetch_begin(heap);
	old = MakeSingleTupleTableSlot(RelationGetDescr(heap), &TTSOpsMinimalTuple);
	seen = table_slot_create(heap, NULL);
	chunk_cxt = AllocSetContextCreate(CurrentMemoryContext, "sift dead keys", ALLOCSET_DEFAULT_SIZES);
	ents = palloc(cap * sizeof(SiftTomb));
	tuplestore_rescan(old_rows);
	while (more)
	{
		ArrayBuildState *keys = NULL;
		int			nents = 0;
		int			nkeys = 0;

		while (nkeys < DEAD_KEYS_CHUNK)
		{
			bool		isnull;
			Datum		key;

			if (!(more = tuplestore_gettupleslot(old_rows, true, false, old)))
				break;
			key = slot_getattr(old, hatt, &isnull);
			if (isnull)
				continue;
			keys = accumArrayResult(keys, key, false, intype, chunk_cxt);
			nkeys++;
		}
		if (nkeys > 0)
		{
			Datum		arr = makeArrayResult(keys, chunk_cxt);

			ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, intype, uidx->rd_indcollation[0], eq, arr);
			index_rescan(scan, &sk, 1, NULL, 0);
			for (ItemPointer t; (t = index_getnext_tid(scan, ForwardScanDirection)) != NULL;)
			{
				ItemPointerData tid = *t;
				bool		again = false,
							all_dead;

				if (!table_index_fetch_tuple(fetch, &tid, &dirty, seen, &again, &all_dead) && !own_entry(log, sift_key_from_tid(t)))
				{
					if (nents == cap)
						ents = repalloc(ents, (cap *= 2) * sizeof(SiftTomb));
					ents[nents++] = (SiftTomb) {.key = sift_key_from_tid(t), .xid = GetCurrentTransactionId(), .cid = GetCurrentCommandId(true), .kind = SIFT_TOMB_DELETE};
				}
			}
		}
		MemoryContextReset(chunk_cxt);
		if (nents > 0)
			sift_tomb_append_many(index, log, ents, nents);
	}
	pfree(ents);
	MemoryContextDelete(chunk_cxt);
	ExecDropSingleTupleTableSlot(seen);
	ExecDropSingleTupleTableSlot(old);
	table_index_fetch_end(fetch);
	index_endscan(scan);
	index_close(uidx, NoLock);
}
