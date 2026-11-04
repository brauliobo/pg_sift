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

/*
 * The copy of pg_repack (repack.table_<oid>) and that of pg_squeeze (tmp_<oid>, in the schema of its table) are built without the constraints and the NOT NULL of their table, and take
 * them by the swap: their indexes cannot ask for them. The log of such an index is never complete for the table after the swap: pg_squeeze applies what the writers did meanwhile
 * without statements, so no trigger hears it, and the witnesses of the log are those of triggers that go with the copy (REINDEX gives the table its baseline back).
 */
static bool
repack_copy(Relation heap)
{
	const char *name = RelationGetRelationName(heap);
	char	   *end;
	unsigned long oid;

	if (strcmp(get_namespace_name(RelationGetNamespace(heap)), "repack") == 0 && strncmp(name, "table_", 6) == 0)
		return true;
	if (strncmp(name, "tmp_", 4) != 0)
		return false;
	oid = strtoul(name + 4, &end, 10);
	return *end == '\0' && oid != 0 && oid != RelationGetRelid(heap) && get_rel_relkind(oid) == RELKIND_RELATION && get_rel_namespace(oid) == RelationGetNamespace(heap);
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

/*
 * Why an index whose track_changes is 'auto' tracks nothing, NULL when it can (a short phrase for the summary of the build): the table has no primary key of one column, the index
 * does not hold it, point_tid_columns is off, a statement trigger cannot hear the table (or the extension is not updated to the release that has its function: the restore of
 * pg_upgrade makes the indexes before ALTER EXTENSION UPDATE), or sift.col_format_minor lets an older library read the index (the log is a directory kind that a library before 4.0
 * refuses: a rewrite at a lowered minor, the rollback of docs/UPGRADING.md, drops what an earlier build chose). *pk is the key column when there is one.
 */
const char *
sift_col_row_key_why(Relation heap, Relation index, AttrNumber *pk)
{
	const char *why = NULL;
	bool		held = false;

	*pk = primary_key_att(heap);
	if (*pk == InvalidAttrNumber)
		return "the table has no one-column primary key";
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
		held |= index->rd_index->indkey.values[i] == *pk;
	if (!held)
		return "the index lacks the primary key column";
	if (strcmp(sift_col_options_point_tid_columns(index), "off") == 0)
		return "point_tid_columns is off";
	why = sift_col_track_deaf(heap);
	if (why != NULL)
		return strstr(why, "extension") ? "the extension is not updated yet" : strstr(why, "partition") ? "the table is a partition or a parent" : "the table is a subscription target";
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return "sift.col_format_minor is below 15";
	return NULL;
}

/*
 * Before a build: 'auto' (the default) becomes the primary key, or stays 'auto' when there is none of one column, the index does not hold it, or a statement trigger
 * cannot hear the table (the summary of the build says why; a column the user named has no such way out: a table the triggers cannot hear is an error, in
 * sift_col_track_check). The column is checked, and the point directory of heap addresses that finds its entries lists it (the option
 * point_tid_columns gets the column when it does not name it, and keeps its automatic choice).
 */
void
sift_col_row_key_prepare(Relation heap, Relation index)
{
	const char *rk = sift_col_options_row_key(index);
	const char *tid;
	StringInfoData v;
	bool		autokey = strcmp(rk, "auto") == 0;
	AttrNumber	pk = InvalidAttrNumber;

	if (rk[0] == '\0' || strcmp(rk, "off") == 0)
		return;
	if (autokey)
	{
		if (sift_col_row_key_why(heap, index, &pk) != NULL)
			return;
		sift_col_set_option(index, "track_changes", get_attname(RelationGetRelid(heap), pk, false));
		rk = sift_col_options_row_key(index);
	}
	else if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
	{
		ereport(NOTICE, (errmsg("sift_col index \"%s\" does not track changes: sift.col_format_minor is below 15", RelationGetRelationName(index)),
						 errhint("Raise sift.col_format_minor to 15 or more to track changes, or write WITH (track_changes = 'off').")));
		sift_col_set_option(index, "track_changes", "off");
		return;
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
#define DEAD_SHARE_MAX	(4 << 20)

/*
 * The dead addresses one statement found, for the next index of the table whose trigger asks for the same keys of the same old rows: they are a fact of the table, so the
 * unique index is probed once for all the indexes that track it. Kept in the backend until the transaction ends (at most DEAD_SHARE_MAX addresses, 32 MB; a statement
 * with more is probed again by each index), and only when another index of the table is going to ask.
 */
static struct
{
	Oid			heap;
	Oid			uoid;
	Tuplestorestate *old;
	CommandId	cid;
	SubTransactionId sub;
	MemoryContext cxt;
	uint64	   *keys;
	int			n;
	int			cap;
	bool		whole;			/* the probe finished and kept every address */
} dead_share;

static void
dead_share_reset(XactEvent event, void *arg)
{
	if (dead_share.cxt != NULL)
		MemoryContextDelete(dead_share.cxt);
	memset(&dead_share, 0, sizeof(dead_share));
}

static bool
dead_share_is(Relation heap, Oid uoid, Tuplestorestate *old)
{
	return dead_share.whole && dead_share.heap == RelationGetRelid(heap) && dead_share.uoid == uoid && dead_share.old == old &&
		dead_share.cid == GetCurrentCommandId(false) && dead_share.sub == GetCurrentSubTransactionId();
}

/* Does another index of the table track the same key (its trigger will ask for the same addresses)? */
static bool
other_tracker(Relation index, Relation heap, AttrNumber hatt)
{
	foreach_oid(oid, RelationGetIndexList(heap))
	{
		Relation	other;
		bool		same;

		if (oid == RelationGetRelid(index))
			continue;
		other = index_open(oid, AccessShareLock);
		same = sift_col_is_index(other) && sift_col_row_key_on(other) && other->rd_index->indkey.values[sift_col_rel(other)->rowkey_attno - 1] == hatt;
		index_close(other, AccessShareLock);
		if (same)
			return true;
	}
	return false;
}

/* The tombstone of an address the log does not hold from this transaction yet, in the chunk */
static void
add_dead(SiftTomb **ents, int *nents, int *cap, const SiftTombLog *log, uint64 key)
{
	if (own_entry(log, key))
		return;
	if (*nents == *cap)
		*ents = repalloc(*ents, (*cap *= 2) * sizeof(SiftTomb));
	(*ents)[(*nents)++] = (SiftTomb) {.key = key, .xid = GetCurrentTransactionId(), .cid = GetCurrentCommandId(true), .kind = SIFT_TOMB_DELETE};
}

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
	bool		keep;

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
	ents = palloc(cap * sizeof(SiftTomb));
	if (dead_share_is(heap, uoid, old_rows))
	{
		for (int done = 0; done < dead_share.n;)
		{
			int			nents = 0;

			for (int end = Min(done + DEAD_KEYS_CHUNK, dead_share.n); done < end; done++)
				add_dead(&ents, &nents, &cap, log, dead_share.keys[done]);
			if (nents > 0)
				sift_tomb_append_many(index, log, ents, nents);
		}
		pfree(ents);
		return;
	}
	keep = other_tracker(index, heap, hatt);
	if (keep)
	{
		static bool registered;

		if (!registered)
		{
			RegisterXactCallback(dead_share_reset, NULL);
			registered = true;
		}
		dead_share_reset(XACT_EVENT_COMMIT, NULL);
		dead_share.cxt = AllocSetContextCreate(TopMemoryContext, "sift dead share", ALLOCSET_DEFAULT_SIZES);
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

				if (table_index_fetch_tuple(fetch, &tid, &dirty, seen, &again, &all_dead))
					continue;
				add_dead(&ents, &nents, &cap, log, sift_key_from_tid(t));
				if (keep)
				{
					if (dead_share.n == DEAD_SHARE_MAX)
						keep = false;
					else
					{
						if (dead_share.n == dead_share.cap)
						{
							MemoryContext oldcxt = MemoryContextSwitchTo(dead_share.cxt);

							dead_share.cap = dead_share.cap ? dead_share.cap * 2 : 1024;
							dead_share.keys = dead_share.keys ? repalloc(dead_share.keys, dead_share.cap * sizeof(uint64)) : palloc(dead_share.cap * sizeof(uint64));
							MemoryContextSwitchTo(oldcxt);
						}
						dead_share.keys[dead_share.n++] = sift_key_from_tid(t);
					}
				}
			}
		}
		MemoryContextReset(chunk_cxt);
		if (nents > 0)
			sift_tomb_append_many(index, log, ents, nents);
	}
	if (keep)
	{
		dead_share.heap = RelationGetRelid(heap);
		dead_share.uoid = uoid;
		dead_share.old = old_rows;
		dead_share.cid = GetCurrentCommandId(false);
		dead_share.sub = GetCurrentSubTransactionId();
		dead_share.whole = true;
	}
	else
		dead_share_reset(XACT_EVENT_COMMIT, NULL);
	pfree(ents);
	MemoryContextDelete(chunk_cxt);
	ExecDropSingleTupleTableSlot(seen);
	ExecDropSingleTupleTableSlot(old);
	table_index_fetch_end(fetch);
	index_endscan(scan);
	index_close(uidx, NoLock);
}
