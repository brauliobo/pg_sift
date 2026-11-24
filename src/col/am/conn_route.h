/*
 * col/am/conn_route.h - the connections of a sift_col index by the format of the index: the key journal of a parent index and the checks that keep a child and its parents in one format.
 * A format that keeps a journal of its own gives its table (SiftColFormatOps.journal); format major 1 has none, and its journal is the one of col/v1/conn.h.
 *
 * The protocol is the one of col/v1/conn.h: a derived value for a parent key is read as stored for a snapshot when the journal vouches for its stamp and the key is not in changed(S, X).
 */
#ifndef SIFT_COL_CONN_ROUTE_H
#define SIFT_COL_CONN_ROUTE_H

#include "col/v1/conn.h"

typedef struct SiftColJournalOps
{
	bool		(*on) (Relation index);
	SiftConnWhy (*take) (Relation heap, Relation index, Snapshot snap, SiftConnStamp *stamp);
	SiftConnWhy (*vouches) (Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap);
	uint64		(*inserted_rows) (Relation heap, Relation index, const SiftConnStamp *stamp);
	SiftConnWhy (*changed) (Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out);
	bool		(*state) (Relation index, uint64 *lineage, uint64 *records);
	void		(*alter) (Relation heap, Relation index);
} SiftColJournalOps;

extern bool sift_jrnl_on(Relation index);
extern SiftConnWhy sift_jrnl_take(Relation heap, Relation index, Snapshot snap, SiftConnStamp *stamp);
extern SiftConnWhy sift_jrnl_vouches(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap);
extern uint64 sift_jrnl_inserted_rows(Relation heap, Relation index, const SiftConnStamp *stamp);
extern SiftConnWhy sift_jrnl_changed(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out);
/* The lineage of the journal of the index and the records appended to it (false: it keeps none) */
extern bool sift_jrnl_state(Relation index, uint64 *lineage, uint64 *records);

/*
 * The sift_col index of the parent table that serves a connection (the lowest oid of those that track its key), the parent table left open in *parent, locked AccessShare. An index of a format
 * that keeps a journal and has not chosen its key (track_changes auto) serves the primary key of its table. ERROR with a hint for what is wrong with the connection.
 */
extern Oid	sift_conn_route_parent(Relation heap, Relation index, const SiftConnSpec *s, Relation *parent);

/* Does a connection keep the index in format major 1: a child index connected to it is of format major 1 (it reads the journal of its parent in format major 1), or a parent it names is */
extern bool sift_conn_route_format1(Relation heap, Relation index);
/* ERROR unless every parent index the connections of the index name is of the format major of the index */
extern void sift_conn_route_check_parents(Relation heap, Relation index, uint16 major);
/* ALTER INDEX of a sift_col index: the connections follow the option, in the format of the index */
extern void sift_conn_route_alter(Relation index);

/* The SQL functions of the connections, which read the journal (and the found flags) of the format of the index they are given */
extern Datum sift_col_connection_fill(PG_FUNCTION_ARGS);

#endif							/* SIFT_COL_CONN_ROUTE_H */
