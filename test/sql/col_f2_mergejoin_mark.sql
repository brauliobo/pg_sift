-- A merge join over sift_col indexes of format major 2 marks and restores the position of the inner scan: over an index whose scan gives the order of the join key (the join key is
-- the key_order), over an index that gives no order (the planner sorts its rows and the sort marks), through a rescan of the join as the inner side of a nested loop, and with the
-- inner side empty. Every join answers the rows of the heap.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE mj_a (id int, tag text, val int);
CREATE TABLE mj_b (id int, tag text, val int);
INSERT INTO mj_a SELECT g, 't' || (g % 5), g FROM generate_series(1, 500) g;
INSERT INTO mj_b SELECT g % 100, 't' || (g % 5), g FROM generate_series(1, 500) g;
CREATE INDEX mj_a_ord ON mj_a USING sift_col (id, val) WITH (key_order = 'id, val');
CREATE INDEX mj_b_ord ON mj_b USING sift_col (id, val) WITH (key_order = 'id, val');
CREATE INDEX mj_b_txt ON mj_b USING sift_col (tag, id);
VACUUM (ANALYZE) mj_a;
VACUUM (ANALYZE) mj_b;

-- the rows of a join with the merge join forced, and with the heap plan; true when they are the same
CREATE FUNCTION mj_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE merged text; plain text; l text; nodes text := '';
BEGIN
	SET LOCAL enable_hashjoin = off; SET LOCAL enable_nestloop = off; SET LOCAL enable_material = off; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	SET LOCAL sift.enable_agg = off;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Merge Join' THEN nodes := 'merge join'; END IF;
	END LOOP;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (%s) r', q) INTO merged;
	SET LOCAL enable_hashjoin = on; SET LOCAL enable_nestloop = on; SET LOCAL enable_material = on; SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
	SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_mergejoin = off;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (%s) r', q) INTO plain;
	RETURN nodes || ' ' || CASE WHEN merged IS NOT DISTINCT FROM plain THEN 'same' ELSE 'DIFFERENT' END;
END $$;

SELECT mj_same($$SELECT a.id, b.id, b.val FROM mj_a a JOIN mj_b b ON a.id = b.id$$);
SELECT mj_same($$SELECT a.id, b.val FROM mj_a a JOIN mj_b b ON a.id = b.id WHERE a.id < 40$$);
SELECT mj_same($$SELECT a.tag, count(*) FROM mj_a a JOIN mj_b b ON a.tag = b.tag AND b.tag = 't1' GROUP BY a.tag$$);
SELECT mj_same($$SELECT a.id, b.val FROM mj_a a JOIN mj_b b ON a.tag = b.tag WHERE a.id < 4 AND b.id < 3$$);
SELECT mj_same($$SELECT a.id, b.id FROM mj_a a JOIN mj_b b ON a.id = b.id WHERE b.val > 100000$$);
SELECT mj_same($$SELECT a.id, b.id FROM mj_a a JOIN mj_b b ON a.id = b.id AND a.val = b.val$$);

DROP TABLE mj_a, mj_b;
DROP FUNCTION mj_same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
