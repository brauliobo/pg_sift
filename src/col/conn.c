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
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/conn.h"
#include "core/rowmem.h"
#include "core/tomb.h"

/* A stamp older than this many transaction ids is not compared with a current one: the comparison of two ids wraps at 2^31 and the journal drops records past this */
#define CONN_AGE_MAX	(1U << 30)

/*
 * What this backend knows of the journals it writes: the lineage of an index (0: none), by the oid and the file, dropped by a relcache invalidation of the index (the
 * transaction that connects a child invalidates it, and a writer takes its lock on the table after that transaction ended: the table lock it holds out until then).
 */
typedef struct JournalEnt
{
	Oid			relid;			/* hash key */
	RelFileNumber relnumber;
	uint64		lineage;
} JournalEnt;

static HTAB *journals;

static void
journal_inval(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	JournalEnt *e;

	if (journals == NULL)
		return;
	if (relid != InvalidOid)
	{
		(void) hash_search(journals, &relid, HASH_REMOVE, NULL);
		return;
	}
	hash_seq_init(&seq, journals);
	while ((e = hash_seq_search(&seq)) != NULL)
		(void) hash_search(journals, &e->relid, HASH_REMOVE, NULL);
}

/* The lineage of the journal of the index, read from the header page once for each use of the index until it is invalidated */
static uint64
journal_lineage(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	JournalEnt *e;
	bool		found;
	SiftTombKeys k;
	uint64		n;
	SiftDirSnapshot *dir;

	if (journals == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(JournalEnt),.hcxt = CacheMemoryContext};

		journals = hash_create("sift key journals", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterRelcacheCallback(journal_inval, (Datum) 0);
	}
	e = hash_search(journals, &relid, HASH_FIND, NULL);
	if (e != NULL && e->relnumber == index->rd_locator.relNumber)
		return e->lineage;
	dir = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, dir);
	if (!sift_tomb_peek_keys(index, dir, &k, &n))
		memset(&k, 0, sizeof(k));
	pfree(dir);
	e = hash_search(journals, &relid, HASH_ENTER, &found);
	e->relnumber = index->rd_locator.relNumber;
	e->lineage = k.lineage;
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

bool
sift_conn_record(Relation index, Oid keytype, Datum key, SiftTomb *rec)
{
	int64		k;

	if (!sift_col_track_on(index) || journal_lineage(index) == 0 || !key64(keytype, key, &k))
		return false;
	*rec = (SiftTomb) {.key = SIFT_TOMB_KEYS_KEY,.xid = GetCurrentTransactionId(),.cid = sift_col_stmt_cid(),.succ = (uint64) k,.kind = SIFT_TOMB_KEYCHG};
	return true;
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
	k.trim = InvalidTransactionId;
	sift_tomb_set_keys(index, log, &k);
	sift_tomb_set_format(index, SIFT_COL_FORMAT_KEYJOURNAL);
	CacheInvalidateRelcache(index);
	return k.lineage;
}

static void
check_key(Relation heap, Relation index)
{
	AttrNumber	hatt;
	Oid			typid;

	if (!sift_col_row_key_on(index))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" cannot be connected: it does not track a key column", RelationGetRelationName(index)),
						errhint("Connect to an index with track_changes naming the unique key of its table.")));
	hatt = index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1];
	typid = TupleDescAttr(RelationGetDescr(heap), hatt - 1)->atttypid;
	if (typid != INT2OID && typid != INT4OID && typid != INT8OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" cannot be connected: its key column is not an integer", RelationGetRelationName(index)),
						errhint("The key of the parent index is an int2, int4 or int8 column.")));
}

uint64
sift_conn_enable(Relation heap, Relation index)
{
	SiftTombLog *log;

	check_key(heap, index);
	/* writers of the table are held out until the transaction ends: every transaction that wrote it before is finished, so the journal is complete from here */
	LockRelationOid(RelationGetRelid(heap), ShareLock);
	if ((log = sift_col_log(index)) == NULL || !sift_col_track_ok(index, heap, NULL))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("index \"%s\" cannot be connected: its tombstone log is not complete", RelationGetRelationName(index)),
						errhint("REINDEX INDEX %s, without CONCURRENTLY, makes it so.", RelationGetRelationName(index))));
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
sift_conn_connect(Relation child_index, Relation parent_heap, Relation parent_index)
{
	ObjectAddress child,
				parent;

	(void) sift_conn_enable(parent_heap, parent_index);
	ObjectAddressSet(child, RelationRelationId, RelationGetRelid(child_index));
	ObjectAddressSet(parent, RelationRelationId, RelationGetRelid(parent_index));
	recordDependencyOn(&child, &parent, DEPENDENCY_NORMAL);
}

void
sift_conn_build(Relation heap, Relation index)
{
	Oid			origin = RelationGetRelid(index);
	bool		wanted = sift_conn_children(origin) != NIL;

	if (!wanted && (origin = sift_col_track_swap_origin(heap, index)) != InvalidOid)
		wanted = sift_conn_children(origin) != NIL;
	if (wanted && sift_col_row_key_on(index))
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

static int
cmp_key(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

SiftConnWhy
sift_conn_changed(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	SiftTombLog *log;
	SiftConnWhy why = sift_conn_vouches(heap, index, stamp, snap);
	const SiftTomb *rec;
	int			n;
	int64	   *keys;
	uint32		nk = 0;
	TransactionId last = InvalidTransactionId;
	bool		last_void = false;

	*out = (SiftConnKeys) {0};
	if (why != SIFT_CONN_OK)
		return why;
	log = sift_col_log(index);
	rec = sift_tomb_find(log, SIFT_TOMB_KEYS_KEY, &n);
	keys = n > 0 ? sift_rows_alloc(n * sizeof(int64)) : NULL;
	for (int i = 0; i < n; i++)
	{
		if (rec[i].kind != SIFT_TOMB_KEYCHG || TransactionIdPrecedes(rec[i].xid, stamp->xmin))
			continue;
		if (rec[i].xid != last)
		{
			last = rec[i].xid;
			last_void = sift_xid_state(last) == SIFT_XID_ABORTED;
		}
		if (!last_void)
			keys[nk++] = (int64) rec[i].succ;
	}
	if (nk > 1)
	{
		uint32		w = 1;

		qsort(keys, nk, sizeof(int64), cmp_key);
		for (uint32 i = 1; i < nk; i++)
			if (keys[i] != keys[w - 1])
				keys[w++] = keys[i];
		nk = w;
	}
	if (nk > cap)
	{
		if (keys)
			pfree(keys);
		return SIFT_CONN_OVERFLOW;
	}
	*out = (SiftConnKeys) {.keys = keys,.n = nk,.cap = nk};
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
	*k = (SiftConnKeys) {0};
}

const char *
sift_conn_why_name(SiftConnWhy why)
{
	static const char *const names[] = {"ok", "no journal", "log not complete", "other lineage", "trimmed", "snapshot", "snapshot older than the stamp", "stamp aged", "too many changed keys"};

	return names[why];
}
