/*
 * core/quarantine.c - see quarantine.h: the record of a damaged index, the registry the backends share and the rule that tells a standby's passing mix of pages from damage.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "core/maint.h"
#include "col/dsmreg.h"
#include "core/cachebudget.h"
#include "core/errctx.h"
#include "col/f2/col_meta.h"
#include "core/xlog.h"
#include "core/limits.h"
#include "core/progress.h"
#include "core/meta_peek.h"
#include "core/quarantine.h"
#include "core/sift.h"
#include "core/sql.h"
#include "core/worker.h"

PG_FUNCTION_INFO_V1(sift_quarantine_rows);
PG_FUNCTION_INFO_V1(sift_meta_quarantined);

#define QUAR_NAME		"sift_quarantine_1"
#define QUAR_ENTRIES	256
#define QUAR_LOCAL		8
#define QUAR_REASON_LEN	SIFT_MAINT_ERRMSG_LEN

/* The worker that flags a metapage is asked for again, by the next plan or write that meets the index, when this long has passed and the flag is still not there (no slot was free) */
#define QUAR_ASK_RETRY_US		(10 * USECS_PER_SEC)

int			sift_on_corruption = SIFT_ON_CORRUPTION_QUARANTINE;

static const struct config_enum_entry on_corruption_options[] = {
	{"quarantine", SIFT_ON_CORRUPTION_QUARANTINE, false},
	{"error", SIFT_ON_CORRUPTION_ERROR, false},
	{NULL, 0, false}
};

/* One index file. Counters are what the backend that skipped it has not handed over yet (an entry of the registry holds the totals) */
typedef struct QuarKey
{
	Oid			db;
	Oid			spc;
	RelFileNumber rel;
} QuarKey;

typedef struct QuarEntry
{
	bool		used;
	bool		confirmed;
	bool		persisted;
	bool		unflaggable;	/* the metapage cannot be written (it is damaged): the registry alone holds the index out of service, and no worker is asked for it again */
	QuarKey		file;
	Oid			indexoid;
	TimestampTz detected_at;
	TimestampTz asked_at;
	int			sqlerrcode;
	char		reason[QUAR_REASON_LEN];
	uint32		detections;
	uint64		skipped_writes;
	uint64		skipped_vacuums;
	uint64		refused_scans;
} QuarEntry;

typedef struct QuarShm
{
	slock_t		mutex;
	pg_atomic_uint32 any;		/* 1 once an entry is in force: the test that costs nothing while no index was ever quarantined */
	QuarEntry	entry[QUAR_ENTRIES];
} QuarShm;

static QuarShm *shared;
static bool unavailable;
static bool damage_unseen;		/* damage of a sift index was recorded since a catch last looked, and the SQLSTATE of the last */
static int	damage_code;
static QuarEntry local[QUAR_LOCAL];
static bool unpublished;

void
sift_quarantine_define_gucs(void)
{
	DefineCustomEnumVariable("sift.on_corruption",
							 "What a sift index does when it finds its own damage: quarantine (leave service, queries run on the heap) or error.",
							 "quarantine records the damage, raises the error once, and takes the index out of service: statements plan without it and give the answers of the plain "
							 "plans, writes and VACUUM skip it with a warning, until REINDEX rebuilds it. error only raises (the behaviour before 5.0).",
							 &sift_on_corruption, SIFT_ON_CORRUPTION_QUARANTINE, on_corruption_options,
							 PGC_SUSET, 0, NULL, NULL, NULL);
}

static void
quar_init(SIFT_DSM_INIT_ARGS)
{
	QuarShm    *q = ptr;

	memset(q, 0, sizeof(*q));
	SpinLockInit(&q->mutex);
	pg_atomic_init_u32(&q->any, 0);
}

static QuarShm *
table(void)
{
	if (shared == NULL && !unavailable)
		unavailable = (shared = sift_dsm_try(QUAR_NAME, sizeof(QuarShm), quar_init)) == NULL;
	return shared;
}

static bool
is_sift_index(Relation rel)
{
	const SiftCatalog *cat;

	if (rel->rd_rel->relkind != RELKIND_INDEX)
		return false;
	cat = sift_catalog();
	return rel->rd_rel->relam == cat->am || rel->rd_rel->relam == cat->col_am;
}

static bool
same_file(const QuarEntry *e, const QuarKey *key)
{
	return e->used && e->file.rel == key->rel && e->file.db == key->db && e->file.spc == key->spc;
}

/* The entries of the registry in use: the flag in the metapage of a persisted index holds it out of service when its entry is taken, an unpersisted one has only the entry */
static int
entries(void)
{
	return (int) sift_limit(quarantine_entries, QUAR_ENTRIES);
}

/* The entry of the file in the n entries of list, or the one to take: a free one, else the oldest persisted, else the oldest. No allocation and no error: it runs under a spinlock and inside an error */
static QuarEntry *
slot(QuarEntry *list, int n, const QuarKey *key)
{
	QuarEntry  *free = NULL,
			   *oldest = &list[0],
			   *oldest_persisted = NULL;

	for (int i = 0; i < n; i++)
	{
		if (same_file(&list[i], key))
			return &list[i];
		if (!list[i].used && free == NULL)
			free = &list[i];
		if (list[i].used && list[i].detected_at < oldest->detected_at)
			oldest = &list[i];
		if (list[i].used && list[i].persisted && (oldest_persisted == NULL || list[i].detected_at < oldest_persisted->detected_at))
			oldest_persisted = &list[i];
	}
	return free ? free : oldest_persisted ? oldest_persisted : oldest;
}

/* Takes what src holds into the list: a first sighting, another one of the same file, or the counters of the skips since the last hand-over (src's are left zero); true if the file is in force.
 * A replay applies a record of several pages one page at a time, so damage seen during recovery is not taken for damage: only a flag the primary wrote puts an index out of service on a standby */
static bool
take(QuarEntry *list, int n, QuarEntry *src, bool recovery)
{
	QuarEntry  *e = slot(list, n, &src->file);

	if (!same_file(e, &src->file))
	{
		*e = *src;
		e->confirmed = false;
		e->detections = 0;
		e->skipped_writes = e->skipped_vacuums = e->refused_scans = 0;
	}
	e->detections += src->detections;
	e->skipped_writes += src->skipped_writes;
	e->skipped_vacuums += src->skipped_vacuums;
	e->refused_scans += src->refused_scans;
	e->persisted |= src->persisted;
	e->confirmed |= src->confirmed || (src->detections > 0 && !recovery);
	src->detections = 0;
	src->skipped_writes = src->skipped_vacuums = src->refused_scans = 0;
	return e->confirmed;
}

static bool
has_counts(const QuarEntry *e)
{
	return e->used && (e->detections > 0 || e->skipped_writes > 0 || e->skipped_vacuums > 0 || e->refused_scans > 0);
}

/* Hands the shared registry what this backend recorded since (q NULL: it has none) */
static void
publish(QuarShm *q)
{
	bool		recovery = RecoveryInProgress();

	if (q == NULL || !unpublished)
		return;
	SpinLockAcquire(&q->mutex);
	for (int i = 0; i < QUAR_LOCAL; i++)
		if (has_counts(&local[i]))
		{
			QuarEntry	copy = local[i];

			if (take(q->entry, entries(), &copy, recovery))
				pg_atomic_write_u32(&q->any, 1);
			local[i].detections = 0;
			local[i].skipped_writes = local[i].skipped_vacuums = local[i].refused_scans = 0;
		}
	SpinLockRelease(&q->mutex);
	unpublished = false;
}

/*
 * The flag SIFT_META_QUARANTINED of the metapage is written by a worker, not by the statement that found the damage (the transaction of a statement that fails rolls its writes back, and
 * a write inside an error is no place for page changes). The ask is made where the damage is recorded, or where a write or VACUUM first skips an index no worker has flagged (a
 * promoted standby, a server that restarted), and the worker is started when the transaction ends, whichever way it ends. A server with no slot for it keeps the registry alone:
 * sift.quarantine says persisted = false, and the next statement that meets the index asks again.
 */
static void key_of(Relation index, QuarKey *key);
static bool overlay_has(Relation index);

#define QUAR_ASKS	8

typedef struct PersistAsk
{
	Oid			index;
	QuarKey		file;
	char		subject[2 * NAMEDATALEN + 2];
} PersistAsk;

static PersistAsk asks[QUAR_ASKS];
static int	nasks;
static bool xact_hooked;
static bool persisting;

/*
 * Whether the worker is to be asked for now: the registry holds the index out of service with no flag in the metapage, none was asked for in the last QUAR_ASK_RETRY_US (a server with
 * no free slot is asked again by the next plan or write), and the metapage can be written. A server with no registry asks every time.
 */
static bool
ask_due(const QuarKey *key)
{
	QuarShm    *q = table();
	QuarEntry  *e;
	TimestampTz now = GetCurrentTimestamp();
	bool		due;

	if (q == NULL)
		return true;
	SpinLockAcquire(&q->mutex);
	e = slot(q->entry, entries(), key);
	due = same_file(e, key) && e->confirmed && !e->persisted && !e->unflaggable && now - e->asked_at >= QUAR_ASK_RETRY_US;
	if (due)
		e->asked_at = now;
	SpinLockRelease(&q->mutex);
	return due;
}

/* What a transaction ended with is handed to the registry, and the workers asked for are started: a session that skips an index and ends (an autovacuum worker) leaves nothing behind in its own memory */
static void
quarantine_xact(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
		return;
	publish(shared);
	for (int i = 0; i < nasks; i++)
		if (ask_due(&asks[i].file))
			(void) sift_maint_register_worker_oid(asks[i].index, SIFT_WK_QUARANTINE, asks[i].subject);
	nasks = 0;
}

static void
hook_xact(void)
{
	if (!xact_hooked)
	{
		RegisterXactCallback(quarantine_xact, NULL);
		xact_hooked = true;
	}
}

static void
ask_persist(Relation index)
{
	PersistAsk *a;

	if (persisting || RecoveryInProgress() || !IsUnderPostmaster || IsParallelWorker() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	for (int i = 0; i < nasks; i++)
		if (asks[i].index == RelationGetRelid(index))
			return;
	if (nasks == QUAR_ASKS)
		return;
	hook_xact();
	a = &asks[nasks++];
	a->index = RelationGetRelid(index);
	key_of(index, &a->file);
	snprintf(a->subject, sizeof(a->subject), "%s", sift_worker_subject(index));
}

static bool
shared_persisted(const QuarKey *key)
{
	QuarShm    *q = table();
	QuarEntry  *e;
	bool		done;

	if (q == NULL)
		return false;
	SpinLockAcquire(&q->mutex);
	e = slot(q->entry, entries(), key);
	done = same_file(e, key) && e->persisted;
	SpinLockRelease(&q->mutex);
	return done;
}

static void
mark_persisted(Relation index)
{
	QuarKey		key;
	QuarEntry  *e;
	QuarShm    *q = table();

	if (q == NULL)
		return;
	key_of(index, &key);
	SpinLockAcquire(&q->mutex);
	e = slot(q->entry, entries(), &key);
	if (!same_file(e, &key))
		*e = (QuarEntry) {.used = true,.confirmed = true,.file = key,.indexoid = RelationGetRelid(index),.detected_at = GetCurrentTimestamp()};
	e->persisted = true;
	pg_atomic_write_u32(&q->any, 1);
	SpinLockRelease(&q->mutex);
}

static void
stamp_flag(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->flags |= SIFT_META_QUARANTINED;
}

static void
mark_unflaggable(const QuarKey *key)
{
	QuarShm    *q = table();
	QuarEntry  *e;

	if (q == NULL)
		return;
	SpinLockAcquire(&q->mutex);
	e = slot(q->entry, entries(), key);
	if (same_file(e, key))
		e->unflaggable = true;
	SpinLockRelease(&q->mutex);
}

/* A format 2 metapage keeps the flag in state_flags, under the checksum of its struct: one generic WAL record stamps it and seals the page again */
static void
flag_index_f2(Relation index)
{
	Buffer		buf = sift_col_meta_lock(index, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(index);
	Page		page = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(page);

	m->state_flags |= SIFT_META_FLAG_QUARANTINED;
	sift_meta_seal(page, m->common.struct_size);
	sift_xlog_done(&x);
}

static void
flag_index(Relation index)
{
	SiftMetaPeek peek = sift_meta_peek_format(index);

	if ((peek.flags & SIFT_META_QUARANTINED) == 0)
	{
		if (peek.status == SIFT_FMT_V2)
			flag_index_f2(index);
		else
			sift_dir_update(index, &(SiftDirChange) {.fn = stamp_flag,.extra = InvalidBuffer});
	}
	CacheInvalidateRelcacheByRelid(RelationGetRelid(index));
	mark_persisted(index);
}

/*
 * The worker: flags the metapage of the index the registry holds out of service, once (a rebuilt index is another file the registry does not hold). A metapage or directory that
 * is damaged itself cannot be written: the registry alone holds the index out of service, said once with a LOG, and no worker is asked for it again.
 */
void
sift_quarantine_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;
	Relation	index;
	volatile bool unflaggable = false;

	persisting = true;
	StartTransactionCommand();
	if ((index = sift_try_open_index(indexoid, RowExclusiveLock)) != NULL)
	{
		if (is_sift_index(index) && overlay_has(index))
		{
			QuarKey		file;
			char	   *name = MemoryContextStrdup(cx, RelationGetRelationName(index));

			key_of(index, &file);
			PG_TRY();
			{
				flag_index(index);
			}
			PG_CATCH();
			{
				ErrorData  *e;

				MemoryContextSwitchTo(cx);
				e = CopyErrorData();
				if (!sift_damage_caught(e))
					ReThrowError(e);
				FlushErrorState();
				AbortCurrentTransaction();
				mark_unflaggable(&file);
				ereport(LOG,
						(errmsg("sift index \"%s\" is out of service in the registry of the server only", name),
						 errdetail("Its metapage cannot be flagged: %s", e->detail ? e->detail : e->message),
						 errhint("A restart forgets it until the damage is read again. REINDEX INDEX puts the index back in service.")));
				FreeErrorData(e);
				unflaggable = true;
			}
			PG_END_TRY();
		}
		if (!unflaggable)
			index_close(index, RowExclusiveLock);
	}
	if (!unflaggable)
		CommitTransactionCommand();
}

void
sift_damage_prepare(void)
{
	if (sift_on_corruption == SIFT_ON_CORRUPTION_QUARANTINE)
	{
		publish(table());
	}
}

static void
key_of(Relation index, QuarKey *key)
{
	*key = (QuarKey) {.db = index->rd_locator.dbOid,.spc = index->rd_locator.spcOid,.rel = index->rd_locator.relNumber};
}

/* The reason a sift.quarantine row gives: what the error says is wrong, its detail where it has one. Called inside the error being raised, after its text is built */
static void
reason_of(QuarEntry *e)
{
	ErrorData  *ed = CopyErrorData();
	const char *text = ed->detail ? ed->detail : ed->message;

	e->sqlerrcode = ed->sqlerrcode;
	memcpy(e->reason, text, pg_mbcliplen(text, strlen(text), sizeof(e->reason) - 1));
	FreeErrorData(ed);
}

/* The error that raised the damage may be caught (a guard, an EXCEPTION block) and never reach the log: the first cause of an index leaving service is logged here */
static void
log_first_cause(Relation index, const QuarEntry *rec)
{
	ereport(LOG,
			(errmsg("sift index \"%s\" is out of service after damage", RelationGetRelationName(index)),
			 errdetail("First cause, SQLSTATE %s: %s", unpack_sql_state(rec->sqlerrcode), rec->reason)));
}

static void
record(Relation index)
{
	QuarEntry	rec;
	bool		was_out;

	rec = (QuarEntry) {.used = true,.indexoid = RelationGetRelid(index),.detected_at = GetCurrentTimestamp()};
	key_of(index, &rec.file);
	rec.detections = 1;
	reason_of(&rec);
	damage_unseen = true;
	damage_code = rec.sqlerrcode;
	if (persisting)
		return;
	was_out = overlay_has(index);
	take(local, QUAR_LOCAL, &rec, RecoveryInProgress());
	unpublished = true;
	publish(shared);
	if (!was_out && overlay_has(index))
		log_first_cause(index, &rec);
	ask_persist(index);
}

int
sift_damage_report(Relation index)
{
	Oid			scope;
	bool		diagnostic;
	bool		scoped = sift_errctx_scope(&scope, &diagnostic);
	Relation	rel = index;

	if (sift_on_corruption != SIFT_ON_CORRUPTION_QUARANTINE || diagnostic)
		return 0;
	if (rel == NULL)
	{
		if (!scoped || !IsTransactionState() || !RelationIsValid(rel = RelationIdGetRelation(scope)))
			return 0;
	}
	if (is_sift_index(rel))
		record(rel);
	if (rel != index)
		RelationClose(rel);
	return 0;
}

static bool
overlay_has(Relation index)
{
	QuarKey		key;
	QuarEntry  *e;
	bool		found;
	QuarShm    *q = table();

	key_of(index, &key);
	for (int i = 0; i < QUAR_LOCAL; i++)
		if (same_file(&local[i], &key) && local[i].confirmed)
			return true;
	if (q == NULL || pg_atomic_read_u32(&q->any) == 0)
		return false;
	SpinLockAcquire(&q->mutex);
	e = slot(q->entry, entries(), &key);
	found = same_file(e, &key) && e->confirmed;
	SpinLockRelease(&q->mutex);
	return found;
}

char *
sift_quarantine_reason(Relation index)
{
	QuarKey		key;
	QuarShm    *q = table();
	char		reason[QUAR_REASON_LEN] = "";

	key_of(index, &key);
	for (int i = 0; i < QUAR_LOCAL && reason[0] == '\0'; i++)
		if (same_file(&local[i], &key))
			strlcpy(reason, local[i].reason, sizeof(reason));
	if (reason[0] == '\0' && q != NULL && pg_atomic_read_u32(&q->any) != 0)
	{
		QuarEntry  *e;

		SpinLockAcquire(&q->mutex);
		e = slot(q->entry, entries(), &key);
		if (same_file(e, &key))
			strlcpy(reason, e->reason, sizeof(reason));
		SpinLockRelease(&q->mutex);
	}
	return reason[0] != '\0' ? pstrdup(reason) : NULL;
}

/* A plan that leaves a quarantined index out asks for the worker that flags its metapage when the registry holds it with no flag (a worker that found no slot, or damage a parallel worker found) */
void
sift_quarantine_planned(Relation index)
{
	QuarKey		key;
	QuarShm    *q = table();
	QuarEntry  *e;
	bool		unflagged;

	if (q == NULL || pg_atomic_read_u32(&q->any) == 0)
		return;
	key_of(index, &key);
	SpinLockAcquire(&q->mutex);
	e = slot(q->entry, entries(), &key);
	unflagged = same_file(e, &key) && e->confirmed && !e->persisted && !e->unflaggable;
	SpinLockRelease(&q->mutex);
	if (unflagged)
		ask_persist(index);
}

bool
sift_index_known_out(Relation index)
{
	return sift_on_corruption == SIFT_ON_CORRUPTION_QUARANTINE && overlay_has(index);
}

bool
sift_index_out(Relation index, uint32 meta_flags)
{
	return sift_on_corruption == SIFT_ON_CORRUPTION_QUARANTINE && ((meta_flags & SIFT_META_QUARANTINED) != 0 || overlay_has(index));
}

typedef struct FlagMemo
{
	Oid			index;
	bool		flagged;
} FlagMemo;

/* In the memory of the relcache entry of the index, where the relcache frees it when it invalidates the entry: the flag of the metapage is forgotten then */
typedef struct FlagForget
{
	MemoryContextCallback cb;
	Oid			index;
} FlagForget;

static HTAB *flag_memo;

/*
 * The flag of a metapage changes with a commit that invalidates the relcache entry of its index (the worker that writes it, a REINDEX), so the backend reads it once in between and not at
 * every plan. No relcache callback for it: a backend has ten of those slots for all its libraries.
 */
static void
flag_forget(void *arg)
{
	Oid			index = ((FlagForget *) arg)->index;

	if (flag_memo != NULL)
		hash_search(flag_memo, &index, HASH_REMOVE, NULL);
}

static bool
meta_flagged(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	FlagMemo   *m;
	FlagForget *f;
	bool		flagged;

	if (flag_memo == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(FlagMemo),.hcxt = sift_cache_create_context("sift cache quarantine flags")};

		flag_memo = hash_create("sift quarantine flags", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if ((m = hash_search(flag_memo, &relid, HASH_FIND, NULL)) != NULL)
		return m->flagged;
	flagged = (sift_meta_peek_format(index).flags & SIFT_META_QUARANTINED) != 0;
	m = hash_search(flag_memo, &relid, HASH_ENTER, NULL);
	m->flagged = flagged;
	f = MemoryContextAlloc(index->rd_indexcxt, sizeof(FlagForget));
	f->cb.func = flag_forget;
	f->cb.arg = f;
	f->index = relid;
	MemoryContextRegisterResetCallback(index->rd_indexcxt, &f->cb);
	return m->flagged;
}

/* The relations the quarantine is about: a sift or sift_col index in service, while sift.on_corruption says quarantine (a sift_table, or anything else, is never quarantined) */
static bool
guarded(Relation rel)
{
	return sift_on_corruption == SIFT_ON_CORRUPTION_QUARANTINE && is_sift_index(rel) && rel->rd_index->indisready;
}

bool
sift_index_quarantined(Relation index)
{
	return guarded(index) && (overlay_has(index) || meta_flagged(index));
}

bool
sift_quarantine_cost(Relation index, double index_pages, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages)
{
	if (!sift_index_quarantined(index))
		return false;
	sift_quarantine_planned(index);
	*startup = *total = 1e30;
	*selectivity = 1.0;
	*correlation = 0.0;
	*pages = Max(index_pages, 1);
	return true;
}

void
sift_quarantine_skip(Relation index, SiftQuarSkip kind)
{
	QuarKey		key;
	QuarEntry  *e;

	key_of(index, &key);
	e = slot(local, QUAR_LOCAL, &key);
	if (!same_file(e, &key))
	{
		*e = (QuarEntry) {.used = true,.confirmed = true,.file = key,.indexoid = RelationGetRelid(index),.detected_at = GetCurrentTimestamp()};
		if (!shared_persisted(&key))
			ask_persist(index);
	}
	e->skipped_writes += kind == SIFT_SKIP_WRITE;
	e->skipped_vacuums += kind == SIFT_SKIP_VACUUM;
	e->refused_scans += kind == SIFT_SKIP_SCAN;
	unpublished = true;
	hook_xact();
}

/* One notice for each index and a minute, whatever the writes and VACUUMs that skip it: the count is in sift.quarantine */
static bool
warn_due(Relation index)
{
	static struct
	{
		Oid			relid;
		TimestampTz at;
	}			seen[QUAR_LOCAL];
	TimestampTz now = GetCurrentTimestamp();
	int			at = 0;

	for (int i = 0; i < QUAR_LOCAL; i++)
	{
		if (seen[i].relid == RelationGetRelid(index))
		{
			at = i;
			break;
		}
		if (seen[i].at < seen[at].at)
			at = i;
	}
	if (seen[at].relid == RelationGetRelid(index) && now - seen[at].at < 60 * USECS_PER_SEC)
		return false;
	seen[at].relid = RelationGetRelid(index);
	seen[at].at = now;
	return true;
}

static void
warn_skipped(Relation index, SiftQuarSkip kind)
{
	if (!warn_due(index))
		return;
	ereport(WARNING,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("sift index \"%s\" is quarantined; %s", RelationGetRelationName(index),
					kind == SIFT_SKIP_WRITE ? "the write was not indexed" : "VACUUM skipped it"),
			 errhint("Queries and maintenance run without it. REINDEX INDEX %s puts it back in service.", sift_index_ident(index))));
}

/*
 * The flag of the metapage as a write or a VACUUM finds it now, not as the planner remembers it (meta_flagged): a metapage that went bad since is raised here by the reader that validates it,
 * the damage is recorded and the statement fails once, where the work would go on and write through the state it holds
 */
static bool
meta_flagged_now(Relation index)
{
	SiftMetaPeek peek = sift_meta_peek_format(index);

	if (peek.status == SIFT_FMT_V2 || peek.status == SIFT_FMT_OTHER)
		return (peek.flags & SIFT_META_QUARANTINED) != 0;
	return (sift_store_meta_flags(index) & SIFT_META_QUARANTINED) != 0;
}

void
sift_quarantine_mark(Relation index)
{
	if (is_sift_index(index) && overlay_has(index))
		flag_index(index);
}

bool
sift_quarantine_skips(Relation index, SiftQuarSkip kind)
{
	if (!sift_index_quarantined(index))
		return false;
	sift_quarantine_stopped(index, kind);
	return true;
}

bool
sift_quarantine_run(Relation index, SiftQuarSkip kind, SiftGuarded fn, void *arg)
{
	if (guarded(index) && (overlay_has(index) || meta_flagged_now(index)))
	{
		sift_quarantine_stopped(index, kind);
		return false;
	}
	fn(arg);
	return true;
}

bool
sift_damage_caught(const ErrorData *e)
{
	bool		damage = damage_unseen && e->sqlerrcode == damage_code;

	if (damage)
		damage_unseen = false;
	return damage;
}

void
sift_quarantine_stopped(Relation index, SiftQuarSkip kind)
{
	sift_quarantine_skip(index, kind);
	warn_skipped(index, kind);
}

void
sift_quarantine_require(Relation index, const char *what)
{
	if (sift_index_quarantined(index))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sift index \"%s\" is quarantined; %s has no result without it", RelationGetRelationName(index), what),
				 errhint("REINDEX INDEX %s puts it back in service.", sift_index_ident(index))));
}

/* The indexes of this database that the registry holds out of service, as a list of names for a hint; NULL when there is none */
const char *
sift_quarantine_names(void)
{
	QuarShm    *q = table();
	Oid			ids[QUAR_ENTRIES];
	int			n = 0;
	StringInfoData names;

	if (q == NULL || pg_atomic_read_u32(&q->any) == 0 || !IsTransactionState())
		return NULL;
	SpinLockAcquire(&q->mutex);
	for (int i = 0; i < entries(); i++)
		if (q->entry[i].used && q->entry[i].confirmed && q->entry[i].file.db == MyDatabaseId)
			ids[n++] = q->entry[i].indexoid;
	SpinLockRelease(&q->mutex);
	initStringInfo(&names);
	for (int i = 0; i < n; i++)
	{
		char	   *name = get_rel_name(ids[i]);

		if (name != NULL)
			appendStringInfo(&names, "%s\"%s\"", names.len ? ", " : "", name);
	}
	return names.len ? names.data : NULL;
}

void
sift_quarantine_net(Relation index, uint32 meta_flags)
{
	SharedInvalidationMessage msg;

	if (!sift_index_out(index, meta_flags))
		return;
	sift_quarantine_skip(index, SIFT_SKIP_SCAN);
	msg.rc.id = SHAREDINVALRELCACHE_ID;
	msg.rc.dbId = MyDatabaseId;
	msg.rc.relId = RelationGetRelid(index);
	LocalExecuteInvalidationMessage(&msg);
	msg.rc.relId = index->rd_index->indrelid;
	LocalExecuteInvalidationMessage(&msg);
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("sift index \"%s\" is out of service; the statement must be planned again", RelationGetRelationName(index)),
			 errhint("Run the statement again: it is planned without the index. REINDEX INDEX %s puts it back in service.", sift_index_ident(index))));
}

/* sift._quarantine(): the files of this database the registry knows, one row each (the view sift.quarantine joins them to their indexes) */
Datum
sift_quarantine_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	QuarEntry  *copy = palloc(sizeof(QuarEntry) * QUAR_ENTRIES);
	QuarShm    *q;
	int			n = 0;

	InitMaterializedSRF(fcinfo, 0);
	publish(table());
	if ((q = table()) != NULL)
	{
		SpinLockAcquire(&q->mutex);
		for (int i = 0; i < entries(); i++)
			if (q->entry[i].used && q->entry[i].file.db == MyDatabaseId)
				copy[n++] = q->entry[i];
		SpinLockRelease(&q->mutex);
	}
	else
		for (int i = 0; i < QUAR_LOCAL; i++)
			if (local[i].used && local[i].file.db == MyDatabaseId)
				copy[n++] = local[i];
	for (int i = 0; i < n; i++)
	{
		Datum		v[11];
		bool		nulls[11] = {false};

		v[0] = ObjectIdGetDatum(copy[i].indexoid);
		v[1] = ObjectIdGetDatum(copy[i].file.rel);
		v[2] = TimestampTzGetDatum(copy[i].detected_at);
		v[3] = BoolGetDatum(copy[i].confirmed);
		v[4] = BoolGetDatum(copy[i].persisted);
		v[5] = CStringGetTextDatum(unpack_sql_state(copy[i].sqlerrcode));
		v[6] = CStringGetTextDatum(copy[i].reason);
		v[7] = Int64GetDatum(copy[i].detections);
		v[8] = Int64GetDatum((int64) copy[i].skipped_writes);
		v[9] = Int64GetDatum((int64) copy[i].skipped_vacuums);
		v[10] = Int64GetDatum((int64) copy[i].refused_scans);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}

/* Whether the metapage of the index flags it out of service, whatever the registry says (the rows of sift.quarantine that no backend has met since the server started); false while a REINDEX holds the index, as the inspectors answer NULL then */
Datum
sift_meta_quarantined(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	rel;
	bool		flagged = false;

	if (!ConditionalLockRelationOid(indexoid, AccessShareLock))
		PG_RETURN_BOOL(false);
	rel = try_relation_open(indexoid, NoLock);
	if (rel != NULL && is_sift_index(rel))
		flagged = (sift_meta_peek_format(rel).flags & SIFT_META_QUARANTINED) != 0;
	if (rel != NULL)
		relation_close(rel, NoLock);
	UnlockRelationOid(indexoid, AccessShareLock);
	PG_RETURN_BOOL(flagged);
}
