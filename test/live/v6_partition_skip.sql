-- v6_partition_skip.sql - the partitions, parts and granules a top-N leaves unopened (live, partitioned index).
-- Needs the SIFT_TESTING hook sift_test_f2_split_many of the build (the release library has none); its module path is given as :module, e.g. psql -v module=/path/to/pg_sift.
-- Run: psql -X -v ON_ERROR_STOP=1 -v module=... -f v6_partition_skip.sql <db with the extension>
\set QUIET on
\pset format unaligned
\pset tuples_only on
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET TIME ZONE 'UTC';
\ir s52_helpers.sql

DROP TABLE IF EXISTS multi;
CREATE TABLE multi (id bigint PRIMARY KEY, dim_a_id int NOT NULL, recorded_on timestamptz NOT NULL, v int);
CREATE INDEX multi_col ON multi USING sift_col (id, dim_a_id, recorded_on, v) WITH (key_order = 'dim_a_id, recorded_on');
CREATE FUNCTION s52_split_many(regclass, bigint, bigint, int, bigint) RETURNS int AS :'module', 'sift_test_f2_split_many' LANGUAGE C;
SELECT s52_ok('four partitions of four lead keys each', s52_split_many('multi_col'::regclass, 1, NULL, 3, 4) = 3);
INSERT INTO multi SELECT g, g % 16, TIMESTAMPTZ '2026-01-01' + g * interval '1 minute', g FROM generate_series(1, 20000) g;
SELECT s52_ok('the keys filled the four partitions', (SELECT count(DISTINCT part_id) FROM sift.parts('multi_col'::regclass)) = 4);
ANALYZE multi;

SELECT s52_check('equality on the lead key', $q$SELECT id FROM multi WHERE dim_a_id = 9 ORDER BY recorded_on DESC, id DESC LIMIT 10$q$,
                 'SiftAggV6TopN', 'Partitions Skipped: 3');
SELECT s52_check('IN list on the lead key (the range of its values)', $q$SELECT id FROM multi WHERE dim_a_id = ANY ('{1,9}') ORDER BY recorded_on DESC, id DESC LIMIT 25$q$,
                 'SiftAggV6TopN', 'Partitions Skipped: 1');
SELECT s52_check('range on the lead key', $q$SELECT id FROM multi WHERE dim_a_id >= 5 AND dim_a_id <= 6 ORDER BY recorded_on, id LIMIT 30$q$, 'SiftAggV6TopN', 'Partitions Skipped: 3');
SELECT s52_check('no row can satisfy the bounds', $q$SELECT id FROM multi WHERE dim_a_id > 5 AND dim_a_id < 3 ORDER BY recorded_on, id LIMIT 30$q$);
SELECT s52_ok('descending lead key: partitions the first ones fill the heap beyond',
              s52_plan($q$SELECT id FROM multi ORDER BY dim_a_id DESC, recorded_on DESC, id DESC LIMIT 20$q$) ~ 'Partitions Skipped: [1-9]');
SELECT s52_ok('ascending lead key: partitions the first ones fill the heap beyond',
              s52_plan($q$SELECT id FROM multi ORDER BY dim_a_id, recorded_on, id LIMIT 20$q$) ~ 'Partitions Skipped: [1-9]');
SELECT s52_check('descending lead key rows', $q$SELECT id FROM multi ORDER BY dim_a_id DESC, recorded_on DESC, id DESC LIMIT 20$q$, 'SiftAggV6TopN');
SELECT s52_check('ascending lead key rows', $q$SELECT id FROM multi ORDER BY dim_a_id, recorded_on, id LIMIT 20 OFFSET 30$q$, 'SiftAggV6TopN');

CREATE FUNCTION s52_pk(a int[], n bigint) RETURNS TABLE (id bigint) LANGUAGE plpgsql STABLE AS $f$
BEGIN
  RETURN QUERY SELECT m.id FROM multi m WHERE m.dim_a_id = ANY (a) ORDER BY m.recorded_on DESC, m.id DESC LIMIT n;
END $f$;
SET plan_cache_mode = force_generic_plan;
SELECT s52_ok('= ANY ($n) over partitions: same rows',
              (SELECT string_agg(id::text, ',') FROM (SELECT * FROM s52_pk('{2,3,14}', 40)) x) =
              (SELECT string_agg(id::text, ',') FROM (SELECT id FROM multi WHERE dim_a_id = ANY ('{2,3,14}') ORDER BY recorded_on DESC, id DESC LIMIT 40) y));
PREPARE s52_pkp(int[], bigint) AS SELECT id FROM multi WHERE dim_a_id = ANY ($1) ORDER BY recorded_on DESC, id DESC LIMIT $2;
SELECT s52_ok('= ANY ($n): partitions of the array only', s52_plan($$EXECUTE s52_pkp('{2,3}', 40)$$) ~ 'Partitions Skipped: 3');
RESET plan_cache_mode;

UPDATE multi SET dim_a_id = 15 WHERE id % 97 = 0;
DELETE FROM multi WHERE id % 89 = 0;
INSERT INTO multi SELECT 20000 + g, 7, TIMESTAMPTZ '2026-02-01' + g * interval '1 minute', g FROM generate_series(1, 300) g;
SELECT s52_check('after updates, deletes and inserts: equality', $q$SELECT id FROM multi WHERE dim_a_id = 15 ORDER BY recorded_on DESC, id DESC LIMIT 40$q$,
                 'SiftAggV6TopN', 'Partitions Skipped: 3');
SELECT s52_check('after updates, deletes and inserts: descending', $q$SELECT id, dim_a_id FROM multi ORDER BY dim_a_id DESC, recorded_on DESC, id DESC LIMIT 60$q$, 'SiftAggV6TopN');
SELECT s52_check('after updates, deletes and inserts: ascending', $q$SELECT id, dim_a_id FROM multi ORDER BY dim_a_id, recorded_on, id LIMIT 60$q$, 'SiftAggV6TopN');
SELECT s52_check('after updates, deletes and inserts: the moved partition', $q$SELECT id FROM multi WHERE dim_a_id BETWEEN 6 AND 9 ORDER BY dim_a_id DESC, id LIMIT 80$q$, 'SiftAggV6TopN');

SELECT name || ' | ' || ok FROM checks ORDER BY name COLLATE "C";

-- the plans of the checks above as the server runs them (EXPLAIN ANALYZE; kept in results/)
SET plan_cache_mode = force_generic_plan;
SELECT E'== ' || n || E'\n' || s52_plan(q) FROM (VALUES
  ('equality on the lead key', $q$SELECT id FROM multi WHERE dim_a_id = 9 ORDER BY recorded_on DESC, id DESC LIMIT 10$q$),
  ('descending lead key (reverse frontier)', $q$SELECT id FROM multi ORDER BY dim_a_id DESC, recorded_on DESC, id DESC LIMIT 20$q$),
  ('= ANY ($n) over partitions', $$EXECUTE s52_pkp('{2,3}', 40)$$)
) v(n, q);
