/*
 * core/owner_sql.h - one statement of SPI as the owner of a relation: on a catalog table of the extension, the caller needs no privilege on it and what it may do with a row is decided
 * by the function that runs the statement, not by a grant; on a table of a role (the pairs of a followed table), the code of that role runs with its rights and never with those of the
 * caller. The statement runs as a security-restricted operation with search_path pg_catalog, pg_temp, so no operator, function or type of the caller's path runs with the owner's rights
 * and no setting, temporary object or cursor of the owner outlives the statement in the session of the caller.
 */
#ifndef SIFT_CORE_OWNER_SQL_H
#define SIFT_CORE_OWNER_SQL_H

typedef struct SiftOwnerSql
{
	const char *sql;
	int			nargs;
	const Oid  *types;
	const Datum *values;
	const char *nulls;
	bool		read_only;
	long		limit;
	int			expect;			/* the SPI_OK_ code of the statement */
	void		(*rows) (void *out);	/* reads SPI_tuptable while the connection is open */
	void	   *out;
} SiftOwnerSql;

/* Runs q as the owner of the relation rel: the user, the search_path, the snapshot and the SPI connection of the caller are back whatever the statement does */
extern void sift_owner_sql(Oid rel, const SiftOwnerSql *q);

#endif
