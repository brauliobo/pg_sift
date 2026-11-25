/*
 * col/f2/f2_tier_sql.c - sift.set_tier_policy, sift.clear_tier_policy, sift.set_ttl and sift.tier_status.
 *
 * The functions run as the caller and need MAINTAIN on the table (tier_status: SELECT); setting and clearing a tier policy needs a superuser too, as the location is a resource of the server. The policy tables are written as their owner, so a caller needs no grant on them. The location of a policy
 * holds no user, password or query, and the credentials of an S3 or R2 store are read from the environment of the server when a connection is made: nothing of them is stored or shown.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/f2/col_meta.h"
#include "col/f2/f2_tier_host.h"
#include "col/pg.h"
#include "core/catname.h"
#include "core/meta_peek.h"
#include "core/sift.h"
#include "core/xlog.h"
#include "table/dispatch.h"

PG_FUNCTION_INFO_V1(sift_set_tier_policy);
PG_FUNCTION_INFO_V1(sift_clear_tier_policy);
PG_FUNCTION_INFO_V1(sift_set_ttl);
PG_FUNCTION_INFO_V1(sift_tier_status);

static bool
is_col_index(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX && sift_col_is_index(rel);
}

static bool
is_f2(Relation rel)
{
	return (is_col_index(rel) || sift_tbl_is_table(rel)) && sift_meta_resolve_major(rel) == SIFT_FORMAT2_MAJOR;
}

/* The table whose privileges decide what the caller may do with the relation: an index has none of its own */
static Oid
acl_relid(Relation rel)
{
	return is_col_index(rel) ? rel->rd_index->indrelid : RelationGetRelid(rel);
}

/* A Format 2.0 sift_table or sift_col index: both keep their parts, tier directory and metapage the same way, so the policies and the maintenance step are the same for both */
static Relation
open_f2_rel(Oid relid)
{
	Relation	rel = relation_open(sift_rel_must_exist(relid), ShareUpdateExclusiveLock);

	if (!is_col_index(rel) && !sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table or a sift_col index", RelationGetRelationName(rel))));
	if (sift_meta_resolve_major(rel) != SIFT_FORMAT2_MAJOR)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is a Format 1 relation: object storage tiering and TTL need Format 2.0", RelationGetRelationName(rel)),
						is_col_index(rel) ? 0 : errhint("Run sift.upgrade_table(\"%s\").", RelationGetRelationName(rel))));
	if (pg_class_aclcheck(acl_relid(rel), GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
	return rel;
}

/* A location is a directory of the server host or an object store reached with the credentials of the server environment: the resources of its superusers, as a tablespace is */
static void
require_server_resource_owner(const char *what)
{
	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to %s", what),
						errdetail("A tier policy names a directory of the server or an object store that the server reaches with its own credentials.")));
}

static void
require_arg(FunctionCallInfo fcinfo, int i, const char *name)
{
	if (PG_ARGISNULL(i))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("%s must not be null", name)));
}

static int64
interval_us(const Interval *iv)
{
	return iv->time + (int64) iv->day * USECS_PER_DAY + (int64) iv->month * 30 * USECS_PER_DAY;
}

static void
require_positive(const Interval *iv, const char *name)
{
	if (interval_us(iv) <= 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s must be longer than zero", name)));
}

static void
require_no_remote_parts(Relation rel, const char *what)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);

	if (meta.ntier_parts > 0)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("\"%s\" has %u parts in object storage: %s", RelationGetRelationName(rel), meta.ntier_parts, what),
						errhint("A part leaves object storage when it expires (sift.set_ttl on a table) or when the relation is rewritten; sift.tier_status lists them.")));
}

/* The location of a policy is left to nothing the standbys may still read: a sweep that finds objects of the relation that wait for the end of their grace refuses */
static void
require_store_reclaimed(Relation rel, const SiftTierPolicy *old)
{
	int			left = sift_f2_tier_reclaim(rel, old);

	if (left < 0)
		ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE), errmsg("the maintenance of \"%s\" is running", RelationGetRelationName(rel)), errhint("Try again in a moment.")));
	if (left > 0)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("the object store of \"%s\" holds %d objects of parts that left the relation", RelationGetRelationName(rel), left),
						errhint("A standby or a recovery may still read them; they are deleted once the grace of the standbys is over: try again then.")));
}

/* A location this build can use now: the scheme, the characters and, for an S3 or R2 store, libcurl and the credentials */
static void
check_location(const char *uri, const char *endpoint)
{
	SiftTierErr err = {0};
	SiftTierBackend *be;

	if (sift_tier_uri_check(uri, &err) != SIFT_TIER_OK)
		sift_tier_raise(err.code, &err, "the location of the tier policy is not valid");
	be = sift_tier_open(&(SiftTierOpen) {.uri = uri,.endpoint = endpoint}, &err);
	if (be == NULL)
		sift_tier_raise(err.code, &err, "the object store of the tier policy cannot be used");
	sift_tier_close(be);
}

static uint64
new_scope_id(void)
{
	uint64		id;

	do
	{
		if (!pg_strong_random(&id, sizeof(id)))
			ereport(ERROR, (errcode(ERRCODE_SYSTEM_ERROR), errmsg("could not generate a random namespace for the objects")));
		id &= PG_INT64_MAX;
	} while (id == 0);
	return id;
}

Datum
sift_set_tier_policy(PG_FUNCTION_ARGS)
{
	Relation	rel;
	SiftF2Store *store;
	SiftTierPolicy old;
	char	   *uri,
			   *endpoint;
	Interval   *move_after;
	bool		changed = true;

	require_arg(fcinfo, 0, "rel");
	require_arg(fcinfo, 1, "uri");
	require_arg(fcinfo, 2, "move_after");
	rel = open_f2_rel(PG_GETARG_OID(0));
	require_server_resource_owner("set a tier policy");
	uri = text_to_cstring(PG_GETARG_TEXT_PP(1));
	move_after = PG_GETARG_INTERVAL_P(2);
	endpoint = PG_ARGISNULL(3) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(3));
	require_positive(move_after, "move_after");
	store = sift_f2_store_open(rel);
	if (store->spec.time_cols[0] == 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" has no date or timestamp column to age its parts by", RelationGetRelationName(rel)),
				 errhint("Tiering moves a part by the age of a date or timestamp column of the index: add one.")));
	check_location(uri, endpoint);
	if (!sift_tier_policy_of(rel, &old))
		sift_tier_policy_put(rel, uri, move_after, endpoint, new_scope_id());
	else if (strcmp(old.uri, uri) == 0 && (old.endpoint == NULL ? endpoint == NULL : endpoint != NULL && strcmp(old.endpoint, endpoint) == 0))
	{
		changed = old.move_after_us != interval_us(move_after);
		if (changed)
			sift_tier_policy_set_move_after(rel, move_after);
	}
	else
	{
		require_no_remote_parts(rel, "the location of the policy cannot change until they are gone");
		require_store_reclaimed(rel, &old);
		sift_tier_policy_delete(rel);
		sift_tier_policy_put(rel, uri, move_after, endpoint, new_scope_id());
	}
	relation_close(rel, NoLock);
	PG_RETURN_BOOL(changed);
}

Datum
sift_clear_tier_policy(PG_FUNCTION_ARGS)
{
	Relation	rel = open_f2_rel(PG_GETARG_OID(0));
	SiftTierPolicy old;
	bool		had;

	require_server_resource_owner("clear a tier policy");
	had = sift_tier_policy_of(rel, &old);
	if (had)
	{
		require_no_remote_parts(rel, "the policy cannot be cleared until they are gone");
		require_store_reclaimed(rel, &old);
		sift_tier_policy_delete(rel);
	}
	relation_close(rel, NoLock);
	PG_RETURN_BOOL(had);
}

/* Block 0 and the registry in shared memory, which the visibility map reads the flags from, agree on the flag */
static void
set_ttl_flag(Relation rel, bool on)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);
	SiftColMetaPageData committed;
	SiftShmIndexMeta shm;

	m->common.ro_compat_flags = on ? (m->common.ro_compat_flags | SIFT_ROCOMPAT_TTL) : (m->common.ro_compat_flags & ~SIFT_ROCOMPAT_TTL);
	sift_meta_seal(p0, m->common.struct_size);
	committed = *m;
	sift_xlog_done(&x);
	shm = sift_col_meta_shm(rel, &committed);
	(void) sift_shm_manifest_publish(&shm, NULL);
}

bool
sift_ttl_rearm_rocompat(Relation rel)
{
	SiftTtlPolicy policy;

	if (!sift_ttl_policy_of(rel, &policy) || (sift_col_meta_read(rel).common.ro_compat_flags & SIFT_ROCOMPAT_TTL) != 0)
		return false;
	set_ttl_flag(rel, true);
	return true;
}

/* The time column the TTL runs on: one of the time columns of the manifest, and NOT NULL, because the time bounds of a part are those of its non-null values and a row without a time would be dropped with its part */
static void
check_ttl_column(Relation rel, const SiftF2Store *store, const char *col)
{
	AttrNumber	attno = get_attnum(RelationGetRelid(rel), col);

	if (attno == InvalidAttrNumber || sift_tier_time_slot(rel, store, col) < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("\"%s\" is not a time column of \"%s\"", col, RelationGetRelationName(rel)),
						errhint("The TTL runs on one of the first date or timestamp columns of the table (those of key_order first).")));
	if (!TupleDescAttr(RelationGetDescr(rel), attno - 1)->attnotnull)
		ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("column \"%s\" of \"%s\" allows NULL", col, RelationGetRelationName(rel)),
						errhint("A row without a time would be dropped with the part it is in: ALTER TABLE ... ALTER COLUMN %s SET NOT NULL.", quote_identifier(col))));
}

Datum
sift_set_ttl(PG_FUNCTION_ARGS)
{
	Relation	rel;
	char	   *col;
	SiftTtlPolicy old;
	bool		had;

	require_arg(fcinfo, 0, "rel");
	require_arg(fcinfo, 1, "col");
	rel = open_f2_rel(PG_GETARG_OID(0));
	if (is_col_index(rel))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is a sift_col index: a TTL would drop index entries of rows its table keeps", RelationGetRelationName(rel)),
						errhint("Set the TTL on the sift_table, or delete the old rows of the table and let the index follow.")));
	col = text_to_cstring(PG_GETARG_TEXT_PP(1));
	had = sift_ttl_policy_of(rel, &old);
	if (PG_ARGISNULL(2))
	{
		if (had)
		{
			sift_ttl_policy_delete(rel);
			sift_f2_ttl_unmark_all(rel);
			set_ttl_flag(rel, false);
		}
	}
	else
	{
		require_positive(PG_GETARG_INTERVAL_P(2), "retention");
		check_ttl_column(rel, sift_f2_store_open(rel), col);
		if (had && strcmp(old.col, col) != 0)
			sift_f2_ttl_unmark_all(rel);
		sift_ttl_policy_put(rel, col, PG_GETARG_INTERVAL_P(2));
		set_ttl_flag(rel, true);
		had = true;
	}
	relation_close(rel, NoLock);
	PG_RETURN_BOOL(had);
}

static void
status_rows(const SiftTierPolicyRow *p, Relation rel, ReturnSetInfo *rsi)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			n;
	SiftTierPartRef *refs = sift_manifest_read_tier(rel, &meta, &n);
	size_t		ulen = strlen(p->uri);

	for (int i = 0; i < n; i++)
	{
		char		key[160];
		Datum		values[7];
		bool		nulls[7] = {false};

		sift_tier_object_key(key, sizeof(key), p->scope_id, refs[i].part_id, refs[i].part_gen);
		values[0] = ObjectIdGetDatum(RelationGetRelid(rel));
		values[1] = Int64GetDatum((int64) refs[i].part_id);
		values[2] = Int64GetDatum((int64) refs[i].part_gen);
		values[3] = CStringGetTextDatum(psprintf("%s%s%s", p->uri, ulen > 0 && p->uri[ulen - 1] == '/' ? "" : "/", key));
		values[4] = Int64GetDatum((int64) refs[i].byte_len);
		values[5] = TimestampTzGetDatum(refs[i].tiered_at);
		values[6] = TimestampTzGetDatum(refs[i].expires_at);
		nulls[6] = refs[i].expires_at == 0;
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
	}
}

Datum
sift_tier_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			n;
	SiftTierPolicyRow *rows;

	InitMaterializedSRF(fcinfo, 0);
	rows = sift_tier_policy_all(&n);
	for (int i = 0; i < n; i++)
	{
		Relation	rel = try_relation_open(rows[i].relid, AccessShareLock);

		if (rel == NULL)
			continue;
		if (pg_class_aclcheck(acl_relid(rel), GetUserId(), ACL_SELECT) == ACLCHECK_OK && is_f2(rel))
			status_rows(&rows[i], rel, rsi);
		relation_close(rel, AccessShareLock);
	}
	return (Datum) 0;
}
