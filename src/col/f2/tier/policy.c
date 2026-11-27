/*
 * col/f2/tier/policy.c - the policies of a relation (sift.tier_policies, sift.ttl_policies), read as the owner of those tables so that a reader of a tiered table needs no privilege on them, and
 * what a session keeps of them: the backend and the cache of each policy, and the tier directory of the last relation it read.
 *
 * A policy id never changes what it names: the location, the endpoint and the namespace of the objects are fixed for it, and sift.set_tier_policy gives a relation that changes them a new id. A
 * backend a session opened for an id is therefore right for as long as there is a ref that names the id.
 */
#include "postgres.h"

#include <sys/stat.h>

#include "catalog/namespace.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "col/f2/meta.h"
#include "col/f2/tier/host.h"
#include "core/cachebudget.h"
#include "core/owner_sql.h"
#include "core/page_io.h"

#define POLICY_SELECT \
	"SELECT policy_id, uri, endpoint, (extract(epoch FROM move_after) * 1000000)::int8, cache_mb, scope_id, db_oid FROM sift.tier_policies WHERE "

static uint32 policies_relid_hash;	/* the hash of sift.tier_policies in the cache of pg_class */

/* The SQL of an extension that is not yet updated to 6.0 has no policy tables: no relation of it has a policy, and only the functions of 6.0 write one */
static void
owner_execute(const SiftOwnerSql *q)
{
	Oid			relid = get_relname_relid("tier_policies", get_namespace_oid("sift", false));

	if (!OidIsValid(relid))
	{
		SIFT_INVARIANT(q->read_only, "sift: a policy write without sift.tier_policies");
		return;
	}
	policies_relid_hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(relid));
	sift_owner_sql(relid, q);
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

typedef struct PolicyRead
{
	MemoryContext cx;
	SiftTierPolicy *out;
	bool		found;
} PolicyRead;

static void
read_policy(void *arg)
{
	PolicyRead *r = arg;
	bool		isnull;

	r->found = SPI_processed == 1;
	if (r->found)
		*r->out = (SiftTierPolicy) {
			.policy_id = (uint32) DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull)),
			.uri = text_col(r->cx, 2, &isnull),
			.endpoint = text_col(r->cx, 3, &isnull),
			.move_after_us = int_col(4),
			.cache_mb = (uint32) DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 5, &isnull)),
			.scope_id = (uint64) int_col(6),
			.db_oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 7, &isnull))
		};
}

static bool
policy_row(const char *where, Oid argtype, Datum arg, SiftTierPolicy *out)
{
	PolicyRead	r = {CurrentMemoryContext, out, false};
	SiftOwnerSql q = {.sql = psprintf("%s%s", POLICY_SELECT, where),.nargs = 1,.types = &argtype,.values = &arg,.read_only = true,.limit = 1,.expect = SPI_OK_SELECT,
	.rows = read_policy,.out = &r};

	owner_execute(&q);
	pfree((char *) q.sql);
	return r.found;
}

bool
sift_tier_policy_of(Relation rel, SiftTierPolicy *out)
{
	return policy_row("relid::oid = $1", OIDOID, ObjectIdGetDatum(RelationGetRelid(rel)), out);
}

typedef struct TtlRead
{
	MemoryContext cx;
	SiftTtlPolicy *out;
	bool		found;
} TtlRead;

static void
read_ttl(void *arg)
{
	TtlRead    *r = arg;
	bool		isnull;

	r->found = SPI_processed == 1;
	if (r->found)
		*r->out = (SiftTtlPolicy) {.col = text_col(r->cx, 1, &isnull),.retention_us = int_col(2)};
}

bool
sift_ttl_policy_of(Relation rel, SiftTtlPolicy *out)
{
	Datum		arg = ObjectIdGetDatum(RelationGetRelid(rel));
	Oid			argtype = OIDOID;
	TtlRead		r = {CurrentMemoryContext, out, false};
	SiftOwnerSql q = {.sql = "SELECT col, (extract(epoch FROM retention) * 1000000)::int8 FROM sift.ttl_policies WHERE relid::oid = $1",.nargs = 1,.types = &argtype,.values = &arg,
	.read_only = true,.limit = 1,.expect = SPI_OK_SELECT,.rows = read_ttl,.out = &r};

	owner_execute(&q);
	return r.found;
}

/* ---- what a session keeps ---- */

typedef struct Opened
{
	uint32		policy_id;
	bool		loaded;
	bool		retired;		/* the id names another location now: what it opened is closed once no statement of the transaction may read through it */
	SiftTierPolicy policy;
	SiftTierBackend *backend;
	SiftTierCache *cache;
	MemoryContext cx;			/* the policy strings, the backend and the cache: dropping the policy gives them back */
	uint64		used;			/* sift_cache_tick() of the last ask */
} Opened;

static Opened *opened;
static int	nopened,
			capopened;
static MemoryContext policy_cx;
static bool policies_stale;

typedef struct CachedRefs
{
	RelFileLocator locator;
	uint64		gen;
	int			n;
	SiftTierPartRef *refs;
} CachedRefs;

static CachedRefs cached;

static void
refs_drop(void)
{
	if (cached.refs != NULL)
		pfree(cached.refs);
	cached = (CachedRefs) {0};
}

void
sift_tier_refs_forget(const RelFileLocator *locator)
{
	if (RelFileLocatorEquals(cached.locator, *locator))
		refs_drop();
}

static void
opened_close(Opened *o)
{
	if (o->cache != NULL)
		sift_tier_cache_close(o->cache);
	if (o->backend != NULL)
		sift_tier_close(o->backend);
	if (o->cx != NULL)
		MemoryContextDelete(o->cx);
}

/* A statement of the transaction may read through what a policy opened: the budget drops only a policy no statement of it asked for, the one asked for longest ago */
static Opened *
policies_victim(void)
{
	Opened	   *lru = NULL;

	for (int i = 0; i < nopened; i++)
		if (!sift_cache_used_in_xact(opened[i].used) && (lru == NULL || opened[i].used < lru->used))
			lru = &opened[i];
	return lru;
}

static uint64
policies_oldest(void)
{
	Opened	   *victim = policies_victim();

	return victim != NULL ? victim->used : UINT64_MAX;
}

static bool
policies_evict_one(void)
{
	Opened	   *victim = policies_victim();

	if (victim == NULL)
		return false;
	opened_close(victim);
	*victim = opened[--nopened];
	return true;
}

static void
policies_evict_all(void)
{
	for (int i = 0; i < nopened; i++)
		opened_close(&opened[i]);
	opened = NULL;
	nopened = capopened = 0;
	cached = (CachedRefs) {0};
	if (policy_cx != NULL)
		MemoryContextReset(policy_cx);
}

static const SiftCacheKind policies_kind = {"tier_policies", policies_oldest, policies_evict_one, policies_evict_all};

/* A change to sift.tier_policies, or its drop with the extension, may have given the ids a session opened to other locations */
static void
policies_invalidated(Datum arg, int cacheid, uint32 hashvalue)
{
	(void) arg;
	(void) cacheid;
	if (hashvalue == 0 || hashvalue == policies_relid_hash)
		policies_stale = true;
}

static MemoryContext
policy_context(void)
{
	if (policy_cx == NULL)
	{
		policy_cx = sift_cache_create_context("sift cache tier policies");
		sift_cache_register(&policies_kind);
		CacheRegisterSyscacheCallback(RELOID, policies_invalidated, (Datum) 0);
	}
	return policy_cx;
}

static bool
same_location(const SiftTierPolicy *a, const SiftTierPolicy *b)
{
	return a->scope_id == b->scope_id && strcmp(a->uri, b->uri) == 0 && (a->endpoint == NULL ? b->endpoint == NULL : b->endpoint != NULL && strcmp(a->endpoint, b->endpoint) == 0);
}

/*
 * The catalog says again what each id a session opened names. An id that names another location now (DROP EXTENSION and CREATE EXTENSION start the ids again at 1) is another policy: what was
 * opened for the old one is closed once no statement of the transaction may read through it, and the id is opened afresh. A slot whose policy was never read has nothing to check.
 */
static void
revalidate(void)
{
	policies_stale = false;
	for (int i = 0; i < nopened; i++)
	{
		SiftTierPolicy now;

		if (opened[i].loaded && !(policy_row("policy_id = $1", INT4OID, Int32GetDatum((int32) opened[i].policy_id), &now) && same_location(&now, &opened[i].policy)))
			opened[i].retired = true;
	}
}

/* The slots of ids that name another location, closed when the transaction no longer reads through them */
static void
close_retired(void)
{
	for (int i = nopened - 1; i >= 0; i--)
		if (opened[i].retired && !sift_cache_used_in_xact(opened[i].used))
		{
			opened_close(&opened[i]);
			opened[i] = opened[--nopened];
		}
}

static Opened *
opened_for(uint32 policy_id)
{
	Opened	   *o = NULL;

	if (policies_stale)
		revalidate();
	close_retired();
	for (int i = 0; i < nopened && o == NULL; i++)
		if (opened[i].policy_id == policy_id && !opened[i].retired)
			o = &opened[i];
	if (o == NULL)
	{
		if (nopened == capopened)
		{
			capopened = Max(8, capopened * 2);
			opened = opened == NULL ? MemoryContextAlloc(policy_context(), capopened * sizeof(Opened)) : repalloc(opened, capopened * sizeof(Opened));
		}
		o = &opened[nopened++];
		*o = (Opened) {.policy_id = policy_id,.cx = sift_cache_create_context_in(policy_context(), "sift cache tier policy")};
	}
	o->used = sift_cache_tick();
	return o;
}

static void
keep_policy(Opened *o, const SiftTierPolicy *p)
{
	MemoryContext cx = o->cx;

	o->policy = (SiftTierPolicy) {.policy_id = p->policy_id,.uri = MemoryContextStrdup(cx, p->uri),.endpoint = p->endpoint != NULL ? MemoryContextStrdup(cx, p->endpoint) : NULL,
	.move_after_us = p->move_after_us,.cache_mb = p->cache_mb,.scope_id = p->scope_id,.db_oid = p->db_oid};
	o->loaded = true;
}

/* The policy a ref names, read once per session: what an id names never changes while the table of the policies stands */
void
sift_tier_policy_by_id(Relation rel, uint32 policy_id, SiftTierPolicy *out)
{
	Opened	   *o = opened_for(policy_id);

	if (!o->loaded)
	{
		MemoryContext old = MemoryContextSwitchTo(o->cx);
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
	Opened	   *o = opened_for(p->policy_id);
	MemoryContext old;

	if (!o->loaded)
		keep_policy(o, p);
	if (o->backend == NULL)
	{
		old = MemoryContextSwitchTo(o->cx);
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

/* The cache is ephemeral: in the directory of the temporary files of the server, whose contents a base backup leaves out and a restart removes by this name */
#define TIER_CACHE_PREFIX	"pgsql_tmp_sift_tier."

static void
make_dir(const char *path)
{
	if (MakePGDirectory(path) != 0 && errno != EEXIST)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not create directory \"%s\": %m", path)));
}

SiftTierCache *
sift_tier_cache_try(const SiftTierPolicy *p, SiftTierErr *err)
{
	Opened	   *o = opened_for(p->policy_id);
	char		root[MAXPGPATH],
				dir[MAXPGPATH];
	MemoryContext old;

	if (!o->loaded)
		keep_policy(o, p);
	if (o->cache == NULL)
	{
		snprintf(root, sizeof(root), "%s/base/pgsql_tmp", DataDir);
		snprintf(dir, sizeof(dir), "%s/%s%u-%u", root, TIER_CACHE_PREFIX, MyDatabaseId, p->policy_id);
		make_dir(root);
		make_dir(dir);
		old = MemoryContextSwitchTo(o->cx);
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
	refs_drop();
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
	SiftOwnerSql q = {.sql = sql,.nargs = nargs,.types = types,.values = values,.nulls = nulls,.expect = expect};

	owner_execute(&q);
}

void
sift_tier_policy_put(Relation rel, const char *uri, Interval *move_after, const char *endpoint, uint64 scope_id)
{
	Oid			types[] = {OIDOID, TEXTOID, INTERVALOID, TEXTOID, INT8OID, OIDOID};
	Datum		values[] = {ObjectIdGetDatum(RelationGetRelid(rel)), CStringGetTextDatum(uri), IntervalPGetDatum(move_after),
	endpoint != NULL ? CStringGetTextDatum(endpoint) : (Datum) 0, Int64GetDatum((int64) scope_id), ObjectIdGetDatum(MyDatabaseId)};

	write_as_owner("INSERT INTO sift.tier_policies (relid, uri, move_after, endpoint, scope_id, db_oid) VALUES ($1, $2, $3, $4, $5, $6)", 6, types, values,
				   endpoint != NULL ? "      " : "   n  ", SPI_OK_INSERT);
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

typedef struct AllRead
{
	MemoryContext cx;
	SiftTierPolicyRow *rows;
	int			n;
} AllRead;

static void
read_all(void *arg)
{
	AllRead    *r = arg;

	r->n = (int) SPI_processed;
	r->rows = MemoryContextAlloc(r->cx, Max(r->n, 1) * sizeof(SiftTierPolicyRow));
	for (int i = 0; i < r->n; i++)
	{
		bool		isnull;
		HeapTuple	t = SPI_tuptable->vals[i];

		r->rows[i].relid = DatumGetObjectId(SPI_getbinval(t, SPI_tuptable->tupdesc, 1, &isnull));
		r->rows[i].uri = MemoryContextStrdup(r->cx, TextDatumGetCString(SPI_getbinval(t, SPI_tuptable->tupdesc, 2, &isnull)));
		r->rows[i].scope_id = (uint64) DatumGetInt64(SPI_getbinval(t, SPI_tuptable->tupdesc, 3, &isnull));
	}
}

SiftTierPolicyRow *
sift_tier_policy_all(int *n)
{
	AllRead		r = {CurrentMemoryContext, NULL, 0};
	SiftOwnerSql q = {.sql = "SELECT relid::oid, uri, scope_id FROM sift.tier_policies ORDER BY policy_id",.read_only = true,.expect = SPI_OK_SELECT,.rows = read_all,.out = &r};

	owner_execute(&q);
	*n = r.n;
	return r.rows;
}
