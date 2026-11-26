/*
 * core/owner_sql.h - one statement of SPI on a catalog table of the extension, as the owner of that table: the caller needs no privilege on it, and what it may do with a row is decided by
 * the function that runs the statement, not by a grant. The statement runs as a security-restricted operation with search_path pg_catalog, pg_temp, so no operator, function or type of
 * the caller's path runs with the owner's rights.
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

/* Runs q as the owner of the relation catalog: the user, the search_path, the snapshot and the SPI connection of the caller are back whatever the statement does */
extern void sift_owner_sql(Oid catalog, const SiftOwnerSql *q);

#endif
