/*
 * col/f2/f2_connect.h - connect = 'fk -> parent(key)' on an index of format major 2 (docs/ANALYTICS.md): the index and the parents it names are of format major 2, the child keeps no leaf of its
 * own for the connection, and the journal of each parent (col/f2/f2_journal.h) is what tells a statement that a dimension map it kept for the join is still the parent.
 */
#ifndef SIFT_COL_F2_CONNECT_H
#define SIFT_COL_F2_CONNECT_H

#include "postgres.h"

#include "utils/rel.h"

/* Before the build of the child: the parents the option names are given the key it joins by, and the child depends on them (the parents must be of format major 2) */
extern void sift_f2_connect_resolve(Relation heap, Relation index);

/* After the build: the journal of each parent of the child (made under the lock that holds its writers out), and the journal of this index when a child is connected to it (also in the copy REINDEX CONCURRENTLY builds: the stamps of the old index are another lineage) */
extern void sift_f2_connect_build(Relation heap, Relation index);

/* ALTER INDEX of the child: the connections follow the option */
extern void sift_f2_connect_alter(Relation heap, Relation index);

#endif							/* SIFT_COL_F2_CONNECT_H */
