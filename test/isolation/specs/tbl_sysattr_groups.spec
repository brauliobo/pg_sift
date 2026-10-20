# The system columns of the rows of a column group that keeps the version of every row (the directory limit is lowered, a testing setting, so
# that the write buffers of a running transaction are promoted before it ends): xmin and cmin are read from the version section of the group,
# xmax from the tombstones. After the writer committed the rows are unfrozen groups still, and another session reads the writer's xmin (a later
# write may freeze final rows by merging them, so the first transaction's rows are read before it); VACUUM FREEZE then freezes them
# (FrozenTransactionId) and drops the lock.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE sg (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE TABLE sg_inserters (n int, x xid);
}

teardown
{
	DROP TABLE sg, sg_inserters;
}

session writer
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; SET max_parallel_workers_per_gather = 0; }
step wbegin	{ BEGIN; INSERT INTO sg_inserters SELECT 1, pg_current_xact_id()::xid; }
step wins1	{ INSERT INTO sg SELECT x, repeat('x', 1000) || x FROM generate_series(1, 40000) x; }
step wins2	{ INSERT INTO sg SELECT x, repeat('y', 1000) || x FROM generate_series(40001, 80000) x; }
step wgroups	{ SELECT count(*) FILTER (WHERE kind = 'columns') > 0 AS promoted_with_versions FROM sift.table_segments('sg'); }
step wown	{ SELECT (id > 40000) AS second_command, cmin::text AS cmin, count(*) AS n, count(DISTINCT xmin::text) AS xmins,
		bool_and(xmin = pg_current_xact_id()::xid) AS inserter FROM sg GROUP BY 1, 2 ORDER BY 1; }
step wlock	{ SELECT id FROM sg WHERE id = 5 FOR UPDATE; }
step wlocked	{ SELECT count(*) AS n, bool_and(xmax = pg_current_xact_id()::xid) AS locker FROM sg WHERE xmax::text <> '0'; }
step wdel	{ DELETE FROM sg WHERE id = 8; }
step wcommit	{ COMMIT; }
step wmore	{ BEGIN; INSERT INTO sg_inserters SELECT 2, pg_current_xact_id()::xid; INSERT INTO sg SELECT x, repeat('z', 1000) || x FROM generate_series(80001, 90000) x; COMMIT; }
step wupd	{ UPDATE sg SET v = 'updated' WHERE id = 7; }

session reader
step rinserters	{ SELECT i.n AS inserter, count(*) AS rows, bool_and(g.xmin = i.x) AS xmin_is_inserter
		FROM sg g JOIN sg_inserters i ON i.n = CASE WHEN g.id > 80000 THEN 2 ELSE 1 END GROUP BY 1 ORDER BY 1; }
step rsecond	{ SELECT count(*) AS rows, bool_and(g.xmin = i.x) AS xmin_is_inserter FROM sg g JOIN sg_inserters i ON i.n = 2 WHERE g.id > 80000; }
step rlocker	{ SELECT id, xmax = (SELECT x FROM sg_inserters WHERE n = 1) AS locked_by_first FROM sg WHERE id IN (4, 5, 6) ORDER BY id; }
step rversions	{ SELECT id, xmin = (SELECT x FROM sg_inserters WHERE n = 1) AS first_inserter FROM sg WHERE id IN (6, 7, 8, 9) ORDER BY id; }
step rfrozen	{ SELECT count(*) AS rows, count(*) FILTER (WHERE xmin::text = '2') AS frozen FROM sg; }

session freezer
step vfreeze	{ VACUUM FREEZE sg; }

permutation wbegin wins1 wins2 wgroups wown wlock wlocked wcommit rinserters rlocker wupd wdel rversions wmore rsecond vfreeze rfrozen rlocker
