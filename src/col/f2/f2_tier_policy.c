/*
 * col/f2/f2_tier_policy.c - the policies of a relation (sift.tier_policies, sift.ttl_policies), read as the owner of those tables so that a reader of a tiered table needs no privilege on them, and
 * what a session keeps of them: the backend and the cache of each policy, and the tier directory of the last relation it read.
 *
 * A policy id never changes what it names: the location, the endpoint and the namespace of the objects are fixed for it, and sift.set_tier_policy gives a relation that changes them a new id. A
 * backend a session opened for an id is therefore right for as long as there is a ref that names the id.
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "col/f2/col_meta.h"
#include "col/f2/f2_tier_host.h"
#include "core/cachebudget.h"
#include "core/page_io.h"

#define POLICY_SELECT \
	"SELECT policy_id, uri, endpoint, (extract(epoch FROM move_after) * 1000000)::int8, cache_mb, scope_id FROM sift.tier_policies WHERE "

/*
 * The catalog tables are the extension owner's: what a caller may do with a policy is decided by the functions, not by a grant. A VACUUM that reads them may have no snapshot set, which SPI needs.
 */
typedef struct CatalogUser
{
	Oid			uid;
	int			sec;
	bool		pushed;
} CatalogUser;

static void
catalog_user_enter(CatalogUser *save)
{
	Oid			relid = get_relname_relid("tier_policies", get_namespace_oid("sift", false));
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for relation %u", relid);
	GetUserIdAndSecContext(&save->uid, &save->sec);
	SetUserIdAndSecContext(((Form_pg_class) GETSTRUCT(tup))->relowner, save->sec | SECURITY_LOCAL_USERID_CHANGE);
	ReleaseSysCache(tup);
	save->pushed = !ActiveSnapshotSet();
	if (save->pushed)
		PushActiveSnapshot(GetTransactionSnapshot());
}

static void
catalog_user_leave(const CatalogUser *save)
{
	if (save->pushed)
		PopActiveSnapshot();
	SetUserIdAndSecContext(save->uid, save->sec);
}

static char *
text_col(MemoryContext cx, int col, bool *isnull)
{
	Datum		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, col, isnull);

	return *isnull ? NULL : MemoryContextStrdup(cx, TextDatumGetCString(d));
}

static int64
int_col(int col)
{
	bool		isnull;
	Datum		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, col, &isnull);

	return DatumGetInt64(d);
}

static bool
policy_row(const char *where, Oid argtype, Datum arg, SiftTierPolicy *out)
{
	MemoryContext cx = CurrentMemoryContext;
	CatalogUser save;
	StringInfoData sql;
	int			rc;
	bool		found,
				isnull;

	initStringInfo(&sql);
	appendStringInfoString(&sql, POLICY_SELECT);
	appendStringInfoString(&sql, where);
	catalog_user_enter(&save);
	SPI_connect();
	rc = SPI_execute_with_args(sql.data, 1, &argtype, &arg, NULL, true, 1);
	SIFT_INVARIANT(rc == SPI_OK_SELECT, "sift: could not read sift.tier_policies");
	found = SPI_processed == 1;
	if (found)
	{
		*out = (SiftTierPolicy) {
			.policy_id = (uint32) DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull)),
			.uri = text_col(cx, 2, &isnull),
			.endpoint = text_col(cx, 3, &isnull),
			.move_after_us = int_col(4),
			.cache_mb = (uint32) DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 5, &isnull)),
			.scope_id = (uint64) int_col(6)
		};
	}
	SPI_finish();
	catalog_user_leave(&save);
	pfree(sql.data);
	return found;
}

bool
sift_tier_policy_of(Relation rel, SiftTierPolicy *out)
{
	return policy_row("relid::oid = $1", OIDOID, ObjectIdGetDatum(RelationGetRelid(rel)), out);
}

bool
sift_ttl_policy_of(Relation rel, SiftTtlPolicy *out)
{
	MemoryContext cx = CurrentMemoryContext;
	Datum		arg = ObjectIdGetDatum(RelationGetRelid(rel));
	Oid			argtype = OIDOID;
	CatalogUser save;
	int			rc;
	bool		found,
				isnull;

	catalog_user_enter(&save);
	SPI_connect();
	rc = SPI_execute_with_args("SELECT col, (extract(epoch FROM retention) * 1000000)::int8 FROM sift.ttl_policies WHERE relid::oid = $1", 1, &argtype, &arg, NULL, true, 1);
	SIFT_INVARIANT(rc == SPI_OK_SELECT, "sift: could not read sift.ttl_policies");
	found = SPI_processed == 1;
	if (found)
		*out = (SiftTtlPolicy) {.col = text_col(cx, 1, &isnull),.retention_us = int_col(2)};
	SPI_finish();
	catalog_user_leave(&save);
	return found;
}

/* ---- what a session keeps ---- */

typedef struct Opened
{
	uint32		policy_id;
	bool		loaded;
	SiftTierPolicy policy;
	SiftTierBackend *backend;
	SiftTierCache *cache;
} Opened;

static Opened *opened;
static int	nopened;
static MemoryContext policy_cx;

/* What a policy opened is in use by the statement that asked for it, so the budget never drops it: the setting that keeps nothing does, between statements */
static uint64
policies_oldest(void)
{
	return UINT64_MAX;
}

static bool
policies_evict_one(void)
{
	return false;
}

static void
policies_evict_all(void)
{
	for (int i = 0; i < nopened; i++)
	{
		if (opened[i].cache != NULL)
			sift_tier_cache_close(opened[i].cache);
		if (opened[i].backend != NULL)
			sift_tier_close(opened[i].backend);
	}
	opened = NULL;
	nopened = 0;
	if (policy_cx != NULL)
		MemoryContextReset(policy_cx);
}

static const SiftCacheKind policies_kind = {"tier_policies", policies_oldest, policies_evict_one, policies_evict_all};

static MemoryContext
policy_context(void)
{
	if (policy_cx == NULL)
	{
		policy_cx = sift_cache_create_context("sift cache tier policies");
		sift_cache_register(&policies_kind);
	}
	return policy_cx;
}

static Opened *
opened_for(const SiftTierPolicy *p)
{
	for (int i = 0; i < nopened; i++)
		if (opened[i].policy_id == p->policy_id)
			return &opened[i];
	opened = opened == NULL ? MemoryContextAlloc(policy_context(), sizeof(Opened)) : repalloc(opened, (nopened + 1) * sizeof(Opened));
	opened[nopened] = (Opened) {.policy_id = p->policy_id};
	return &opened[nopened++];
}

/* The policy a ref names, read once per session: what an id names never changes */
void
sift_tier_policy_by_id(Relation rel, uint32 policy_id, SiftTierPolicy *out)
{
	Opened	   *o = opened_for(&(SiftTierPolicy) {.policy_id = policy_id});

	if (!o->loaded)
	{
		MemoryContext old = MemoryContextSwitchTo(policy_context());
		bool		found = policy_row("policy_id = $1", INT4OID, Int32GetDatum((int32) policy_id), &o->policy);

		MemoryContextSwitchTo(old);
		if (!found)
			SIFT_CORRUPT(rel, "The tier directory names policy %u, which sift.tier_policies does not have.", policy_id);
		o->loaded = true;
	}
	*out = o->policy;
}

SiftTierBackend *
sift_tier_backend_try(const SiftTierPolicy *p, SiftTierErr *err)
{
	Opened	   *o = opened_for(p);
	MemoryContext old;

	if (o->backend == NULL)
	{
		old = MemoryContextSwitchTo(policy_context());
		o->backend = sift_tier_open(&(SiftTierOpen) {.uri = p->uri,.endpoint = p->endpoint}, err);
		MemoryContextSwitchTo(old);
	}
	return o->backend;
}

SiftTierBackend *
sift_tier_backend(const SiftTierPolicy *p)
{
	SiftTierErr err = {0};
	SiftTierBackend *be = sift_tier_backend_try(p, &err);

	if (be == NULL)
		sift_tier_raise(err.code, &err, "cannot open the object store of the tier policy");
	return be;
}

static void
make_dir(const char *path)
{
	if (MakePGDirectory(path) != 0 && errno != EEXIST)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not create directory \"%s\": %m", path)));
}

SiftTierCache *
sift_tier_cache_try(const SiftTierPolicy *p, SiftTierErr *err)
{
	Opened	   *o = opened_for(p);
	char		root[MAXPGPATH],
				dir[MAXPGPATH];
	MemoryContext old;

	if (o->cache == NULL)
	{
		snprintf(root, sizeof(root), "%s/pg_sift_tier", DataDir);
		snprintf(dir, sizeof(dir), "%s/%u-%u", root, MyDatabaseId, p->policy_id);
		make_dir(root);
		make_dir(dir);
		old = MemoryContextSwitchTo(policy_context());
		o->cache = sift_tier_cache_open(dir, (uint64) p->cache_mb << 20, SIFT_TIER_CACHE_CHUNK, err);
		MemoryContextSwitchTo(old);
	}
	return o->cache;
}

SiftTierCache *
sift_tier_cache(const SiftTierPolicy *p)
{
	SiftTierErr err = {0};
	SiftTierCache *c = sift_tier_cache_try(p, &err);

	if (c == NULL)
		sift_tier_raise(err.code, &err, "cannot open the local cache of the tier policy");
	return c;
}

void
sift_tier_raise(SiftTierStatus st, const SiftTierErr *err, const char *what)
{
	int			code = st == SIFT_TIER_E_NO_CURL ? ERRCODE_FEATURE_NOT_SUPPORTED :
		st == SIFT_TIER_E_NO_CRED ? ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION :
		(st == SIFT_TIER_E_ARG || st == SIFT_TIER_E_URI || st == SIFT_TIER_E_TOO_LARGE) ? ERRCODE_INVALID_PARAMETER_VALUE :
		(st == SIFT_TIER_E_CORRUPT || st == SIFT_TIER_E_MISMATCH) ? ERRCODE_DATA_CORRUPTED :
		st == SIFT_TIER_E_NOT_FOUND ? ERRCODE_UNDEFINED_OBJECT : ERRCODE_IO_ERROR;

	ereport(ERROR, (errcode(code), errmsg("sift: %s: %s", what, err->msg)));
}

typedef struct CachedRefs
{
	RelFileLocator locator;
	uint64		gen;
	int			n;
	SiftTierPartRef *refs;
} CachedRefs;

static CachedRefs cached;

const SiftTierPartRef *
sift_tier_refs(Relation rel, int *n)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	SiftTierPartRef *refs;

	*n = (int) meta.ntier_parts;
	if (meta.ntier_parts == 0)
		return NULL;
	if (cached.refs != NULL && RelFileLocatorEquals(cached.locator, rel->rd_locator) && cached.gen == meta.tier_dir_gen && cached.n == *n)
		return cached.refs;
	refs = sift_manifest_read_tier(rel, &meta, n);
	if (cached.refs != NULL)
	{
		pfree(cached.refs);
		cached.refs = NULL;
	}
	cached = (CachedRefs) {.locator = rel->rd_locator,.gen = meta.tier_dir_gen,.n = *n,.refs = MemoryContextAlloc(policy_context(), *n * sizeof(SiftTierPartRef))};
	memcpy(cached.refs, refs, *n * sizeof(SiftTierPartRef));
	pfree(refs);
	return cached.refs;
}

int
sift_tier_time_slot(Relation rel, const SiftF2Store *store, const char *col)
{
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		int			leaf = store->spec.time_cols[s] - 1;
		int			attno = leaf >= 0 ? store->leaf_attno[leaf] : 0;

		if (attno > 0 && strcmp(NameStr(TupleDescAttr(RelationGetDescr(rel), attno - 1)->attname), col) == 0)
			return s;
	}
	return -1;
}

int64
sift_tier_cutoff(const SiftF2Store *store, int slot, TimestampTz now, int64 span_us)
{
	int64		t = now - span_us,
				unit = store->time_unit[slot];

	return t / unit + (t % unit > 0 ? 1 : 0);
}

/* ---- the writes of sift.set_tier_policy, sift.clear_tier_policy and sift.set_ttl ---- */

static void
write_as_owner(const char *sql, int nargs, const Oid *types, const Datum *values, const char *nulls, int expect)
{
	CatalogUser save;
	int			rc;

	catalog_user_enter(&save);
	SPI_connect();
	rc = SPI_execute_with_args(sql, nargs, (Oid *) types, (Datum *) values, nulls, false, 0);
	SIFT_INVARIANT(rc == expect, "sift: could not write the policy of a relation");
	SPI_finish();
	catalog_user_leave(&save);
}

void
sift_tier_policy_put(Relation rel, const char *uri, Interval *move_after, const char *endpoint, uint64 scope_id)
{
	Oid			types[] = {OIDOID, TEXTOID, INTERVALOID, TEXTOID, INT8OID};
	Datum		values[] = {ObjectIdGetDatum(RelationGetRelid(rel)), CStringGetTextDatum(uri), IntervalPGetDatum(move_after),
	endpoint != NULL ? CStringGetTextDatum(endpoint) : (Datum) 0, Int64GetDatum((int64) scope_id)};

	write_as_owner("INSERT INTO sift.tier_policies (relid, uri, move_after, endpoint, scope_id) VALUES ($1, $2, $3, $4, $5)", 5, types, values,
				   endpoint != NULL ? "     " : "   n ", SPI_OK_INSERT);
}

void
sift_tier_policy_set_move_after(Relation rel, Interval *move_after)
{
	Oid			types[] = {OIDOID, INTERVALOID};
	Datum		values[] = {ObjectIdGetDatum(RelationGetRelid(rel)), IntervalPGetDatum(move_after)};

	write_as_owner("UPDATE sift.tier_policies SET move_after = $2 WHERE relid::oid = $1", 2, types, values, NULL, SPI_OK_UPDATE);
}

void
sift_tier_policy_delete(Relation rel)
{
	Oid			types[] = {OIDOID};
	Datum		values[] = {ObjectIdGetDatum(RelationGetRelid(rel))};

	write_as_owner("DELETE FROM sift.tier_policies WHERE relid::oid = $1", 1, types, values, NULL, SPI_OK_DELETE);
}

void
sift_ttl_policy_put(Relation rel, const char *col, Interval *retention)
{
	Oid			types[] = {OIDOID, TEXTOID, INTERVALOID};
	Datum		values[] = {ObjectIdGetDatum(RelationGetRelid(rel)), CStringGetTextDatum(col), IntervalPGetDatum(retention)};

	write_as_owner("INSERT INTO sift.ttl_policies (relid, col, retention) VALUES ($1, $2, $3) ON CONFLICT (relid) DO UPDATE SET col = $2, retention = $3", 3, types, values, NULL,
				   SPI_OK_INSERT);
}

void
sift_ttl_policy_delete(Relation rel)
{
	Oid			types[] = {OIDOID};
	Datum		values[] = {ObjectIdGetDatum(RelationGetRelid(rel))};

	write_as_owner("DELETE FROM sift.ttl_policies WHERE relid::oid = $1", 1, types, values, NULL, SPI_OK_DELETE);
}

SiftTierPolicyRow *
sift_tier_policy_all(int *n)
{
	MemoryContext cx = CurrentMemoryContext;
	SiftTierPolicyRow *rows;
	CatalogUser save;
	int			rc;

	catalog_user_enter(&save);
	SPI_connect();
	rc = SPI_execute("SELECT relid::oid, uri, scope_id FROM sift.tier_policies ORDER BY policy_id", true, 0);
	SIFT_INVARIANT(rc == SPI_OK_SELECT, "sift: could not read sift.tier_policies");
	*n = (int) SPI_processed;
	rows = MemoryContextAlloc(cx, Max(*n, 1) * sizeof(SiftTierPolicyRow));
	for (int i = 0; i < *n; i++)
	{
		bool		isnull;
		HeapTuple	t = SPI_tuptable->vals[i];

		rows[i].relid = DatumGetObjectId(SPI_getbinval(t, SPI_tuptable->tupdesc, 1, &isnull));
		rows[i].uri = MemoryContextStrdup(cx, TextDatumGetCString(SPI_getbinval(t, SPI_tuptable->tupdesc, 2, &isnull)));
		rows[i].scope_id = (uint64) DatumGetInt64(SPI_getbinval(t, SPI_tuptable->tupdesc, 3, &isnull));
	}
	SPI_finish();
	catalog_user_leave(&save);
	return rows;
}
