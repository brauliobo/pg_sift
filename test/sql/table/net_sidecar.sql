-- VACUUM of a sift_table writes the pre-aggregates of the units that kept a dead row net of it, so that the units answer from them again: the aggregates equal
-- those of the scan with the pre-aggregates off, before the VACUUM (the tombstones), after it (the sidecar), and after more deletes that the sidecar does not know.
CREATE EXTENSION pg_sift;
CREATE TABLE t (id int, g int, n int) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false);
INSERT INTO t SELECT i, i % 7, i % 100 FROM generate_series(1, 18000) i;
VACUUM t;

CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (' || q || ') x' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (' || q || ') x' INTO b;
	RETURN a = b;
END $$;
-- the units a statement answered net of their dead rows (EXPLAIN)
CREATE FUNCTION net_units(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j json;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS off, TIMING off, SUMMARY off) ' || q INTO j;
	RETURN COALESCE((j->0->'Plan'->>'Units Net Of Dead Rows')::int, 0);
END $$;

DELETE FROM t WHERE id % 10 = 3;
UPDATE t SET n = n + 1000 WHERE id % 20 = 5;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS before_vacuum;
VACUUM t;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS after_vacuum;
SELECT same('SELECT count(*), sum(n) FROM t WHERE g IN (1, 2)') AS filtered;
SELECT net_units('SELECT g, count(*), sum(n) FROM t GROUP BY g') > 0 AS answered_net;

-- rows that die after the sidecar was written: the units read what the sidecar does not know
DELETE FROM t WHERE id % 50 = 7;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS newer_deletes;
VACUUM t;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS vacuumed_again;
SELECT net_units('SELECT g, count(*), sum(n) FROM t GROUP BY g') > 0 AS answered_net_again;
SELECT count(*), sum(n) FROM t;

DROP FUNCTION net_units(text);
DROP FUNCTION same(text);
DROP TABLE t;
DROP EXTENSION pg_sift;
