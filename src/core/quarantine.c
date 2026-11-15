/*
 * core/quarantine.c - see quarantine.h: the record of a damaged index, the registry the backends share and the rule that tells a standby's passing mix of pages from damage.
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/spin.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/dsmreg.h"
#include "core/errctx.h"
#include "core/quarantine.h"
#include "core/sift.h"
#include "core/sql.h"

PG_FUNCTION_INFO_V1(sift_quarantine_rows);

#define QUAR_NAME		"sift_quarantine_1"
#define QUAR_ENTRIES	256
#define QUAR_LOCAL		8
#define QUAR_REASON_LEN	SIFT_MAINT_ERRMSG_LEN

/* A replay applies a record of several pages one page at a time: on a standby damage counts when it is seen again this long after it was first seen */
#define QUAR_STANDBY_CONFIRM_US	USECS_PER_SEC

int			sift_on_corruption = SIFT_ON_CORRUPTION_QUARANTINE;

static const struct config_enum_entry on_corruption_options[] = {
	{"quarantine", SIFT_ON_CORRUPTION_QUARANTINE, false},
	{"error", SIFT_ON_CORRUPTION_ERROR, false},
	{NULL, 0, false}
};

/* One index file. Counters are what the backend that skipped it has not handed over yet (an entry of the registry holds the totals) */
typedef struct QuarEntry
{
	bool		used;
	bool		confirmed;
	bool		persisted;
	Oid			db;
	Oid			spc;
	RelFileNumber rel;
	Oid			indexoid;
	TimestampTz detected_at;
	TimestampTz first_seen;
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
same_file(const QuarEntry *e, const QuarEntry *key)
{
	return e->used && e->rel == key->rel && e->db == key->db && e->spc == key->spc;
}

/* The entry of the file in the n entries of list, or the one to take: a free one, else the oldest. No allocation and no error: it runs under a spinlock and inside an error */
static QuarEntry *
slot(QuarEntry *list, int n, const QuarEntry *key)
{
	QuarEntry  *free = NULL,
			   *oldest = &list[0];

	for (int i = 0; i < n; i++)
	{
		if (same_file(&list[i], key))
			return &list[i];
		if (!list[i].used && free == NULL)
			free = &list[i];
		if (list[i].used && list[i].detected_at < oldest->detected_at)
			oldest = &list[i];
	}
	return free ? free : oldest;
}

static bool
confirms(QuarEntry *e, TimestampTz now, bool recovery)
{
	if (!recovery)
		return true;
	if (e->first_seen == 0)
		e->first_seen = now;
	return now - e->first_seen >= QUAR_STANDBY_CONFIRM_US;
}

/* Takes what src holds into the list: a first sighting, another one of the same file, or the counters of the skips since the last hand-over (src's are left zero); true if the file is in force */
static bool
take(QuarEntry *list, int n, QuarEntry *src, bool recovery)
{
	QuarEntry  *e = slot(list, n, src);

	if (!same_file(e, src))
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
	if (src->detections > 0 || src->confirmed)
		e->confirmed |= src->confirmed || confirms(e, src->detected_at, recovery);
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

			if (take(q->entry, QUAR_ENTRIES, &copy, recovery))
				pg_atomic_write_u32(&q->any, 1);
			local[i].detections = 0;
			local[i].skipped_writes = local[i].skipped_vacuums = local[i].refused_scans = 0;
		}
	SpinLockRelease(&q->mutex);
	unpublished = false;
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
key_of(Relation index, QuarEntry *key)
{
	*key = (QuarEntry) {.used = true,.db = index->rd_locator.dbOid,.spc = index->rd_locator.spcOid,.rel = index->rd_locator.relNumber,
	.indexoid = RelationGetRelid(index)};
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

static void
record(Relation index)
{
	QuarEntry	rec;

	key_of(index, &rec);
	rec.detected_at = GetCurrentTimestamp();
	rec.detections = 1;
	reason_of(&rec);
	take(local, QUAR_LOCAL, &rec, RecoveryInProgress());
	unpublished = true;
	publish(shared);
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
	QuarEntry	key;
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
	e = slot(q->entry, QUAR_ENTRIES, &key);
	found = same_file(e, &key) && e->confirmed;
	SpinLockRelease(&q->mutex);
	return found;
}

bool
sift_index_quarantined(Relation index)
{
	if (sift_on_corruption != SIFT_ON_CORRUPTION_QUARANTINE || !index->rd_index->indisready || !is_sift_index(index))
		return false;
	return overlay_has(index) || (sift_store_meta_flags(index) & SIFT_META_QUARANTINED) != 0;
}

void
sift_quarantine_skip(Relation index, SiftSkipKind kind)
{
	QuarEntry	key;
	QuarEntry  *e;

	key_of(index, &key);
	e = slot(local, QUAR_LOCAL, &key);
	if (!same_file(e, &key))
	{
		*e = key;
		e->confirmed = e->persisted = true;
		e->detected_at = GetCurrentTimestamp();
	}
	e->skipped_writes += kind == SIFT_SKIP_WRITE;
	e->skipped_vacuums += kind == SIFT_SKIP_VACUUM;
	e->refused_scans += kind == SIFT_SKIP_SCAN;
	unpublished = true;
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
		for (int i = 0; i < QUAR_ENTRIES; i++)
			if (q->entry[i].used && q->entry[i].db == MyDatabaseId)
				copy[n++] = q->entry[i];
		SpinLockRelease(&q->mutex);
	}
	else
		for (int i = 0; i < QUAR_LOCAL; i++)
			if (local[i].used && local[i].db == MyDatabaseId)
				copy[n++] = local[i];
	for (int i = 0; i < n; i++)
	{
		Datum		v[11];
		bool		nulls[11] = {false};

		v[0] = ObjectIdGetDatum(copy[i].indexoid);
		v[1] = ObjectIdGetDatum(copy[i].rel);
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
