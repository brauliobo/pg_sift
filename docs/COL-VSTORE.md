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

There is no option to turn it on or off for one index. An index whose column another index holds with a **different** key order keeps its own strings, and says so: the NOTICE of CREATE INDEX says how much this one stores again and how much less it would be with the order of the index that holds the column (`sift.index_choices(index)` gives the command that shares it: the same `key_order`), and `sift.col_value_store_members()` reports `own: different key order` with the duplicate size. The first member of a column keeps its own strings until it is rebuilt (`own: not yet rebuilt`).

Off switch: `SET sift.col_format_minor = 14` before the build writes strings in the segments (as before 5.0).

## Reads

A shared leaf answers equality, IN, range, prefix and contains from the store in place: each run is searched like a segment dictionary (hash, sorted order, blocks), and group by and distinct work on the codes, translating only the groups that are output. A segment's dictionary is never materialized for a read that does not need it.

## The only index left

When the other indexes that held the column with the key order of an index are dropped or rebuilt with another order, nothing is left to share with: DROP INDEX says so with a NOTICE that names the index. From then on the segments that the index writes hold their strings, its maintenance (a merge, a rewrite or VACUUM) rewrites the segments that name the store, and once none does the index stops depending on the store, which goes with the transaction of that VACUUM. `REINDEX INDEX` does it at once. An index that waits for a peer to be rebuilt (the first member, `own: not yet rebuilt`) is not left alone: another index holds the column with its order.

## Garbage and epochs

A string that no segment names any more (its rows were deleted or replaced and the segments that held it were merged or rewritten) keeps its code: the store is append-only inside an epoch. `sift.col_value_store_usage(store)` reads, from the segments of the members, the codes each names by epoch, and `sift.col_value_store_status` shows `total_codes`, `live_codes` and `garbage_pct`.

Compaction is lazy and does not rewrite anything by itself:

- A new epoch begins (the codes start again at 0; nothing is copied). VACUUM of the table begins one when the store takes at least 8 MB and half its codes or more are named by no segment, and says so with a NOTICE.
- A segment that is written afterwards names the new epoch: the merge, the rewrite and the REINDEX of a member read the strings of the old epoch from its runs and write the new epoch's. A segment that is not rewritten keeps naming the old epoch, and is read from the old epoch's runs.
- When no segment of any member names an old epoch, VACUUM marks its runs retired, and removes them (their blocks are released) once the transactions that could have read a segment naming it have ended.
- `sift.col_value_store_compact(table, column)` does it now: it begins the epoch, rewrites the members that name the old one and retires it. The owner of the table runs it; it waits for the writers of the store and blocks them while it runs.

## Locks

A writer of a member holds the store in `RowExclusiveLock` to the end of its transaction, from the codes it takes to the segment it publishes; a reader takes `AccessShareLock` when it first needs a string. The retirement of an epoch takes the members (`ShareUpdateExclusiveLock`, as maintenance does) and then the store in `ShareRowExclusiveLock`, so that no segment is written between its codes and its publication while the segments of the members are read for the epochs they name. The append of a run takes the relation-extension lock of the store for the append only.

## What each type shares

Only a column whose index segments hold a dictionary of strings has something to share. Lab figures of a table of 1,000,000 rows (leaf bytes in one index):

| column type | in the segments | shared |
| --- | --- | --- |
| text, varchar | dictionary of 13.0 MB, chunks of 6.8 kB | yes |
| uuid | dictionary of 10.6 MB, chunks of 6.8 kB | yes |
| bigint, integer, timestamp, float, enum, interval, numeric | chunks of 0.2 to 4.0 MB, no dictionary | no (nothing to spare) |

## Numbers

Lab figures on one table of `id`, `tenant`, a uuid and a 62-byte URL of distinct values (a row each), three sift_col indexes: A (`id, tenant, uid, url, amount`), B (`url, id`) and C (`uid, tenant`). Shared: the rule's choice. Own: `sift.col_format_minor = 14`. Instructions are of the backend (`perf stat`), cold the first statement of a session and warm the second.

| rows | all indexes, own strings | all indexes, shared | the only member left |
| ---: | ---: | ---: | ---: |
| 100,000 | 9.2 MB | 8.4 MB (-8%) | 4.7 -> 6.5 MB (+38%) |
| 500,000 | 47.0 MB | 41.5 MB (-12%) | 24.3 -> 31.9 MB (+31%) |
| 2,000,000 | 229.6 MB | 187.5 MB (-18%) | 116.8 -> 147.5 MB (+26%) |

The gain grows with the indexes that hold the column and with the rows (the codes of an index are a few bits a distinct string, its strings are 60 bytes). An index that is the only one left holds the store as well as its codes: a REINDEX of it (no other index holds the column) takes its strings back and the store goes.

Four indexes on the same table (the three above and D, `url, uid, tenant` with `key_order = 'tenant'`), all indexes and stores in MB:

| rows | own strings | the rule (same key order only) | every order (what a different `key_order` costs) |
| ---: | ---: | ---: | ---: |
| 100,000 | 13.8 | 13.1 (-5%) | 10.3 (-25%) |
| 500,000 | 70.4 | 64.2 (-9%) | 51.1 (-27%) |
| 2,000,000 | 337.7 | 295.3 (-13%) | 254.4 (-25%) |

D keeps its own strings under the rule, and its CREATE INDEX says how much it stores again and what the order of the index that holds the column would spare it (at 2,000,000 rows D's strings are 72.5 MB of the 295.3 MB). Sharing across orders is not done: the codes of an index that meets the rows in another order do not climb in the store, and its strings are read block by block in the order of the store.

Reads, 2,000,000 rows (millions of instructions, own -> shared, cold / warm): equality on the URL 4.8 -> 4.9 / 1.2 -> 1.2; IN of 100 URLs 125 -> 127 / 114 -> 116; range on the URL 2,698 -> 2,788 / 2,245 -> 2,273; group by the URL 951 -> 1,044 / 439 -> 466 (one index), 1,076 / 484 with three indexes, whose parallel builds cut the segments differently and the codes climb in pieces; distinct 2,331 -> 2,332 / 1,814 -> 1,811. At 100,000 and 500,000 rows the same shapes are equal or cheaper (equality 70 -> 3.7 cold at 500,000: the segment's dictionary is not read for a code lookup). Blocks touched fall for group, distinct and range (15,216 -> 4,700 + 269 at 2M for the range), the store's own blocks are read only for the strings that are output.

Builds, 2,000,000 rows: an index that adopts a store builds in 1.0 to 1.45 times the time of one with its strings (B 3.7-5.0 s -> 4.3-7.1 s, C 2.0-2.1 s -> 2.0-2.9 s on a loaded host); the index that creates the stores pays for them once (5.4-7.0 s -> 11.5-13.4 s for two stores of two million strings). WAL of a build is the same.

The stores of the table above take 102.8 MB at 2,000,000 rows (two columns).

Garbage (see above), 200,000 rows and two indexes: with 10% of the rows replaced each round, 63% of the codes are named by no segment after 20 rounds and the store is 42.0 MB where it began with 11.6 MB; with 50% replaced, 86%. A compaction brings the garbage to 0%; the blocks of the retired epoch go to the free list of the store and later runs use them.

## Why on, and the off switch

Default on because the gain is large when two indexes of the table hold the column and the rule shares only then; the reads are as cheap and the builds within 1.5 times. The cost is a relation more per column and the lock protocol above. Off: `SET sift.col_format_minor = 14` before the build (the strings stay in the segments), or `REINDEX` an index that has no other index with its key order.

## Limits

- Only text-like and uuid leaves (numeric types are not shared yet).
- A shared leaf has no trigram signature of its own (contains on it reads the store's runs).
- The members must have the same key order to share (the owner rule); sharing across orders is not done.
