/*
 * core/catname.h - catalog lookups that must find their object: the lookup of PostgreSQL returns NULL for an object that is gone (dropped by another session between the plan and the
 * read), and the callers read through the result. These raise the error of a missing object.
 */
#ifndef SIFT_CORE_CATNAME_H
#define SIFT_CORE_CATNAME_H

#include "postgres.h"

#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

static inline void
sift_rel_missing(Oid relid)
{
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation with OID %u does not exist", relid)));
}

/* The OID of a relation argument that must exist: a dropped regclass or 0 is a user error (42P01), not a failure of the code that opens it */
static inline Oid
sift_rel_must_exist(Oid relid)
{
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		sift_rel_missing(relid);
	return relid;
}

static inline Relation
sift_relation_get(Oid relid)
{
	Relation	rel = RelationIdGetRelation(relid);

	if (!RelationIsValid(rel))
		sift_rel_missing(relid);
	return rel;
}

static inline char *
sift_func_name(Oid funcid)
{
	char	   *name = get_func_name(funcid);

	if (name == NULL)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("function with OID %u does not exist", funcid)));
	return name;
}

static inline char *
sift_rel_name(Oid relid)
{
	char	   *name = get_rel_name(relid);

	if (name == NULL)
		sift_rel_missing(relid);
	return name;
}

static inline char *
sift_namespace_name(Oid nspid)
{
	char	   *name = get_namespace_name(nspid);

	if (name == NULL)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg("schema with OID %u does not exist", nspid)));
	return name;
}

#endif
