-- key_order = 'group, key' with no key_cut cuts the units by the group (the key as second key where the sample says it costs little): the order arrays answer the lists of one
-- group, the cut the counts, the ranges and the group-bys of the keys, which the arrays alone read like an index with no layout. The build writes key_cut = 'auto: group, ...' and
-- sift.key_cut_decision says the same; a key_cut of the user's, 'off' too, wins; so does an order_columns of the user's, which the cut is not made with; a group stored together
-- is not cut. The zones of the other leaves (an arrival timestamp) are not weighed against the layout the user named. The answers are the table's.
CREATE EXTENSION pg_sift;
SET sift.test_col_legacy_options = on;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

-- ko: 101 groups scattered over the heap, a date scattered, a counter that follows the heap; kt: the same rows stored by group
CREATE TABLE ko (id int PRIMARY KEY, group_id int, d date, seq int, v int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ko SELECT i, i % 101, date '2020-01-01' + (random() * 1500)::int, i, i % 1000, repeat('x', 100) FROM generate_series(1, 60000) i;
CREATE TABLE kt (LIKE ko) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO kt SELECT * FROM ko ORDER BY group_id, d, id;
CREATE TABLE kp AS SELECT * FROM ko;
VACUUM (FREEZE, ANALYZE) ko, kt;
ANALYZE kp;

-- the key a build cut by (the first of its columns) and what the decision says it would (a key_cut of the user's is theirs: the decision is the rule's, not the build's)
CREATE FUNCTION kbuilt(tbl regclass, w text) RETURNS TABLE (key_cut text, decision text) LANGUAGE plpgsql AS
$$ BEGIN
	EXECUTE format('CREATE INDEX kb_i ON %s USING sift_col (id, group_id, d, seq, v) %s', tbl, w);
	RETURN QUERY SELECT b.b, d.d
	FROM (SELECT coalesce((SELECT split_part(substring(o FROM 'key_cut=(?:auto: )?(.*)'), ',', 1) FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'kb_i' AND o ~ '^key_cut='), 'none') b) b,
		(SELECT coalesce((SELECT k.leaf FROM sift.key_cut_decision('kb_i') k WHERE k.decision = 'cut'), 'none') d) d;
	DROP INDEX kb_i;
END $$;
SELECT 'key_order group, key' AS options, k.* FROM kbuilt('ko', 'WITH (key_order = ''group_id, d'')') k
UNION ALL SELECT 'key_order, key_cut = off', k.* FROM kbuilt('ko', 'WITH (key_order = ''group_id, d'', key_cut = ''off'')') k
UNION ALL SELECT 'key_order, key_cut = d', k.* FROM kbuilt('ko', 'WITH (key_order = ''group_id, d'', key_cut = ''d'')') k
UNION ALL SELECT 'key_order, order_columns = d', k.* FROM kbuilt('ko', 'WITH (key_order = ''group_id, d'', order_columns = ''d'')') k
UNION ALL SELECT 'key_order, stored by group', k.* FROM kbuilt('kt', 'WITH (key_order = ''group_id, d'')') k
UNION ALL SELECT 'key_order, group follows heap', k.* FROM kbuilt('ko', 'WITH (key_order = ''seq, d'')') k
UNION ALL SELECT 'no key_order', k.* FROM kbuilt('ko', '') k;

-- the lookups: the group cut reads the units of the group, the list is the table's
CREATE INDEX ko_i ON ko USING sift_col (id, group_id, d, seq, v) WITH (key_order = 'group_id, d');
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

DROP TABLE ko, kt, kp;
DROP FUNCTION kbuilt(regclass, text), kunits(text);
DROP EXTENSION pg_sift;
