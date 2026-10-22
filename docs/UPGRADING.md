# Upgrading

What to run to move a database to a newer pg_sift. The on-disk formats and the first release that reads each are in [FORMATS.md](FORMATS.md).

## 3.2.1 to 3.2.2

A library-only release: the extension SQL stays at 3.2 and no format changes.

1. Install the files of 3.2.2 (the package, or `make USE_PGXS=1 && sudo make USE_PGXS=1 install`).
2. Restart the server, so that every process runs one library. A new session loads the new file by itself, but a session that was open keeps the old library until it reconnects, and pg_sift in `shared_preload_libraries` (needed to publish a `sift_table` under logical replication) is loaded at start only.
3. There is no `ALTER EXTENSION pg_sift UPDATE` (`extversion` stays 3.2) and no `REINDEX`. Going back to 3.2.1 needs nothing.

3.2.2 fixes the row estimates of partitions and inheritance children under row-level security (a security fix; releases 3.0.0 to 3.2.1) and `permission denied for table` from the aggregate pushdown for a role with column privileges or a view; see [docs/releases/v3.2.2.md](releases/v3.2.2.md).

## Known issue of 3.2.0, 3.2.1 and 3.2.2: a role cannot drop anything

The event trigger function `sift.table_paths_drop()` is not `SECURITY DEFINER` in the 3.2 extension SQL, so a `DROP` by a role that is neither a superuser nor the owner of the extension fails with `permission denied for schema sift` (any table, index or schema, also one without pg_sift). It applies to every database where pg_sift is installed. The SQL of a patch release cannot change; `ALTER EXTENSION pg_sift UPDATE` to 3.3 fixes it. Until then, run once in each database with the extension, as a superuser (and again after restoring a database from a dump, which creates the extension from the 3.2 script):

```sql
ALTER FUNCTION sift.table_paths_drop() SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
```
