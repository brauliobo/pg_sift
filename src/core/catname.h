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

static inline Relation
sift_relation_get(Oid relid)
{
	Relation	rel = RelationIdGetRelation(relid);

	if (!RelationIsValid(rel))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation with OID %u does not exist", relid)));
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
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation with OID %u does not exist", relid)));
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
