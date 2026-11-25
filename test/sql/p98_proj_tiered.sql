-- A projection of a sift_table whose old parts are in the object store: the projection is built over the parts that left for the store as it is over the others, and answers as the table does.
-- Every check prints t.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SELECT current_setting('data_directory') || '/sift_tier_p98' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE ev (id int8, ts timestamp NOT NULL, tenant int, v int, pad text) USING sift_table;
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 7, i % 100, md5(i::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE) ev;
INSERT INTO ev SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 7, i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE) ev;
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', '30 days');
VACUUM ev;
SELECT count(*) > 0 AS parts_in_the_store FROM sift.tier_status;

SELECT sift.create_projection('ev', 'by_v', 'ORDER BY v, id');
SELECT sift.create_projection('ev', 'sum_v', 'GROUP BY tenant MEASURES count(*), sum(v)');
VACUUM ev;
SELECT name, state FROM sift.projection_list() WHERE relation = 'ev'::regclass ORDER BY name;
SELECT count(*) > 0 AS parts_still_in_the_store FROM sift.tier_status;

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '';'' ORDER BY r::text) FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT string_agg(r::text, '';'' ORDER BY r::text) FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;
SELECT same_rows('SELECT id, v FROM ev ORDER BY v, id LIMIT 100', 'SELECT id, v FROM ev ORDER BY v + 0, id LIMIT 100') AS sort_projection_answers;
SELECT same_rows('SELECT tenant, count(*), sum(v) FROM ev GROUP BY tenant', 'SELECT tenant, count(*), sum(v + 0) FROM ev GROUP BY tenant') AS aggregate_projection_answers;
SELECT count(*) AS problems FROM sift.check('ev');

DROP TABLE ev;
DROP FUNCTION same_rows(text, text);
DROP EXTENSION pg_sift;
