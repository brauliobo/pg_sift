/*
 * col/conn.h - connected indexes: the connection of an index on a child table to the sift_col index on the parent table it joins, and the protocol that lets a
 * value derived from the parent (a carried column of the child's segments, a shared map of the parent's attributes) be read as exact.
 *
 * The rule (docs/COL-CONNECT.md). A derived value for a parent key k is computed under a snapshot R of the parent index. X = R.xmin: every transaction below X
 * was finished when R was taken, so it is decided the same way for R and for every snapshot S with S.xmin >= X. The parent's KEY JOURNAL lists the keys whose
 * entry was ended (updated away, deleted, re-keyed) by a transaction, with its xid. The value of k is read as stored for a snapshot S when
 *     S is an MVCC snapshot that is not SERIALIZABLE, S.xmin >= X,
 *     the journal vouches for X (its lineage is the stamp's, no record at or after X was dropped, the parent's tombstone log is complete for S), and
 *     k is not in changed(S, X): the keys of the journal records of transactions >= X (in doubt, committed or S's own; aborted ones are void).
 * A key in changed(S, X), a key the derived structure has no value for (a parent that was absent when it was derived: an orphan, never "absent for good"),
 * and every key when the answer is not VOUCHED, is read from the parent row S sees, by the lookup path. NULL join keys are absent for good (they join nothing).
 * The invariant is completeness: changed(S, X) must contain every key whose visible entry can differ between R and S. An incomplete set gives a wrong answer
 * that no fallback sees, so every doubt (a trimmed journal, a new lineage, a log that lost its triggers, a standby that cannot tell, a cap) is a verdict other
 * than OK and the caller reads the join path, never a guess.
 *
 * The parent key is one int2, int4 or int8 column with a unique index and track_changes on it (the tombstone log of the parent index is complete); the journal
 * lives in the parent's tombstone segment as records of kind SIFT_TOMB_KEYCHG, so it is appended, held back, voided on abort, read and rewritten by the same
 * code as the tombstones.
 */
#ifndef SIFT_COL_CONN_H
#define SIFT_COL_CONN_H

#include "access/transam.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "col/col.h"
#include "core/tomb.h"

/* Why the parent cannot vouch (SIFT_CONN_OK: it can). EXPLAIN says it as "fallback: <why>" */
typedef enum SiftConnWhy
{
	SIFT_CONN_OK = 0,
	SIFT_CONN_NO_JOURNAL,		/* the parent index keeps no journal (no connection was made to it), or is not ready, or is a table that cannot be tracked */
	SIFT_CONN_NOT_COMPLETE,		/* the tombstone log is not complete for this snapshot: triggers, baseline, concurrent build, subscription target */
	SIFT_CONN_LINEAGE,			/* the parent index was rebuilt (REINDEX, TRUNCATE, a new file): the stamp names another journal */
	SIFT_CONN_TRIMMED,			/* records at or after the stamp's horizon were dropped (VACUUM, size cap, a log made complete again) */
	SIFT_CONN_SNAPSHOT,			/* the snapshot is not an MVCC one, or SERIALIZABLE, or a standby snapshot that cannot be compared with the stamp */
	SIFT_CONN_OLDER,			/* S.xmin < X: the snapshot may not see what the value was derived from */
	SIFT_CONN_AGED,				/* the stamp is too old to compare with a current transaction id (wrap-around guard) */
	SIFT_CONN_OVERFLOW			/* more changed keys than the caller's cap: no keys are returned */
} SiftConnWhy;

/*
 * What a derived value is as of: stored with the derived structure (a segment, a map), 16 bytes. Taken by sift_conn_take BEFORE the parent's entries are read
 * with the snapshot it was taken for.
 */
typedef struct SiftConnStamp
{
	uint64		lineage;		/* the id of the parent's journal: another after a rebuild of the parent index */
	TransactionId xmin;			/* X, the horizon (xmin of the snapshot the value was derived under) */
	uint32		reserved;		/* zero */
} SiftConnStamp;

/*
 * A cheap, comparable word for "has the parent's journal moved": two equal generations of one stamp give equal changed sets, so a consumer that cached
 * changed(S, X) at a generation needs no read of the journal while it is equal. Read from the header page of the log alone.
 */
typedef struct SiftConnGen
{
	uint64		lineage;
	uint64		records;		/* journal and tombstone records appended so far */
	TransactionId trim;			/* first horizon the journal vouches for */
	uint32		flags;			/* SIFT_CONN_GEN_COMPLETE: the tombstone log is complete (its flags and the witnesses of the triggers still have to match: sift_conn_changed checks) */
} SiftConnGen;

#define SIFT_CONN_GEN_COMPLETE	0x0001

/* The changed keys of changed(S, X): sorted, unique, as int64 (the value of the parent key column widened) */
typedef struct SiftConnKeys
{
	int64	   *keys;
	uint32		n;
	uint32		cap;
} SiftConnKeys;

/* Is key in the set (binary search) */
extern bool sift_conn_has(const SiftConnKeys *k, int64 key);
extern void sift_conn_keys_free(SiftConnKeys *k);

/*
 * The parent index gets its journal (a connection is being made to it): created under the lock that holds writers of the parent table out (the caller holds ShareLock
 * on the table: CREATE INDEX and REINDEX of the child do). Idempotent; the parent index must track a key column (sift_col_row_key_on) and be complete. Returns the
 * lineage.
 */
extern uint64 sift_conn_enable(Relation parent_heap, Relation parent_index);

/*
 * A child index is connected to the parent index: the dependency of the child on the parent (so that the parent index and its table cannot be dropped under it without CASCADE,
 * which drops the child index, and so that a dump makes the parent first) and the parent's journal (sift_conn_enable).
 */
extern void sift_conn_connect(Relation child_index, Relation parent_heap, Relation parent_index);
/* Only the dependency of the first half: the journal is made when the child has read its table (sift_conn_start) */
extern void sift_conn_depend(Relation child_index, Relation parent_index);
/* The sift_col indexes connected to the parent index */
extern List *sift_conn_children(Oid parent_index);
/*
 * The build of a parent index (CREATE INDEX, REINDEX, the copy of REINDEX CONCURRENTLY or pg_repack, which the swap puts in the place of an index that has children) makes its journal
 * when children are connected to the index it is or replaces, so that a parent with a connected child never loses it. The writers are out (the lock of the build) or cannot see the
 * index yet (a concurrent build before it is ready); whether the log is complete is the vouch's question, not this one's.
 */
extern void sift_conn_build(Relation parent_heap, Relation parent_index);

/*
 * The stamp of a derivation that is about to read the parent's entries under resolve (an MVCC snapshot): X = resolve->xmin and the lineage of the journal. Not OK when
 * the parent cannot vouch now (the derivation then needs no stamp: it is read by the join path). gen, when not NULL, is the generation at the stamp.
 */
extern SiftConnWhy sift_conn_take(Relation parent_heap, Relation parent_index, Snapshot resolve, SiftConnStamp *stamp, SiftConnGen *gen);

/* The generation now (a read of the header page); false when the parent keeps no journal */
extern bool sift_conn_gen(Relation parent_index, SiftConnGen *gen);
extern bool sift_conn_gen_equal(const SiftConnGen *a, const SiftConnGen *b);

/*
 * changed(S, X) for the stamp: SIFT_CONN_OK with out filled (n may be 0), or the reason it cannot be told. cap bounds the keys returned (more: SIFT_CONN_OVERFLOW,
 * out empty); the cost is the journal records at or after X, not the index. The records this backend holds back are written first, so S's own changes are in the
 * set. Every verdict other than OK means "read the join path for these rows".
 */
/*
 * Could a row have been inserted into the parent index by a transaction at or after the stamp's horizon (committed, in progress, or the caller's own; an aborted one does not
 * count)? Conservative: false only when every segment says its inserters finished before the horizon (SIFT_SEG_XSTAMP) and no live record of the write buffer is from
 * the horizon on. An UPDATE writes the new version as a row too, so it answers true as well; the keys of an update are in changed(S, X). Only for a stamp the journal vouches for.
 */
extern bool sift_conn_inserted_since(Relation parent_heap, Relation parent_index, const SiftConnStamp *stamp);
extern SiftConnWhy sift_conn_changed(Relation parent_heap, Relation parent_index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out);

/* Short words for EXPLAIN and the tests */
extern const char *sift_conn_why_name(SiftConnWhy why);

/* Writers: does the index keep a journal, and the KEYCHG record (for the current transaction and statement) for a key of the column type keytype; false when it keeps none */
extern bool sift_conn_journal_on(Relation index);
extern bool sift_conn_record(Relation index, Oid keytype, Datum key, SiftTomb *rec);

/* The same checks without the keys: may a derivation stamped so be read by snap at all (the first four conditions above) */
extern SiftConnWhy sift_conn_vouches(Relation parent_heap, Relation parent_index, const SiftConnStamp *stamp, Snapshot snap);

/*
 * ---- the declaration: WITH (connect = 'fk -> [schema.]parent(key) carries a, b [via carried|map|auto]; fk2 -> parent2(key) carries c') ----
 *
 * One connection for each column of the index that holds a parent key (the same parent twice through two columns is two connections); each has its own parent index, stamp and via, and
 * the journal of one parent that does not vouch leaves the others as they are.
 */

typedef enum SiftConnVia
{
	SIFT_CONN_VIA_AUTO = 0,		/* the planner picks by cost (carried column or shared map) */
	SIFT_CONN_VIA_CARRIED,		/* carried columns in the child's segments */
	SIFT_CONN_VIA_MAP			/* the shared dimension map only: nothing is stored in the child */
} SiftConnVia;

#define SIFT_CONN_MAX_CONNECTIONS	8		/* connections of one index */
#define SIFT_CONN_MAX_CARRY			32		/* carried columns of one index, all connections together */
#define SIFT_CONN_MAX_CARRY_BYTES	512		/* bytes of the carried values of a row, all connections together (the width of each leaf; a string counts its 4 byte code) */

typedef struct SiftConnSpec
{
	char	   *fk;				/* column of the child index that holds the parent key */
	char	   *parent_schema;	/* NULL: the schema of the child's table */
	char	   *parent_table;
	char	   *parent_key;
	int			ncarry;
	char	   *carry[SIFT_CONN_MAX_CARRY];
	SiftConnVia via;
} SiftConnSpec;

typedef struct SiftConnSpecs
{
	int			n;
	bool		automatic;		/* the option is 'auto: ..': the connections were chosen from the foreign keys (sift_conn_auto) */
	SiftConnSpec *c[SIFT_CONN_MAX_CONNECTIONS];
} SiftConnSpecs;

/* The leaves of one connection: the flag that the parent row was found is leaf found, its carried leaves follow up to end (exclusive) */
typedef struct SiftConnLayout
{
	int			found;
	int			end;
} SiftConnLayout;

/* Parses the option text without the catalog (after an 'auto:'); NULL with *err set to what is wrong */
extern SiftConnSpecs *sift_conn_parse(const char *text, const char **err);
/* The connections a child index declares (ERROR for text that does not parse), NULL when it declares none */
extern SiftConnSpecs *sift_conn_specs(Relation child_index);

/*
 * The leaves a connected index takes from its parents (SIFT_COLD_CARRIED): for each connection, in the order of the option, the flag that the parent row was found
 * ("<fk>#found") and the leaves of each carried column ("<fk>.<column>"). Appended to defs from n; returns the new count, and lay[] the leaves of each of the *nconn connections.
 */
extern int	sift_conn_leaves(Relation child_index, SiftColDef *defs, int n, SiftConnLayout *lay, int *nconn);
/* CREATE INDEX, REINDEX and ALTER INDEX of the child: connects to the parent indexes the option names, or removes the connections */
extern void sift_conn_resolve(Relation child_heap, Relation child_index);
/*
 * CREATE INDEX and REINDEX of a child index that has no connect option (or connect = 'auto'): connects it to the parents of its validated foreign keys when sift.col_auto_connect is
 * on, writing the choice into the option as 'auto: <connections>' (what pg_dump writes, so that a restore does not need the constraints) and saying it in a NOTICE.
 */
extern void sift_conn_auto(Relation child_heap, Relation child_index);
extern PGDLLIMPORT bool sift_col_auto_connect;
/*
 * The journals of the parents of a connected child, made when the build of the child has read its table: the writers of a parent table are held out from here to the end of the
 * statement, not for the whole scan of a large child. A parent that has a connected child already is not locked at all.
 */
extern void sift_conn_start(Relation child_heap, Relation child_index);
extern void sift_conn_alter(Relation child_index);

#endif							/* SIFT_COL_CONN_H */
