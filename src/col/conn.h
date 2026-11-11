/*
 * col/conn.h - connected indexes: the connection of an index on a child table to the sift_col index on the parent table it joins, and the protocol that lets a
 * value derived from the parent (the found flag of the child's segments, a shared map of the parent's attributes) be read as exact.
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

#include "access/genam.h"
#include "access/transam.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "col/col.h"
#include "col/enc.h"
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

/* The distinct values of a[0..n), ascending: sorts in place and returns how many */
extern uint32 sift_conn_keys_sort_unique(int64 *a, uint32 n);

/* The changed keys of changed(S, X): sorted, unique, as int64 (the value of the parent key column widened) */
typedef struct SiftConnKeys
{
	int64	   *keys;
	bool	   *kept;			/* [n] every record of the key is of an update that wrote a new version of the same key (no delete, no key change): a row holds the key for every snapshot */
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
/* The same, with the keys of the inserted entries (adds) of a scope relation's journal as well (col/scope.h) */
extern SiftConnWhy sift_conn_changed_by(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, bool adds, SiftConnKeys *out);

/*
 * The journal of a scope relation: a sift_col index with track_changes = 'ctid' whose first column is the member key. The row trigger of the index writes a KEYCHG record for the old
 * version of each row an UPDATE or DELETE ended (ended), its aminsert a KEYADD record for each version inserted (added); both are held with the tombstones.
 */
extern void sift_conn_ended_row(Relation index, Relation heap, HeapTuple old);
extern void sift_conn_added_row(Relation index, Relation heap, const Datum *values, const bool *isnull);

/* Short words for EXPLAIN and the tests */
extern const char *sift_conn_why_name(SiftConnWhy why);

/* Writers: does the index keep a journal, and the KEYCHG record (for the current transaction and statement) for a key of the column type keytype; kept: an update that wrote a new version with the same key; false when it keeps no journal */
extern bool sift_conn_journal_on(Relation index);
extern bool sift_conn_record(Relation index, Oid keytype, Datum key, bool kept, SiftTomb *rec);

/* The same checks without the keys: may a derivation stamped so be read by snap at all (the first four conditions above) */
extern SiftConnWhy sift_conn_vouches(Relation parent_heap, Relation parent_index, const SiftConnStamp *stamp, Snapshot snap);

/*
 * ---- the declaration: WITH (connect = 'fk -> [schema.]parent(key); fk2 -> parent2(key)') ----
 *
 * One connection for each column of the index that holds a parent key (the same parent twice through two columns is two connections); each has its own parent index and stamp, and
 * the journal of one parent that does not vouch leaves the others as they are.
 */

typedef struct SiftConnSpec
{
	char	   *fk;				/* column of the child index that holds the parent key */
	char	   *parent_schema;	/* NULL: the schema of the child's table */
	char	   *parent_table;
	char	   *parent_key;
} SiftConnSpec;

typedef struct SiftConnSpecs
{
	int			n;
	bool		automatic;		/* the option is 'auto: ..': the connections were chosen from the foreign keys (sift_conn_auto) */
	SiftConnSpec *c[SIFT_CONN_MAX_CONNECTIONS];
} SiftConnSpecs;

/* Parses the option text without the catalog (after an 'auto:'); NULL with *err set to what is wrong */
extern SiftConnSpecs *sift_conn_parse(const char *text, const char **err);
/* What the declarations of connect and scope (col/scope.h) share: the name and blank readers of their grammar, the column of the index of a table column, the integer types, and the oid of a named table */
extern const char *sift_conn_skip_ws(const char *p);
extern const char *sift_conn_read_name(const char *p, char **out, const char **err);
extern AttrNumber sift_conn_index_column(Relation index, AttrNumber heap_att);
extern bool sift_conn_integer_type(Oid typid);
extern Oid	sift_conn_relation_oid(Oid index_table, const char *schema, const char *name, LOCKMODE lock, bool missing_ok);
/* The connections a child index declares (ERROR for text that does not parse), NULL when it declares none */
extern SiftConnSpecs *sift_conn_specs(Relation child_index);

/*
 * The leaves a connected index takes from its parents (SIFT_COLD_FOUND): for each connection, in the order of the option, the flag that the parent row was found
 * ("<fk>#found"). Appended to defs from n; returns the new count, and found[] the leaf of the flag of each of the *nconn connections.
 */
extern int	sift_conn_leaves(Relation child_index, SiftColDef *defs, int n, int *found, int *nconn);
/* The parent index that serves a connection (ERROR with a hint when there is none); the parent table is left open in *parent, locked AccessShare */
extern Oid	sift_conn_parent_index(Relation child_heap, Relation child_index, const SiftConnSpec *s, Relation *parent);
/* CREATE INDEX, REINDEX and ALTER INDEX of the child: connects to the parent indexes the option names, or removes the connections */
extern void sift_conn_resolve(Relation child_heap, Relation child_index);
/*
 * CREATE INDEX and REINDEX of a child index that has no connect option (or connect = 'auto'): connects it to the parents of its validated foreign keys when sift.col_auto_connect is
 * on, writing the choice into the option as 'auto: <connections>' (what pg_dump writes, so that a restore does not need the constraints) and saying it in a NOTICE.
 */
extern void sift_conn_auto(Relation child_heap, Relation child_index);
extern PGDLLIMPORT bool sift_col_auto_connect;
extern PGDLLIMPORT int sift_col_connect_refresh_pct;
/*
 * The journals of the parents of a connected child, made when the build of the child has read its table: the writers of a parent table are held out from here to the end of the
 * statement, not for the whole scan of a large child. A parent that has a connected child already is not locked at all.
 */
extern void sift_conn_start(Relation child_heap, Relation child_index);
extern void sift_conn_alter(Relation child_index);

/*
 * The connection of the child index from its column fk (an attribute of the index) to the key of a parent table: its number, the leaf of its flag that the parent row was
 * found and the sift_col index of the parent that serves it. -1 when the index has no such connection.
 */
extern int	sift_conn_find(Relation heap, Relation index, AttrNumber fk, Oid parent_relid, AttrNumber parent_key, int *found, Oid *parent_index);
/* The flag of the connection as a statement may read it for every row of the source (col/scan/proof.c): NULL, with the oldest stamp of its segments and whether every segment is clean (SIFT_CONN_STAMP_CLEAN), or why not */
struct SiftColSource;
extern const char *sift_col_source_proof(struct SiftColSource *src, int found_leaf, SiftConnStamp *oldest, bool *clean);

/*
 * ---- the found flags of a segment ----
 *
 * The writer of a segment of a connected index (a build, a promotion, a merge) fills the found flag of every unit it writes from the parent tables: for each connection it takes a
 * stamp of the parent's journal (sift_conn_take) and knows the keys of the parent table under the snapshot the stamp was taken for, by reading the parent once when the fill is begun or by probing its unique btree for the keys of each batch, whichever is cheaper; a row whose key is found there gets
 * the found flag, every other row (a NULL key, a key the parent lacks) gets NULLs and is read by the join. A connection whose journal does not vouch now (no journal
 * yet, a log that is not complete) clears its flags and leaves the stamp empty (lineage 0): the segment says nothing for it.
 * The stamps are written with the segment (the SCHEMA section, column format 44) and are what a reader compares with its snapshot (the rule above).
 */
struct SiftColFill;
/* rows: the most rows the fill will be asked about (what the writer is to write: a promotion's buffer, a merge's input, a build's table); it prices the parent scan against the probes */
extern struct SiftColFill *sift_conn_fill_begin(Relation child_index, MemoryContext mcxt, uint64 rows);
/* The writer is done with the fill (every segment it writes is finished): releases the snapshot a probing fill keeps. NULL is fine. */
extern void sift_conn_fill_end(struct SiftColFill *fill);

/* What a fill prices its two ways to know the keys of a table with: its estimated rows, and whether asking its btree for `rows` keys is cheaper than reading it (the connection fill and the scope fill) */
extern double sift_conn_rows_of(Relation rel);
extern bool sift_conn_probe_cheaper(uint64 rows, double parent_rows);

/* The keys of a parent table under a snapshot (col/conn_keys.c): the least memory exact membership takes (a bitmap of a dense range, else a sorted array) */
typedef struct SiftConnKeySet SiftConnKeySet;
extern SiftConnKeySet *sift_conn_keyset_load(MemoryContext cx, Relation parent, AttrNumber key, Snapshot snap, Size limit);
extern bool sift_conn_keyset_has_one(const SiftConnKeySet *set, int64 k);
/* Calls f(arg, k, k2) for every visible row of rel whose integer column key (and column key2, InvalidAttrNumber: none, k2 is 0) is not NULL; the keys of a parent table and the pairs of a scope relation */
extern bool sift_conn_slot_key(TupleTableSlot *slot, AttrNumber att, int64 *k);
typedef void (*SiftConnKeyFn) (void *arg, int64 k, int64 k2);
extern void sift_conn_scan_keys(Relation rel, Oid pred_index, AttrNumber key, AttrNumber key2, Snapshot snap, SiftConnKeyFn f, void *arg);

/* The btree probe of the keys of a batch (col/conn_probe.c): the other way to know whether the parent has one visible row for a key */
typedef struct SiftConnProbe SiftConnProbe;
extern SiftConnProbe *sift_conn_probe_begin(MemoryContext cx, Relation parent, AttrNumber key, Snapshot snap);
extern void sift_conn_probe_batch(SiftConnProbe *p, const int64 *keys, const uint64 *nulls, uint32 nrows, bool *one);
/*
 * The probe of any btree by one of its columns: a scan opened for a run of keys, each key looked up with each(arg, slot) called for every visible row of the table it names (the scan
 * stops when it returns false). sift_conn_probe_begin_on names the btree and the probed column of the table (a pair index of a scope relation holds it first or second).
 */
typedef struct SiftConnProbeScan
{
	Relation	heap,
				idx;
	IndexScanDesc scan;
	TupleTableSlot *slot;
} SiftConnProbeScan;
typedef bool (*SiftConnProbeFn) (void *arg, TupleTableSlot *slot);
extern SiftConnProbe *sift_conn_probe_begin_on(MemoryContext cx, Relation parent, Oid btree, AttrNumber key, Snapshot snap);
extern void sift_conn_probe_open(SiftConnProbe *p, SiftConnProbeScan *s);
extern void sift_conn_probe_key(SiftConnProbe *p, SiftConnProbeScan *s, int64 key, SiftConnProbeFn each, void *arg);
extern void sift_conn_probe_close(SiftConnProbeScan *s);

/*
 * Refresh: is a segment of a connected index worth a rewrite for what its stamps say? For a connection whose parent journal vouches now, a segment is stale when it was never filled
 * (lineage 0), when the parent index was rebuilt since, or when the keys changed since its stamp are sift.col_connect_refresh_pct
 * percent of the parent's rows (below that the reader patches them: a few keys cost less than the rewrite) or the journal cannot tell. NULL from begin: the index has no connection.
 */
typedef struct SiftConnStale SiftConnStale;
struct SiftColSegment;
extern SiftConnStale *sift_conn_stale_begin(Relation child_index, MemoryContext mcxt);
extern bool sift_conn_stale(SiftConnStale *s, const struct SiftColSegment *seg);
extern void sift_conn_stale_end(SiftConnStale *s);

#endif							/* SIFT_COL_CONN_H */
