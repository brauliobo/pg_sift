# Shared value store

Release 5.0 (column format minor 46). The sift_col indexes of one table that hold the same string column (text, varchar, uuid ...) store its distinct strings once, in a relation of their own, and their segments refer to them by code.

## What it is

One relation `<table>_<column>_sift_values` for each (table, column) whose strings several indexes share, with its own table access method (`sift_values`). It holds strings only: no row ids, no row data and no more than the indexes hold. Nothing reads or writes it but sift; do not modify, truncate, rewrite or drop it. Each index that uses it depends on it (`pg_depend`), so `DROP TABLE` of the store is refused while one does; it goes with the last index that uses it, in the transaction that drops that index, and with the table. `sift.col_value_store_status` lists the stores and `sift.col_value_store_members()` the string columns of every index and what each does with them.

A segment of a member holds, for each shared leaf, the codes of its distinct strings in the store (bit-packed) and a hash of the strings. Codes are the position at which a string was appended to the store and never move inside an epoch. The store is a chain of runs (directory entries of its relation, each a dictionary in the searchable block form with its sorted order), merged by the writer so that a lookup reads few of them.

## When a column is shared (the rule, automatic)

At CREATE INDEX and REINDEX a string leaf is shared when all of these hold:

- it is a plain column of the table (not a path of a jsonb, not a carried leaf), not in `count_columns`;
- another sift_col index of the table holds the same column **with the same key order and key cut** (the order the index meets the rows in: its `key_cut` and `key_order`, or the table order);
- sharing spares at least 5 bytes a row and more than 25% of what the leaf takes in the index, from the statistics of the column (distinct values, width).

There is no option to turn it on or off for one index. An index whose column another index holds with a **different** key order keeps its own strings, and says so: the NOTICE of CREATE INDEX names the index that holds the column, how much this one stores again and that the same `key_order` would share it; `sift.col_value_store_members()` reports `own: different key order` with the duplicate size. The first member of a column keeps its own strings until it is rebuilt (`own: not yet rebuilt`).

Off switch: `SET sift.col_format_minor = 14` before the build writes strings in the segments (as before 5.0).

## Reads

A shared leaf answers equality, IN, range, prefix and contains from the store in place: each run is searched like a segment dictionary (hash, sorted order, blocks), and group by and distinct work on the codes, translating only the groups that are output. A segment's dictionary is never materialized for a read that does not need it.

## Garbage and epochs

A string that no segment names any more (its rows were deleted or replaced and the segments that held it were merged or rewritten) keeps its code: the store is append-only inside an epoch. `sift.col_value_store_usage(store)` reads, from the segments of the members, the codes each names by epoch, and `sift.col_value_store_status` shows `total_codes`, `live_codes` and `garbage_pct`.

Compaction is lazy and does not rewrite anything by itself:

- A new epoch begins (the codes start again at 0; nothing is copied). VACUUM of the table begins one when the store takes at least 8 MB and half its codes or more are named by no segment, and says so with a NOTICE.
- A segment that is written afterwards names the new epoch: the merge, the rewrite and the REINDEX of a member read the strings of the old epoch from its runs and write the new epoch's. A segment that is not rewritten keeps naming the old epoch, and is read from the old epoch's runs.
- When no segment of any member names an old epoch, VACUUM marks its runs retired, and removes them (their blocks are released) once the transactions that could have read a segment naming it have ended.
- `sift.col_value_store_compact(table, column)` does it now: it begins the epoch, rewrites the members that name the old one and retires it. The owner of the table runs it; it waits for the writers of the store and blocks them while it runs.

## Locks

A writer of a member holds the store in `RowExclusiveLock` to the end of its transaction, from the codes it takes to the segment it publishes; a reader takes `AccessShareLock` when it first needs a string. The retirement of an epoch takes the members (`ShareUpdateExclusiveLock`, as maintenance does) and then the store in `ShareRowExclusiveLock`, so that no segment is written between its codes and its publication while the segments of the members are read for the epochs they name. The append of a run takes the relation-extension lock of the store for the append only.

## Limits

- Only text-like and uuid leaves (numeric types are not shared yet).
- A shared leaf has no trigram signature of its own (contains on it reads the store's runs).
- The members must have the same key order to share (the owner rule); sharing across orders is not done.
