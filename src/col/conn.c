/*
 * col/conn.c - the change protocol of connected indexes (col/conn.h, docs/COL-CONNECT.md): the key journal a parent index keeps once a child index is connected to it, the
 * stamp a derivation takes, and the set of keys that changed since a stamp.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_class.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_type_d.h"
#include "storage/lmgr.h"
#include "port/pg_bitutils.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/conn.h"
#include "core/cmp.h"
#include "core/rowmem.h"
#include "core/tomb.h"

/* A stamp older than this many transaction ids is not compared with a current one: the comparison of two ids wraps at 2^31 and the journal drops records past this */
#define CONN_AGE_MAX	(1U << 30)

/*
 * What this backend knows of the journals it writes: the lineage of an index (0: none), kept in the state the relcache entry of the index holds for the access method, which the relcache
 * frees when it invalidates the entry (the transaction that connects a child invalidates it, and a writer takes its lock on the table after that transaction ended: the table lock it holds
 * out until then). It follows the entry by itself: a relcache callback would take one of the ten slots a backend has for all its libraries (scripts/check-relcache-slots.sh), and a
 * syscache one would not hear that invalidation, which changes no row of pg_class.
 */
/* The lineage of the journal of the index, read from the header page once for each use of the index until it is invalidated */
static uint64
journal_lineage(Relation index)
{
	SiftTombKeys k;
	uint64		n;
	SiftDirSnapshot *dir;

	if (sift_col_rel(index)->journal_known)
		return sift_col_rel(index)->journal_lineage;
	dir = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, dir);
	if (!sift_tomb_peek_keys(index, dir, &k, &n))
		memset(&k, 0, sizeof(k));
	pfree(dir);
	sift_col_rel(index)->journal_known = true;
	sift_col_rel(index)->journal_lineage = k.lineage;
	return k.lineage;
}

/* The int8 value of an int2, int4 or int8 key; false for any other type */
static bool
key64(Oid typid, Datum d, int64 *out)
{
	switch (typid)
	{
		case INT2OID:
			*out = DatumGetInt16(d);
			return true;
		case INT4OID:
			*out = DatumGetInt32(d);
			return true;
		case INT8OID:
			*out = DatumGetInt64(d);
			return true;
		default:
			return false;
	}
}

static bool
journal_record(Relation index, Oid keytype, Datum key, SiftTombKind kind, uint16 flags, SiftTomb *rec)
{
	int64		k;

	if (!sift_col_track_on(index) || journal_lineage(index) == 0 || !key64(keytype, key, &k))
		return false;
	*rec = (SiftTomb) {.key = SIFT_TOMB_KEYS_KEY,.xid = GetCurrentTransactionId(),.cid = sift_col_stmt_cid(),.succ = (uint64) k,.kind = kind,.flags = flags};
	return true;
}

bool
sift_conn_record(Relation index, Oid keytype, Datum key, bool kept, SiftTomb *rec)
{
	return journal_record(index, keytype, key, SIFT_TOMB_KEYCHG, kept ? SIFT_TOMB_KEPT : 0, rec);
}

/*
 * The journal of a scope relation (col/scope.h) is kept by a sift_col index with track_changes = 'ctid' whose first column is the member key: the row trigger of the index
 * calls ended for the old version of each row an UPDATE or DELETE ended (KEYCHG), its aminsert calls added for each version inserted (KEYADD, an UPDATE writes one too). Both are held
 * with the tombstones of the transaction. An index that keeps no journal ignores them.
 */
static void
journal_row(Relation index, Relation heap, SiftTombKind kind, Datum key, bool isnull)
{
	SiftTomb	rec;
	AttrNumber	att = index->rd_index->indkey.values[0];

	if (!isnull && journal_record(index, TupleDescAttr(RelationGetDescr(heap), att - 1)->atttypid, key, kind, 0, &rec))
		sift_col_tomb_hold_rec(index, &rec);
}

void
sift_conn_ended_row(Relation index, Relation heap, HeapTuple old)
{
	bool		isnull;
	Datum		key = heap_getattr(old, index->rd_index->indkey.values[0], RelationGetDescr(heap), &isnull);

	journal_row(index, heap, SIFT_TOMB_KEYCHG, key, isnull);
}

void
sift_conn_added_row(Relation index, Relation heap, const Datum *values, const bool *isnull)
{
	journal_row(index, heap, SIFT_TOMB_KEYADD, values[0], isnull[0]);
}

bool
sift_conn_journal_on(Relation index)
{
	return sift_col_track_on(index) && journal_lineage(index) != 0;
}

static bool
keep_all(SiftTomb *e, void *arg)
{
	return true;
}

/* The journal of the index, made if it has none; the caller has the writers out or the index hidden from them. Returns the lineage. */
static uint64
journal_make(Relation heap, Relation index)
{
	SiftTombLog *log = sift_col_log(index);
	SiftTombKeys k = {0};

	if (log == NULL)
	{
		sift_col_log_create(index);
		log = sift_col_log(index);
	}
	if (log->keys_cover && log->keys.lineage != 0)
		return log->keys.lineage;
	if (!log->keys_cover)
	{
		(void) sift_tomb_rewrite(index, log, keep_all, NULL);
		log = sift_col_log(index);
	}
	do
	{
		bool		ok = pg_strong_random(&k.lineage, sizeof(k.lineage));

		SIFT_INVARIANT(ok, "sift: could not generate a journal id");
	} while (k.lineage == 0);
	/* the records start here: a stamp of a snapshot older than the journal cannot be vouched (a change between the snapshot and now is in no record); the writers are out, so no transaction below the next id writes the table */
	k.trim = ReadNextTransactionId();
	sift_tomb_set_keys(index, log, &k);
	/* a scope relation's journal holds KEYADD records: an older library refuses it */
	sift_tomb_set_format(index, sift_col_track_ctid(index) ? SIFT_COL_FORMAT_SCOPE : SIFT_COL_FORMAT_KEYJOURNAL);
	CacheInvalidateRelcache(index);
	return k.lineage;
}

static void
check_key(Relation heap, Relation index)
{
	AttrNumber	hatt;
	Oid			typid;

	if (!sift_col_track_on(index) || (sift_col_track_ctid(index) && index->rd_index->indkey.values[0] == 0))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" cannot be connected: it does not track a key column", RelationGetRelationName(index)),
						errhint("Connect to an index with track_changes naming the unique key of its table.")));
	hatt = sift_col_track_ctid(index) ? index->rd_index->indkey.values[0] : index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1];
	typid = TupleDescAttr(RelationGetDescr(heap), hatt - 1)->atttypid;
	if (typid != INT2OID && typid != INT4OID && typid != INT8OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" cannot be connected: its key column is not an integer", RelationGetRelationName(index)),
						errhint("The key of the parent index is an int2, int4 or int8 column.")));
}

uint64
sift_conn_enable(Relation heap, Relation index)
{
	SiftConnGen gen;

	check_key(heap, index);
	/* a child that was connected before (its dependency is committed) made the journal under the lock, and every writer records since: nothing to hold out */
	if (sift_conn_children(RelationGetRelid(index)) != NIL && sift_conn_gen(index, &gen))
		return gen.lineage;
	/*
	 * Writers of the table are held out until the transaction ends: every transaction that wrote it before is finished, so the journal is complete from here. The log need not be
	 * complete yet (a parallel restore connects before the primary key of the table exists): nothing is vouched while it is not, and the trim of the journal moves when it becomes so.
	 */
	LockRelationOid(RelationGetRelid(heap), ShareLock);
	return journal_make(heap, index);
}

List *
sift_conn_children(Oid parent_index)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *children = NIL;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(parent_index));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL && d->objsubid == 0 && d->refobjsubid == 0 && get_rel_relkind(d->objid) == RELKIND_INDEX)
			children = lappend_oid(children, d->objid);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return children;
}

void
sift_conn_depend(Relation child_index, Relation parent_index)
{
	ObjectAddress child,
				parent;

	if (list_member_oid(sift_conn_children(RelationGetRelid(parent_index)), RelationGetRelid(child_index)))
		return;
	ObjectAddressSet(child, RelationRelationId, RelationGetRelid(child_index));
	ObjectAddressSet(parent, RelationRelationId, RelationGetRelid(parent_index));
	recordDependencyOn(&child, &parent, DEPENDENCY_NORMAL);
}

void
sift_conn_connect(Relation child_index, Relation parent_heap, Relation parent_index)
{
	(void) sift_conn_enable(parent_heap, parent_index);
	sift_conn_depend(child_index, parent_index);
}

void
sift_conn_build(Relation heap, Relation index)
{
	Oid			origin = RelationGetRelid(index);
	bool		wanted = sift_conn_children(origin) != NIL;

	if (!wanted && (origin = sift_col_track_swap_origin(heap, index)) != InvalidOid)
		wanted = sift_conn_children(origin) != NIL;
	if (wanted && sift_col_track_on(index))
	{
		check_key(heap, index);
		(void) journal_make(heap, index);
	}
}

static bool
aged(TransactionId xmin)
{
	return !TransactionIdIsNormal(xmin) || TransactionIdPrecedes(xmin, ReadNextTransactionId() - CONN_AGE_MAX);
}

static bool
trimmed(const SiftTombKeys *k, TransactionId xmin)
{
	return TransactionIdIsNormal(k->trim) && TransactionIdPrecedes(xmin, k->trim);
}

/* The journal of the index, or why there is none for the snapshot (the log is synced and the records this backend holds back are written) */
static SiftConnWhy
journal_for(Relation heap, Relation index, Snapshot snap, SiftTombLog **log)
{
	if (snap->snapshot_type != SNAPSHOT_MVCC || IsolationIsSerializable() || !TransactionIdIsNormal(snap->xmin))
		return SIFT_CONN_SNAPSHOT;
	if (!sift_col_track_on(index) || (*log = sift_col_log(index)) == NULL || !(*log)->keys_cover || (*log)->keys.lineage == 0)
		return SIFT_CONN_NO_JOURNAL;
	if (!sift_col_track_ok(index, heap, snap))
		return SIFT_CONN_NOT_COMPLETE;
	return SIFT_CONN_OK;
}

SiftConnWhy
sift_conn_take(Relation heap, Relation index, Snapshot resolve, SiftConnStamp *stamp, SiftConnGen *gen)
{
	SiftTombLog *log;
	SiftConnWhy why = journal_for(heap, index, resolve, &log);

	if (why != SIFT_CONN_OK)
		return why;
	if (trimmed(&log->keys, resolve->xmin))
		return SIFT_CONN_TRIMMED;
	*stamp = (SiftConnStamp) {.lineage = log->keys.lineage,.xmin = resolve->xmin};
	if (gen != NULL)
		(void) sift_conn_gen(index, gen);
	return SIFT_CONN_OK;
}

bool
sift_conn_gen(Relation index, SiftConnGen *gen)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftTombKeys k;
	uint64		n;
	bool		cover;

	sift_col_tombs_flush();
	sift_dir_read(index, dir);
	cover = sift_tomb_peek_keys(index, dir, &k, &n);
	pfree(dir);
	*gen = (SiftConnGen) {.lineage = k.lineage,.records = n,.trim = k.trim,.flags = 0};
	return cover && k.lineage != 0;
}

bool
sift_conn_gen_equal(const SiftConnGen *a, const SiftConnGen *b)
{
	return a->lineage == b->lineage && a->records == b->records && a->trim == b->trim && a->flags == b->flags;
}

SiftConnWhy
sift_conn_vouches(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap)
{
	SiftTombLog *log;
	SiftConnWhy why = journal_for(heap, index, snap, &log);

	if (why != SIFT_CONN_OK)
		return why;
	if (log->keys.lineage != stamp->lineage)
		return SIFT_CONN_LINEAGE;
	if (aged(stamp->xmin))
		return SIFT_CONN_AGED;
	if (trimmed(&log->keys, stamp->xmin))
		return SIFT_CONN_TRIMMED;
	if (TransactionIdPrecedes(snap->xmin, stamp->xmin))
		return SIFT_CONN_OLDER;
	return SIFT_CONN_OK;
}

typedef struct KeyRec
{
	int64		key;
	bool		kept;
	bool		used;
} KeyRec;

/* The slot of the key in an open-addressing table of KeyRec (at most half full) */
static KeyRec *
key_slot(KeyRec *tab, uint32 mask, int64 key)
{
	uint32		h = (uint32) (((uint64) key * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & mask;

	while (tab[h].used && tab[h].key != key)
		h = (h + 1) & mask;
	return &tab[h];
}

uint64
sift_conn_inserted_rows(Relation heap, Relation index, const SiftConnStamp *stamp)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	const SiftColSchema *schema;
	uint64		since = 0;

	sift_col_flush_held();
	schema = sift_col_rel(index)->schema;
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		TransactionId xs;

		if (sift_tomb_is_seg(e))
			continue;
		if (e->kind == SIFT_SEG_IMMUTABLE)
			since += !sift_seg_xstamp(e, &xs) || TransactionIdFollows(xs, stamp->xmin) ? e->ndocs : 0;
		else if (sift_seg_is_wbuf(e))
			since += sift_cwbuf_inserted_since(index, e, schema, stamp->xmin);
	}
	pfree(dir);
	return since;
}

SiftConnWhy
sift_conn_changed(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	return sift_conn_changed_by(heap, index, stamp, snap, cap, false, out);
}

/* adds: the keys of the records of inserted entries (KEYADD, a scope relation's journal) are in the set as well as those of ended ones */
SiftConnWhy
sift_conn_changed_by(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, bool adds, SiftConnKeys *out)
{
	SiftTombLog *log;
	SiftConnWhy why = sift_conn_vouches(heap, index, stamp, snap);
	const SiftTomb *rec;
	int			n;
	KeyRec	   *tab;
	uint32		mask;
	int64	   *keys;
	bool	   *kept;
	uint32		nk = 0;
	TransactionId last = InvalidTransactionId;
	bool		last_void = false;

	*out = (SiftConnKeys) {0};
	if (why != SIFT_CONN_OK)
		return why;
	log = sift_col_log(index);
	rec = sift_tomb_find(log, SIFT_TOMB_KEYS_KEY, &n);
	/* the distinct keys are at most cap + 1 before the read refuses: the table of a log that overflows is no larger, and the records past the one that overflows it are not looked at */
	mask = pg_nextpower2_32(2 * Max(Min((uint64) n, (uint64) cap + 1), 1)) - 1;
	tab = sift_rows_alloc((mask + 1) * sizeof(KeyRec));
	memset(tab, 0, (mask + 1) * sizeof(KeyRec));
	for (int i = 0; i < n; i++)
	{
		KeyRec	   *slot;
		bool		keeps = (rec[i].flags & SIFT_TOMB_KEPT) != 0;

		if ((rec[i].kind != SIFT_TOMB_KEYCHG && !(adds && rec[i].kind == SIFT_TOMB_KEYADD)) || TransactionIdPrecedes(rec[i].xid, stamp->xmin))
			continue;
		if (rec[i].xid != last)
		{
			last = rec[i].xid;
			last_void = sift_xid_state(last) == SIFT_XID_ABORTED;
		}
		if (last_void)
			continue;
		slot = key_slot(tab, mask, (int64) rec[i].succ);
		if (slot->used)
			slot->kept &= keeps;
		else
		{
			*slot = (KeyRec) {(int64) rec[i].succ, keeps, true};
			if (++nk > cap)
			{
				pfree(tab);
				return SIFT_CONN_OVERFLOW;
			}
		}
	}
	keys = nk > 0 ? palloc(nk * sizeof(int64)) : NULL;
	kept = nk > 0 ? palloc(nk * sizeof(bool)) : NULL;
	for (uint32 i = 0, w = 0; w < nk; i++)
		if (tab[i].used)
			keys[w++] = tab[i].key;
	if (nk > 1)
		sift_sort_i64(keys, nk);
	for (uint32 i = 0; i < nk; i++)
		kept[i] = key_slot(tab, mask, keys[i])->kept;
	pfree(tab);
	*out = (SiftConnKeys) {.keys = keys,.kept = kept,.n = nk,.cap = nk};
	return SIFT_CONN_OK;
}

bool
sift_conn_has(const SiftConnKeys *k, int64 key)
{
	uint32		lo = 0,
				hi = k->n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (k->keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < k->n && k->keys[lo] == key;
}

void
sift_conn_keys_free(SiftConnKeys *k)
{
	if (k->keys)
		pfree(k->keys);
	if (k->kept)
		pfree(k->kept);
	*k = (SiftConnKeys) {0};
}

const char *
sift_conn_why_name(SiftConnWhy why)
{
	static const char *const names[] = {"ok", "no journal", "log not complete", "other lineage", "trimmed", "snapshot", "snapshot older than the stamp", "stamp aged", "too many changed keys"};

	return names[why];
}
