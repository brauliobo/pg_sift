# Connected indexes

A `sift_col` index on a child table can be connected to the `sift_col` index of the parent table it joins, so that a value of the parent that a query reads (a group key, a filter,
a distinct count) is found from the child's index instead of joined. This page describes the protocol that keeps such an answer exact, the declaration and the connection of a foreign key.

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
      WITH (connect = 'dim_a_id -> dim_a(id)');

An index names several connections in the one option, separated by `;`, one for each column that holds a parent key (a column is connected once):

    WITH (connect = 'a_id -> dim_a(id); b_id -> dim_b(id); created_by_id -> users(id); updated_by_id -> users(id)')

The same parent may be connected by two columns. Each connection has its own parent index, dependency, stamp and journal check: a parent whose journal does not vouch is read by the join while the
others are still answered from the flags. At most 8 connections in an index.

`fk -> [schema.]parent(key)`: the column of this index that holds the parent key (an `int2`, `int4` or `int8`), the parent table (in the schema of this index's
table when it is not qualified: `CREATE INDEX` builds with a restricted search path and a dump restores with an empty one) and its key column. The parent columns that queries read come from the leaves of the
parent's own `sift_col` index; the connection holds none of them (see below). The text is the record of the connection: `pg_dump` writes it, and `CREATE INDEX`, `REINDEX` and
`ALTER INDEX` resolve it again by the names.

Resolving finds the `sift_col` index of the parent table that tracks the key column (the lowest oid when several do), and fails with a hint when there
is none, when a name does not exist, when the key of this index is not an integer, or when the parent is this table. The child index then depends on the parent index (`pg_depend`, normal): `DROP INDEX` of
the parent index, and `DROP TABLE` of the parent table, are refused unless `CASCADE` is given, which drops the whole child index (an index with one connection missing would keep flags that no
longer follow their parent), whatever its other connections; a dump makes the parent index first. The journal of the parent is made for the first child connected to it, when the build of the child has read its table, and holds the writers of the parent table out (`ShareLock`) from then to the end of the
transaction: a large child does not hold them out for its scan, and a parent that has a connected child already is not locked at all (`col_conn_lock`). `ALTER INDEX .. SET (connect = 'off')` or `RESET (connect)` removes the dependency; the parent's journal stays
until its next `REINDEX`.

A parent index with a connected child keeps its journal through `REINDEX` and `REINDEX CONCURRENTLY` (and the copy of `pg_repack`): the build makes it for the index it is or replaces. Until the log of a
concurrently built copy is complete (the next VACUUM does it) nothing is vouched.

`sift.col_connections` lists the connections of the indexes (ordinal, column, parent index, declaration, whether it was chosen from a foreign key, and the state of the parent's journal).

## Automatic connections

`CREATE INDEX` and `REINDEX` of a `sift_col` index that has no `connect` option connect it by themselves (`sift.col_auto_connect`, on by default), and say so:

    NOTICE:  sift_col index "facts_col" connected to dim_a(id) through foreign key "facts_dim_a_id_fkey"
    HINT:  connect = 'off' on the index turns it off.

A foreign key connects when it is validated, has one column, that column is in the index and is an `int2`, `int4` or `int8`, the referenced table is a plain heap table and the referenced
column is an integer that a `sift_col` index of that table is ready and tracks (`track_changes`, the default for a primary key). A deferrable key connects as well: the connection never relies
on the integrity of the data, a child row without a parent is read by the join. A foreign key that does not qualify says why in a NOTICE when the referenced table has a `sift_col` index and
in a DEBUG1 message otherwise (a composite key, a key that is not validated, a text key, a parent index that does not track the key). Several foreign keys give several connections (8 at
most; the first foreign key of a column wins).

A connection is the key mapping, the found flag of each row and the stamp of the segment, and nothing of the parent's columns: the join reads those from the leaves of the parent's own `sift_col` index.

The choice is written into the option, `connect = 'auto: a_id -> dim_a(id); ..'`, so `pg_dump` writes it and a restore needs neither the constraints nor the parent's columns to be
looked up again; `REINDEX` keeps it, and `sift.col_connections` has `automatic` true for it. `connect = 'off'` on the index, `ALTER INDEX .. SET (connect = 'off')` and a `REINDEX`, or
`SET sift.col_auto_connect = off` (then `connect = 'auto'` on one index still connects it) turn it off. An explicit `connect` always wins.

The price the user is told about is in the next section: the journal of the parent index (one 32-byte record for each entry an UPDATE, DELETE or key change of the parent ends), the writers of the parent
table held out while the first child is made, and column format minor 45 for the child.

## What a connection costs, and why the automatic one stays unconditional

One measure for every write number below: WAL **record bytes per changed parent row**, connected minus `connect = 'off'` on the same data and statements, in the steady regime (one CHECKPOINT before the
sequence, then the statements back to back, the pages imaged once as in a server between checkpoints), statements in which no sift worker logged a pass. Images (full-page, 8 KB) are listed apart because they
depend on which pages a checkpoint last cleaned, not on the connection. Lab: 100k child rows, 10k parents, 24 statements per kind, release build, two runs (`write-reduction-43/conn_wal.py`).

| write | statement size | connected - plain, record B/row | images B/row | typified lane (1000-row batch, steady) |
|---|---|---|---|---|
| child INSERT / UPDATE / DELETE | 2,000 / 200 / 66 rows | 0 index pages; WAL +-60 B/row of 1.7-13.8 KB (noise) | - | +0.4 (append), -0.2 (delete) |
| parent INSERT | 20 rows | 0 (no record) | - | - |
| parent UPDATE (not the key) | 100 rows | +30..+34 | +1 | +25..+34 |
| parent key change | 10 / 60 rows | +43 / +36 | 0 / +205 | +21..+23 |
| parent DELETE | 10 / 60 rows | +41 / +36 | 0 / 0 | +8 |

Why the two labs differ: a record is 32 bytes of journal plus the WAL record header, whatever the regime; what moves a figure from +30 to +200 is the regime. With a CHECKPOINT before every statement every first touch
of a page writes an 8 KB image (about 4.2 KB/row of images for a 100-row parent UPDATE, with or without the connection: -25..+1 B/row difference), and a lab that cuts a whole phase (statements, worker passes,
checkpoint) into one LSN window also takes in the WAL of the worker passes the journal calls for. The first version of the write lab (`conn_writes.py`, a CHECKPOINT after each phase of 100 or 10 rows) read +167..+212 B/row
for the UPDATE and +625..+667 for the key change that way; the statement by statement measure of the same operations reads record +21..+42 and image differences from -1,603 to +780 B/row that change sign between
runs, so the images of a phase are not attributed to the connection. The typified lane's key change (+22) and DELETE (+8) are lower than the +36..+43 measured here for 10 and 60 rows per statement, and a larger
statement does not lower them (60 rows: +36); the residual of up to 33 B/row for these two is not explained. The attribute UPDATE agrees (+30 against +25..+34). Use the steady record column.

Other costs, measured once:

| cost | measure |
|---|---|
| writers of the parent table while the first child is made | `ShareLock` on the parent from the end of the child's build to the end of the statement: 2 ms (1M child rows, with a parent 10x or 1x smaller than the child), 20 ms (5M rows); one parent UPDATE every 10 ms during the build never waited 100 ms (max 8-26 ms connected, 14-43 ms without the connection at 1M; 95 against 54 ms at 5M, at moments far from the lock window) |
| build of the child index (first child) | +8..+10% at 1M rows (5.8 against 5.3 s, 4.6 against 4.2 s with a parent as large as the child; the connected build ran second), +27% at 5M (26.4 against 20.7 s) |
| size of the child index | +2.5% (5M tags), +7% (5M exists shapes), typified lane |
| journal | 32 bytes for each parent row changed; 2,876 records after 24 batches of 100 UPDATEs, 10 key changes and 10 DELETEs |

| read | with the connection |
|---|---|
| parent-read-*, many-to-many tags | neutral, within 1.5% of the instruction sum |
| child rows tested against the parent without reading a parent column (anti, semi, count joins) | -36..-47% instructions (63.1 -> 33.5 M, 76.9 -> 49.4 M), -33..-52% buffer hits (924 -> 441), 33 -> 7.6 ms |
| parent as large as the child | -5..-15% on 4 grouped queries, hits 1,800 -> 740 |

Decision (2026-11-09): the automatic connection stays unconditional (every validated one-column integer foreign key whose parent has a tracking `sift_col` index). A rule that connects only when a gain class applies cannot
be written at `CREATE INDEX`: the gain depends on the queries (anti, semi, count joins on the child), which the index does not know, and the one thing the schema shows, a parent comparable in size to the child, is
the smaller gain (-5..-15%) and the largest cost (a journal of a big table). The cost it would save is at most the +30..+43 B a parent row change writes on top of the 700 B or so a parent UPDATE writes
anyway (4.5%), nothing on child writes and appends, a lock of milliseconds once; the typified lane's rule for writes ("lose when wall time and WAL both rise >= 15%") is not met by any class measured.
The off switch stays `connect = 'off'`. One limit is not covered by the rule and is worth a message: a parent with more keys than half of `maintenance_work_mem` holds at 48 bytes a key (about 700,000 keys at the
default 64 MB) is refused by the fill once, and the connection then keeps its journal and fills no flag.

## The found leaf

Format minor 45. A connected index holds, after its own leaves and like the leaves of a jsonb path, one derived leaf for each connection: a boolean `<fk>#found` (the parent row was found when the
segment was made). It is not a column of the table: the write buffer and a row of a segment made before the connection was resolved have no value, which reads as not found, and a not found row is read
by the join. A release before 4.3 refuses a segment of this format; `sift.col_leaves` lists the leaves.

## Filling and the stamp

The writer of a segment (build, promotion of a write buffer, merge) fills the found flag: for each connection it takes the stamp of the parent index (`sift_conn_take`, before the parent
is read), reads the keys of the parent rows visible to a snapshot of that moment into a lookup table (at most half of `maintenance_work_mem`; a bigger parent is not looked up), and gives each row the flag
of its parent row. A row with a NULL key, a key the parent lacks or a key two visible parent rows share gets NULL, which a reader takes as not resolved. The segment stores the
stamp of each connection with its schema, so a flag in it is as of that stamp; what changed in the parent since is in the journal (`changed(S, X)`), and a merge of segments with different stamps
fills the rows again under one new stamp. A connection whose journal does not vouch at that moment (the first child of a parent, whose journal starts after its scan; a parent too big for the
table) leaves the stamp empty and the flag NULL, and the next promotion or merge fills it. `col_verify` does not compare the flag with the heap: it is as of the stamp.

## Refresh

The flags of a segment are as of its stamp, so they age with the parent. The rewrite of `sift.col_auto_merge` (the maintenance gate: the WAL budget, the held horizon, the maintenance lock,
`sift.col_maintain`) takes a segment of a connected index when, for a connection whose journal vouches now, the segment was never filled (the first child of a parent, whose journal starts after its
scan), its stamp is of another journal (the parent index was rebuilt), the journal cannot tell what changed since the stamp (trimmed, too many keys), or the keys changed since the stamp are
`sift.col_connect_refresh_pct` percent (10) of the rows of the parent index: below that a read patches those keys from the join, which costs less than the rewrite. The reason is `stale connection` in
`sift.col_maintenance_due`. The rewrite fills the segment again under a new stamp, so the rule ends; a parent that did not fit the memory of the writer leaves a stamp that says so and is not asked for again.

A snapshot older than the journal (a transaction that began before the first child was built and still runs) is never vouched: its reads use the join path for every row, the answer is the same. The
journal vouches for the snapshots taken after its start, so a long-running snapshot costs time and never changes an answer.

## Found

A semi, anti or inner join of the scanned table to the parent of a connection that reads no column of the parent (`EXISTS`, `NOT EXISTS`, `IN`, a join that counts, `count(DISTINCT parent.key)`) asks one
thing of each row: is there a parent row with its key. The flag `<fk>#found` of the segments says it as of the stamp, so the parent is not read at all. The scan keeps the rows whose flag is set and whose key
is not among the keys `changed(S, X)` names for the oldest stamp of the segments (at most 16,384), plus the rows of those keys that a row of the parent holds now (a key that only updates ended, which wrote a new version of it (`SIFT_TOMB_KEPT` on the record), is held for every snapshot; a key a delete,
a key change or an update that assigns the key column ended is probed in the parent's unique btree under the scan's snapshot); an anti join keeps the complement. It is one conjunct of the scan, so the zones, the source filters and the parallel workers apply it as to any other, and the join itself passes
every tuple.

It holds for every row only when the flag is the whole story, so the statement uses the ordinary join, with the same answer, when any of these is so (EXPLAIN says it as `Found Fallback`):

| reason | when |
|---|---|
| `write buffer` | a write buffer of the index holds rows (the reader's own too): their flag is not filled until the promotion |
| `segment without stamp` | a segment was never filled (the first child of a parent, whose journal starts after its scan): the rewrite of the refresh fills it |
| `parent too large to resolve` | the writer's parent lookup did not fit `maintenance_work_mem` |
| `stamps of two journals` | the segments were stamped under different journals of the parent index |
| `parent rows inserted since the stamp` | only for a segment that had a row with a key the parent lacked when it was filled (no foreign key keeps the child clean of them, `SIFT_CONN_STAMP_CLEAN`): a flag NULL is "not found then", the journal lists the keys that ended, not those that appeared, and a row inserted into the parent since (an update is an insert) may be the parent of such a row |
| the verdict of the journal (`other lineage`, `trimmed`, `snapshot older than the stamp`, `too many changed keys` ..) | the journal cannot vouch for the stamp or names more than 16,384 keys |
| `too many keys deleted or changed` | more than 2,048 keys were deleted, changed or written by a statement that assigns the key column (an update that leaves the key column alone costs no probe) |
| `no unique index on the parent key` | the keys that changed are probed in a unique btree of the parent |

Only a semi, anti or inner join by one `int2`, `int4` or `int8` column that is the foreign key of the connection, with no condition of the parent's own, no top-N and no partitioned table. `sift.agg_connect_found`
turns it off. EXPLAIN ANALYZE counts `Found Proofs` (joins answered so), `Found Keys Outside The Proof` (the keys the journal named), and for a connected index `Parent Leaf Reads` and `Parent Heap Fetches` (what the joins
that did read the parent loaded from the leaves of its index and from its heap). A parent column of an `int2`, `int4`, `int8`, `date`, `time`, `timestamp`, `timestamptz`, `boolean`, `text`, `varchar`, `float4`, `float8`, `numeric(p <= 18)` or
expression leaf is read from the leaves of the parent's index when all the columns the join reads are in one `sift_col` index; `uuid`, `interval` and `numeric` of more than 18 digits are read
from the heap (`test/sql/col_conn_found.sql` fails if that changes).

## Parent to child

`SELECT count(*) FROM parent p WHERE [p conditions AND] [NOT] EXISTS (SELECT 1 FROM child c WHERE c.fk = p.key AND <c conditions>)` with `p.key` unique (a unique btree on it alone, `int2`, `int4` or
`int8`) is planned also as `SELECT count(DISTINCT c.fk) FROM child c WHERE <c conditions> AND EXISTS (SELECT 1 FROM parent p WHERE p.key = c.fk AND <p conditions>)`; NOT EXISTS is
`count(*)` of the parents the conditions keep less that count (`agg/flat/invert.c`). Each key a child holds is the key of one parent at most, a NULL key matches nothing on either side, so the count is the same. The scan
of the child answers the parent EXISTS from the flag of the connection (Found, above) when the conditions leave the parent no column to read, and the parent is not scanned; the planner keeps it where it costs less than the
statement as written (a parent with few rows and a child filter the scan cannot prune reads less the other way). `sift.agg_child_first` turns it off. Shapes with a second aggregate, a grouping, a
condition of the child that reads the parent, an OR, a parent key that is not unique or has another type than the child's column stay as written (`test/sql/col_conn_invert.sql`).

`SELECT p.attr, count(c.id), sum(c.v) FROM parent p LEFT JOIN child c ON c.fk = p.key GROUP BY p.attr` is planned also with the child grouped by `fk` first (`agg/flat/partial.c`, the rewrite of a left join of
tables to one table): the groups are added up over the parents, a parent of no child counting 0 and summing NULL. The same switch turns it off. 1,000,000 child rows over 100,000 parents: 60 ms, 353 M instructions and 488 blocks against 540 ms, 1,633 M and 7,053 for the lookup join (bench case `child_first_left_join_groups`).

A record on the parent of the number of children of each key, written with the child's writes, was measured and discarded: the reading it would answer (a group by the foreign key of the leaf) has a ceiling of
41 ms at 1,000,000 rows, where the joins it replaces cost 599 and 929 ms today, but every child write would become a write to the parent index (a new version of the parent leaf, a journal record) for a number that
the child scan reads at the same speed once the join is a count of distinct keys.

## Why connections do not carry values

A connection could also store a parent column in the child's segments (a column of the parent as a leaf of the child, filled under the same stamp). It was built, measured against the join the
connection already makes (the found flag, the parent's own `sift_col` leaves read as a dimension map or classes) and removed. The ceiling of a carried column is the column stored in the child itself
(1,000,000 child rows, 100,000 parents, warm; cold within 1%; instructions of the backend):

| parent column | query | by the join | carried (ceiling) |
|---|---|---|---|
| `int` (50 values) | `count(*) .. WHERE p.region = 7` | 9.9 M | 0.58 M |
| `int` | group by it, `count`, `sum` | 102.1 M | 0.64 M |
| `int` | group by it with a filter on the child | 25.0 M | 11.0 M |
| `text` (50 values) | filter | 9.8 M | 0.59 M |
| `text` (50 values) | group by it, `count`, `sum` | 102.0 M | 0.68 M |
| `text` (50 values) | group by it with a filter on the child | 24.9 M | 20.5 M |
| `date` | filter | 8.6 M | 12.2 M |
| `date` | group by it, `count`, `sum` | 102.5 M | 34.1 M |
| `date` | group by it with a filter on the child | 25.4 M | 11.0 M |

The child's index of 16.3 MB grew by 2.1 MB for an `int` or a 50-value `text` column (13%), 3.4 MB for a `date` (21%) and 25.9 MB for a `text` column of 100,000 values (160%); the build took 2.7 to
4.2 s against 3.4 s. A segment merge writes and compares the column again, and the column ages with the parent (the refresh above rewrites the segment). The gain is a few milliseconds a query on a
million rows (5 ms to 0.2 ms for the filter, 12 to 20 ms to 0.3 ms for the group), a date filter is slower carried, and the declaration would be a second place that names the parent's columns, next to the
parent's own index that already holds them. The join reads them from there, with the exactness the rule gives, and the connection stays the key mapping.

## Limits of this stage

The found flag is read by the joins above. A write to the parent changes nothing for the flag of a clean child (every row has a parent row or a NULL key, as a foreign key keeps it): an update that keeps the key, a delete, a new key or a key change
only put keys among those the journal names, which the scan probes; a child with rows whose key the parent lacked (no foreign key) goes to the ordinary path while a row was inserted since the stamp.
