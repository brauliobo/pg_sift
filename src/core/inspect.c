/*
 * core/inspect.c - see inspect.h.
 *
 * sift._may_inspect(table) is the rule of every view and inspector: a role that can read the table, that maintains it (owner or MAINTAIN: a DBA who may
 * VACUUM it may look at its indexes) or that is a member of pg_monitor. sift._require_owner and sift._require_select check the role that called the
 * function, outside any SECURITY DEFINER function (GetOuterUserId): the guard triggers of the catalogs that roles write through the sift functions use them.
 */
#include "postgres.h"

#include "catalog/objectaddress.h"
#include "catalog/pg_authid_d.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "core/catname.h"
#include "core/inspect.h"

PG_FUNCTION_INFO_V1(sift_may_inspect_sql);
PG_FUNCTION_INFO_V1(sift_require_owner);
PG_FUNCTION_INFO_V1(sift_require_select);

bool
sift_may_inspect(Oid heap)
{
	Oid			user = GetUserId();
	bool		missing = false;

	if (has_privs_of_role(user, ROLE_PG_MONITOR))
		return true;
	return OidIsValid(heap) && pg_class_aclcheck_ext(heap, user, ACL_SELECT | ACL_MAINTAIN, &missing) == ACLCHECK_OK && !missing;
}

void
sift_check_inspect(Relation rel)
{
	bool		is_index = rel->rd_rel->relkind == RELKIND_INDEX;
	Oid			heap = is_index ? rel->rd_index->indrelid : RelationGetRelid(rel);

	if (!sift_may_inspect(heap))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for %s \"%s\"", is_index ? "index" : "table", RelationGetRelationName(rel)),
				 errdetail("Only a role that can read table \"%s\", maintain it, or is a member of pg_monitor can do this.", get_rel_name(heap))));
}

/* A partitioned index has no storage: what reads a metapage or a segment is given the index of one of its partitions */
void
sift_refuse_partitioned_index(bool partitioned, const char *name)
{
	if (partitioned)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is a partitioned index, which holds no data", name),
						errhint("Use the index of one of its partitions.")));
}

/*
 * What an inspector was given, before it opens the relation: the access method must be want (NULL: any of the three), and an object of another sift access method is
 * named with the function that serves it. A relation that does not exist is left to the open.
 */
void
sift_inspect_expect(Oid relid, const char *want, const char *use)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Form_pg_class form;
	const char *am;
	char	   *name;
	bool		is_index,
				is_sift,
				partitioned;

	if (!HeapTupleIsValid(tup))
		return;
	form = (Form_pg_class) GETSTRUCT(tup);
	am = OidIsValid(form->relam) ? get_am_name(form->relam) : NULL;
	name = pstrdup(NameStr(form->relname));
	is_index = form->relkind == RELKIND_INDEX;
	partitioned = form->relkind == RELKIND_PARTITIONED_INDEX;
	ReleaseSysCache(tup);
	is_sift = am != NULL && strncmp(am, "sift", 4) == 0;
	if (is_sift && (want == NULL || strcmp(am, want) == 0))
	{
		sift_refuse_partitioned_index(partitioned, name);
		return;
	}
	if (!is_sift)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a %s index", name, want ? want : "sift")));
	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("\"%s\" is a %s %s", name, am, is_index ? "index" : "table"),
			 use ? errhint("Use sift.%s.", use) : 0));
}

/* sift._may_inspect(table regclass): NULL (no such table) is false, but for a member of pg_monitor, who sees the rows that name a dropped table */
Datum
sift_may_inspect_sql(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(sift_may_inspect(PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0)));
}

/* sift._require_owner(table regclass): the role that called the function owns the table (or is a superuser; NULL, a table that is gone: only a superuser) */
Datum
sift_require_owner(PG_FUNCTION_ARGS)
{
	Oid			user = GetOuterUserId();
	Oid			relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);

	if (!OidIsValid(relid))
	{
		if (!superuser_arg(user))
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to change the definition of a table that does not exist")));
	}
	else if (!object_ownercheck(RelationRelationId, sift_rel_must_exist(relid), user))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), get_rel_name(relid));
	PG_RETURN_VOID();
}

/* sift._require_select(rel regclass): the role that called the function may read the relation (a table or a view) */
Datum
sift_require_select(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);

	if (!OidIsValid(relid))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("the relation does not exist")));
	if (pg_class_aclcheck(relid, GetOuterUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)), get_rel_name(relid));
	PG_RETURN_VOID();
}
