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
#include "utils/rel.h"
#include "utils/snapshot.h"

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
extern SiftConnWhy sift_conn_changed(Relation parent_heap, Relation parent_index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out);

/* Short words for EXPLAIN and the tests */
extern const char *sift_conn_why_name(SiftConnWhy why);

/* Writers: does the index keep a journal, and the KEYCHG record (for the current transaction and statement) for a key of the column type keytype; false when it keeps none */
extern bool sift_conn_journal_on(Relation index);
extern bool sift_conn_record(Relation index, Oid keytype, Datum key, SiftTomb *rec);

/* The same checks without the keys: may a derivation stamped so be read by snap at all (the first four conditions above) */
extern SiftConnWhy sift_conn_vouches(Relation parent_heap, Relation parent_index, const SiftConnStamp *stamp, Snapshot snap);

#endif							/* SIFT_COL_CONN_H */
