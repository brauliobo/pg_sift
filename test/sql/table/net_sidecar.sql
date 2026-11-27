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
-- whether the statement read every granule from its pre-aggregates, none of its rows (EXPLAIN)
\ir test/common/sql/agg/preagg_units.sql
CREATE FUNCTION answered_net(q text) RETURNS bool LANGUAGE sql AS $$ SELECT pre > 0 AND pre = units FROM pa_units(q) $$;

DELETE FROM t WHERE id % 10 = 3;
UPDATE t SET n = n + 1000 WHERE id % 20 = 5;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS before_vacuum;
VACUUM t;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS after_vacuum;
SELECT same('SELECT count(*), sum(n) FROM t WHERE g IN (1, 2)') AS filtered;
SELECT answered_net('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS answered_net;

-- rows that die after the sidecar was written: the units read what the sidecar does not know
DELETE FROM t WHERE id % 50 = 7;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS newer_deletes;
VACUUM t;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS vacuumed_again;
SELECT answered_net('SELECT g, count(*), sum(n) FROM t GROUP BY g') AS answered_net_again;
SELECT count(*), sum(n) FROM t;

DROP FUNCTION answered_net(text), pa_units(text);
DROP FUNCTION same(text);
DROP TABLE t;
DROP EXTENSION pg_sift;
