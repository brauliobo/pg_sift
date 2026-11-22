# Scope key store

The scope index of a text, varchar or uuid key ([COL-SCOPE](COL-SCOPE.md), "Key types") names each key by a code, an int8. The code is the position at which the writer of a segment (the build, a promotion, a merge) appended the key to the store of its column, and it never moves.

## What it is

One relation `<table>_<column>_sift_values` for each (table, column) of such a key, with its own table access method (`sift_values`). It is a materialized view that is never populated, on purpose: a publication lists tables only, so an index that makes or drops a store never adds a relation to a publication of all tables. pg_dump and pg_upgrade carry it as an empty view whose query is a constant, and nothing refreshes it (`REFRESH MATERIALIZED VIEW` is refused). It holds the keys only, no row ids and no row data. Nothing reads or writes it but sift; do not modify, truncate, rewrite or drop it. Each index that uses it depends on it (`pg_depend`), so `DROP MATERIALIZED VIEW` of the store is refused while one does; it goes with the last index that uses it, in the transaction that drops that index, and with the table.

The store is a chain of runs (directory entries of its relation, each a dictionary in the searchable block form with its sorted order), merged by the writer so that a lookup reads few of them. A store holds at most 1,073,741,824 keys (`key_store_codes` in [LIMITS](LIMITS.md)).

## Locks

A writer of a scope segment holds the store in `RowExclusiveLock` to the end of its transaction; a reader takes `AccessShareLock` when it first needs a key. The sessions that give new keys their codes take the relation-extension lock of the store, one at a time, for the append of their run only. The merge of the runs takes a page lock of its own that a session only tries for, so no append waits for a merge.

## What 6.0 removed

Releases 5.0 and 5.1 also shared the strings of ordinary leaves between the indexes of a table in these stores (column format minor 46, `sift.col_value_store`). 6.0 does not read those segments: `REINDEX` writes the strings in the segments again (`sift.upgrade_check` names each index).
