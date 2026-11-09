# Connected indexes

A `sift_col` index on a child table can be connected to the `sift_col` index of the parent table it joins, so that a value of the parent that a query reads (a group key, a filter,
a distinct count) is read from the child's index instead of joined. This page describes the protocol that keeps such a value exact. The declaration and the carried columns are
described with the releases that add them.

## The rule

A value derived from the parent for a key `k` is computed under a snapshot `R` of the parent index. `X = R.xmin`: every transaction below `X` was finished when `R` was taken, so it is decided
the same way for `R` and for every snapshot `S` with `S.xmin >= X`. The value is read as stored by `S` when

1. `S` is an MVCC snapshot of a transaction that is not SERIALIZABLE, and `S.xmin >= X`;
2. the parent's key journal vouches for `X`: it is the journal the value was derived under (its lineage), no record at or after `X` was dropped, and the parent's tombstone log is complete for `S`
   (`docs/COL-HEAPFREE.md`);
3. `k` is not in `changed(S, X)`, the keys of the journal records of transactions at or after `X` (committed, in doubt or `S`'s own; the records of aborted transactions are void).

Every other case is read by the join: a key in the set, a key the derived structure has no value for (a parent that did not exist when it was derived), and every key when the journal cannot
vouch. A NULL join key joins nothing and is absent for good. The set must be complete: a key missing from it gives an answer that no check sees, so every doubt (a journal trimmed past `X`, another
lineage, a log that was made complete again, a stamp too old to compare, more keys than the caller keeps) is a reason, never a guess.

## The key journal

The tombstone log of an index is keyed by heap address, and VACUUM drops a tombstone with its entry, so it cannot say which keys changed since a stamp. A parent index with a connected child
keeps a journal in its tombstone segment instead: a 32-byte record of kind `KEYCHG` for the key of every entry an UPDATE, MERGE, `INSERT .. ON CONFLICT DO UPDATE`, DELETE or key change ended, with the
transaction and command. It is written where the tombstones are written, held back with them, written before the commit, a savepoint or a read, and void when the transaction or the savepoint aborts.
VACUUM keeps the records (it never freezes them); it drops those of aborted transactions, and those older than 2^30 transactions, raising `trim` first. An index with no connected child writes none.

The header of the log holds the journal's `lineage` (a random id made with the journal; a rebuilt index has none) and `trim` (the first horizon the journal vouches for). `trim` moves to the next
transaction id whenever the completeness of the log changes (a new baseline, the log made incomplete), because what the log did not hear before is not in the journal either.

The parent key is one `int2`, `int4` or `int8` column with a unique index and `track_changes`. A journal is made under a `ShareLock` of the parent table, held to the end of the transaction, so that every
transaction that wrote the table before is finished.

Format minor 44. A release that does not know it refuses the maintenance of the index (a segment format it does not know is invalid in the directory: VACUUM, merge and promotion fail) and, from 4.3, this build refuses to open the log of a newer format; a release before that appends tombstones without the journal records, so a database must not be written by an older release once a connected child exists (REINDEX of the parent index makes the journal again).

## The declaration

    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, value_num)
      WITH (connect = 'dim_a_id -> dim_a(id) carries category_id, label');

An index names several connections in the one option, separated by `;`, one for each column that holds a parent key (a column is connected once):

    WITH (connect = 'a_id -> dim_a(id) carries category_id; b_id -> dim_b(id) carries zone, score via map;
                     created_by_id -> users(id) carries name; updated_by_id -> users(id) carries name')

The same parent may be connected by two columns (the carried leaves are named by the column of the key, `created_by_id.name` and `updated_by_id.name`). Each connection has its own parent index, dependency, stamp
and journal check, and its own `via`: a parent whose journal does not vouch is read by the join while the others are still read from the index. At most 8 connections and 32 carried columns in an index, and
512 bytes a row for the carried values (a boolean 1 byte, `int2` 2, `int4`, `date`, `real`, an enum or a string's code 4, any other 8 bytes a lane); a declaration over a cap is refused with a hint, never cut.

`fk -> [schema.]parent(key) carries a, b [via carried|map|auto]`: the column of this index that holds the parent key (an `int2`, `int4` or `int8`), the parent table (in the schema of this index's
table when it is not qualified: `CREATE INDEX` builds with a restricted search path and a dump restores with an empty one) and its key column, and the parent columns that queries read through the
join. `via` says how they are read once that exists (`auto`, the default, lets the planner choose). The text is the record of the connection: `pg_dump` writes it, and `CREATE INDEX`, `REINDEX` and
`ALTER INDEX` resolve it again by the names.

Resolving finds the `sift_col` index of the parent table that tracks the key column and holds every carried column as a plain column (the lowest oid when several do), and fails with a hint when there
is none, when a name does not exist, when the key of this index is not an integer, or when the parent is this table. The child index then depends on the parent index (`pg_depend`, normal): `DROP INDEX` of
the parent index, and `DROP TABLE` of the parent table, are refused unless `CASCADE` is given, which drops the whole child index (an index with one connection missing would carry values that no
longer follow their parent), whatever its other connections; a dump makes the parent index first. The journal of the parent is made for the first child connected to it, when the build of the child has read its table, and holds the writers of the parent table out (`ShareLock`) from then to the end of the
transaction: a large child does not hold them out for its scan, and a parent that has a connected child already is not locked at all (`col_conn_lock`). `ALTER INDEX .. SET (connect = 'off')` or `RESET (connect)` removes the dependency; the parent's journal stays
until its next `REINDEX`.

A parent index with a connected child keeps its journal through `REINDEX` and `REINDEX CONCURRENTLY` (and the copy of `pg_repack`): the build makes it for the index it is or replaces. Until the log of a
concurrently built copy is complete (the next VACUUM does it) nothing is vouched.

`sift.col_connections` lists the connections of the indexes (ordinal, column, parent index, declaration, `via`, and whether the parent's journal vouches).

## Automatic connections

`CREATE INDEX` and `REINDEX` of a `sift_col` index that has no `connect` option connect it by themselves (`sift.col_auto_connect`, on by default), and say so:

    NOTICE:  sift_col index "facts_col" connected to dim_a(id) through foreign key "facts_dim_a_id_fkey"
    HINT:  Carry parent columns with connect = 'dim_a_id -> dim_a(id) carries <columns>'; turn it off with connect = 'off' on the index, or SET sift.col_auto_connect = off.

A foreign key connects when it is validated, has one column, that column is in the index and is an `int2`, `int4` or `int8`, the referenced table is a plain heap table and the referenced
column is an integer that a `sift_col` index of that table is ready and tracks (`track_changes`, the default for a primary key). A deferrable key connects as well: the connection never relies
on the integrity of the data, a child row without a parent is read by the join. A foreign key that does not qualify says why in a NOTICE when the referenced table has a `sift_col` index and
in a DEBUG1 message otherwise (a composite key, a key that is not validated, a text key, a parent index that does not track the key). Several foreign keys give several connections (8 at
most; the first foreign key of a column wins).

An automatic connection carries no parent column: it is the key mapping (the found flag of each row and the stamp of the segment), and the join reads the parent's columns from the leaves of the parent's
own `sift_col` index. Carried columns are the user's choice, one measured size and gain at a time: name the connection with `carries`.

The choice is written into the option, `connect = 'auto: a_id -> dim_a(id); ..'`, so `pg_dump` writes it and a restore needs neither the constraints nor the parent's columns to be
looked up again; `REINDEX` keeps it, and `sift.col_connections` has `automatic` true for it. `connect = 'off'` on the index, `ALTER INDEX .. SET (connect = 'off')` and a `REINDEX`, or
`SET sift.col_auto_connect = off` (then `connect = 'auto'` on one index still connects it) turn it off. An explicit `connect` always wins.

The price the user is told about: the journal of the parent index (one 32-byte record for each entry an UPDATE, DELETE or key change of the parent ends), the writers of the parent table held
out from the end of the scan of the first child to the end of the statement, and column format minor 45 for the child.

## The carried leaves

Format minor 45. A connected index holds, after its own leaves and like the leaves of a jsonb path, derived leaves: per connection a boolean leaf `<fk>#found` (the parent row was found when the
segment was made) and a leaf `<fk>.<column>` for each carried column. They are not columns of the table: the write buffer and a row of a segment made before the connection was resolved have no value,
which reads as not found, and a not found row is read by the join. A release before 4.3 refuses a segment of this format; `sift.col_leaves` lists the leaves.

## Filling and the stamp

The writer of a segment (build, promotion of a write buffer, merge) fills the carried leaves: for each connection it takes the stamp of the parent index (`sift_conn_take`, before the parent
is read), reads the parent rows visible to a snapshot of that moment into a lookup table by key (at most half of `maintenance_work_mem`; a bigger parent is not looked up), and gives each row the flag
and the values of its parent row. A row with a NULL key, a key the parent lacks or a key two visible parent rows share gets NULLs, which a reader takes as not resolved. The segment stores the
stamp of each connection with its schema, so a value in it is as of that stamp; what changed in the parent since is in the journal (`changed(S, X)`), and a merge of segments with different stamps
fills the rows again under one new stamp. A connection whose journal does not vouch at that moment (the first child of a parent, whose journal starts after its scan; a parent too big for the
table) leaves the stamp empty and the leaves NULL, and the next promotion or merge fills them. `col_verify` does not compare the carried leaves with the heap: they are as of the stamp.

## Refresh

The carried values of a segment are as of its stamp, so they age with the parent. The rewrite of `sift.col_auto_merge` (the maintenance gate: the WAL budget, the held horizon, the maintenance lock,
`sift.col_maintain`) takes a segment of a connected index when, for a connection whose journal vouches now, the segment was never filled (the first child of a parent, whose journal starts after its
scan), its stamp is of another journal (the parent index was rebuilt), the journal cannot tell what changed since the stamp (trimmed, too many keys), or the keys changed since the stamp are
`sift.col_connect_refresh_pct` percent (10) of the rows of the parent index: below that a read patches those keys from the join, which costs less than the rewrite. The reason is `stale connection` in
`sift.col_maintenance_due`. The rewrite fills the segment again under a new stamp, so the rule ends; a parent that did not fit the memory of the writer leaves a stamp that says so and is not asked for again.

A snapshot older than the journal (a transaction that began before the first child was built and still runs) is never vouched: its reads use the join path for every row, the answer is the same. The
journal vouches for the snapshots taken after its start, so a long-running snapshot costs time and never changes an answer.

## Limits of this stage

No query reads the connection yet: it makes the dependency, the journal, the leaves and their values under a stamp.
