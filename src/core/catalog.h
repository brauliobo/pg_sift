/*
 * core/catalog.h - what a hook asks of the catalog before it touches a relation: the planner, executor, utility and object access hooks run for every statement of a backend that loaded
 * the library, so a statement on a relation that is none of ours must cost them a comparison of OIDs the catalog cache already holds, and no relation open, lock or catalog lookup.
 */
#ifndef SIFT_CORE_CATALOG_H
#define SIFT_CORE_CATALOG_H

#include "utils/rel.h"
#include "utils/relcache.h"

#include "core/sift.h"

#ifdef SIFT_TESTING
extern uint64 sift_foreign_probes;
#define SIFT_NOTE_FOREIGN_PROBE() (sift_foreign_probes++)
#else
#define SIFT_NOTE_FOREIGN_PROBE() ((void) 0)
#endif

/* Is the extension installed in this database: a hook of a database without it has nothing to do */
static inline bool
sift_db_has_extension(void)
{
	const SiftCatalog *cat = sift_catalog();

	return cat->am != InvalidOid || cat->col_am != InvalidOid || cat->tbl_am != InvalidOid;
}

/*
 * The access method of a relation, InvalidOid for none, from its relcache entry: no lock and no catalog read, and the entry of a relation that the running command made (an index it builds)
 * is there before its catalog row is visible to a syscache.
 */
static inline Oid
sift_relid_relam(Oid relid)
{
	Relation	rel = RelationIdGetRelation(relid);
	Oid			relam;

	if (!RelationIsValid(rel))
		return InvalidOid;
	relam = rel->rd_rel->relam;
	RelationClose(rel);
	return relam;
}

/* Is the relation a table of the sift_table access method */
static inline bool
sift_relid_is_sift_table(Oid relid)
{
	const Oid	tbl_am = sift_catalog()->tbl_am;

	return tbl_am != InvalidOid && sift_relid_relam(relid) == tbl_am;
}

#endif							/* SIFT_CORE_CATALOG_H */
