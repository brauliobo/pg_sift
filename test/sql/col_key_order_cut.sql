-- key_order = 'group, key' with key_cut = 'group' cuts the units by the group: the order arrays answer the lists of one group, the cut the counts, the ranges and the
-- group-bys of the keys, which the arrays alone read like an index with no layout. The answers are the table's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

-- ko: 101 groups scattered over the heap, a date scattered, a counter that follows the heap
CREATE TABLE ko (id int PRIMARY KEY, group_id int, d date, seq int, v int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ko SELECT i, i % 101, date '2020-01-01' + (random() * 1500)::int, i, i % 1000, repeat('x', 100) FROM generate_series(1, 60000) i;
CREATE TABLE kp AS SELECT * FROM ko;
VACUUM (FREEZE, ANALYZE) ko;
ANALYZE kp;

-- the lookups: the group cut reads the units of the group, the list is the table's
CREATE INDEX ko_i ON ko USING sift_col (id, group_id, d, seq, v) WITH (key_order = 'group_id, d', key_cut = 'group_id');
CREATE FUNCTION kunits(q text) RETURNS int LANGUAGE plpgsql AS
$$ DECLARE l text; r int;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		IF l ~ 'Units:' THEN r := substring(l FROM 'Units: (\d+) read'); END IF;
	END LOOP;
	RETURN r;
END $$;
SET sift.enable_agg = on;
SET sift.agg_force = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT kunits('SELECT count(*), sum(v) FROM ko WHERE group_id = 17') < 5 AS group_reads_few_units;
SELECT md5(string_agg(t::text, '|')) = (SELECT md5(string_agg(t::text, '|')) FROM (SELECT id, d FROM kp WHERE group_id = 17 ORDER BY d DESC, id DESC LIMIT 20 OFFSET 40) t) AS list_is_the_tables
FROM (SELECT id, d FROM ko WHERE group_id = 17 ORDER BY d DESC, id DESC LIMIT 20 OFFSET 40) t;
SELECT (SELECT (count(*), sum(v), max(d)) FROM ko WHERE group_id BETWEEN 10 AND 30 AND d >= date '2022-01-01') IS NOT DISTINCT FROM
	(SELECT (count(*), sum(v), max(d)) FROM kp WHERE group_id BETWEEN 10 AND 30 AND d >= date '2022-01-01') AS range_is_the_tables;

DROP TABLE ko, kp;
DROP FUNCTION kunits(text);
DROP EXTENSION pg_sift;
