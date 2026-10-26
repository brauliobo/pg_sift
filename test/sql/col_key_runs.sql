-- A unique int column read by key: the build lists it in key_columns = 'auto' when it is not in heap order or the index is key cut (a lookup by
-- it would read every unit otherwise), and a unit whose keys come in runs of consecutive values, ids taken in bursts per owner, keeps them as a list of runs
-- (set kind 5, col minor 23) where the offsets or the bitmap would not fit 3% of the unit. The answers are the heap's, the lookups read one unit of about ten.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '120s';

-- ids arrive in order, the owner changes every 100 rows: a unit of the key cut holds runs of 100 ids
CREATE TABLE kr (id int PRIMARY KEY, set_id int NOT NULL, grp int, val int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE kr ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kr SELECT g, 1 + (hashint4(g / 100) & 2147483647) % 400, (hashint4(g) & 2147483647) % 20, g % 1000, repeat('x', 150) FROM generate_series(1, 90000) g;
VACUUM (FREEZE, ANALYZE) kr;

CREATE FUNCTION kr_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION kr_units(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		n := coalesce(substring(l from '^ *Units: (\d+)')::int, n);
	END LOOP;
	RETURN n;
END $$;
CREATE FUNCTION kr_option(i text) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = i AND o LIKE 'key_columns=%'), 'not set') $$;
-- the queries of a key, a list of keys, a range, and a key with its owner
CREATE FUNCTION kr_queries() RETURNS SETOF text LANGUAGE sql AS $$ VALUES
	('SELECT count(*), sum(val) FROM kr WHERE id = 45050'), ('SELECT count(*), sum(val) FROM kr WHERE id IN (45050, 7001, 88999)'),
	('SELECT count(*), sum(val) FROM kr WHERE id BETWEEN 45050 AND 45120'), ('SELECT count(*), sum(val) FROM kr WHERE id = 45050 AND set_id = 1 + (hashint4(450) & 2147483647) % 400'),
	('SELECT count(*), sum(val) FROM kr WHERE id = 100000'), ('SELECT count(*), sum(val) FROM kr WHERE grp = 3 AND id < 2000') $$;

-- the default: id is chosen with the key cut (the NOTICE names it), the segments are minor 23
CREATE INDEX kr_i ON kr USING sift_col (id, set_id, grp, val) WITH (write_buffer_limit = 4096, key_cut = 'set_id');
SELECT kr_option('kr_i') AS option, array_agg(DISTINCT format ORDER BY format)::text AS minors FROM sift._test_col_formats('kr_i');
SELECT sift.col_verify('kr_i', true);
SELECT q, kr_rows(q, true) = kr_rows(q, false) AS same FROM kr_queries() q ORDER BY 1;
SELECT q, kr_units(q) <= 3 AS from_runs FROM kr_queries() q WHERE q NOT LIKE '%grp%' ORDER BY 1;

-- the summaries are decoded with checks: a cut, changed or damaged blob of the new kind is refused or equals the chunk
SELECT blobs > 0 AS has_blobs, intact = blobs AS intact_decodes, matching = blobs AS intact_match_chunks, cut_accepted = 0 AS cuts_refused, missed = 0 AS accepted_differ_from_chunk
FROM sift._test_col_set_damage('kr_i', 40);

-- a merge, a refresh and a rebuild keep the lists
INSERT INTO kr SELECT g, 1 + (hashint4(g / 100) & 2147483647) % 400, (hashint4(g) & 2147483647) % 20, g % 1000, repeat('x', 150) FROM generate_series(90001, 120000) g;
SELECT sift.col_merge('kr_i', true) >= 0 AS merged;
SELECT sift.col_verify('kr_i', true);
SELECT kr_option('kr_i') AS option, kr_units('SELECT count(*), sum(val) FROM kr WHERE id = 45050') <= 3 AS merged_from_runs;
REINDEX INDEX kr_i;
SELECT q, kr_rows(q, true) = kr_rows(q, false) AS same FROM kr_queries() q ORDER BY 1;
SELECT kr_option('kr_i') AS option, kr_units('SELECT count(*), sum(val) FROM kr WHERE id = 45050') <= 3 AS rebuilt_from_runs;

-- a table in heap order without a key cut: the ids of a unit are one run in the zone, and nothing is listed
CREATE INDEX kr_o ON kr USING sift_col (id, grp, val) WITH (write_buffer_limit = 4096);
SELECT kr_option('kr_o') AS option;
DROP INDEX kr_o;

-- a bigint id is listed the same way
CREATE TABLE kr8 (id bigint PRIMARY KEY, set_id int NOT NULL, val int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE kr8 ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kr8 SELECT g + 5000000000, 1 + (hashint4(g / 100) & 2147483647) % 400, g % 1000, repeat('x', 150) FROM generate_series(1, 90000) g;
VACUUM (FREEZE, ANALYZE) kr8;
CREATE INDEX kr8_i ON kr8 USING sift_col (id, set_id, val) WITH (write_buffer_limit = 4096, key_cut = 'set_id');
SELECT kr_option('kr8_i') AS option, array_agg(DISTINCT format ORDER BY format)::text AS minors FROM sift._test_col_formats('kr8_i');
SELECT sift.col_verify('kr8_i', true);
SELECT q, kr_rows(q, true) = kr_rows(q, false) AS same, kr_units(q) <= 3 AS from_runs FROM (VALUES
	('SELECT count(*), sum(val) FROM kr8 WHERE id = 5000045050'), ('SELECT count(*), sum(val) FROM kr8 WHERE id IN (5000045050, 5000007001, 5000088999)'),
	('SELECT count(*), sum(val) FROM kr8 WHERE id BETWEEN 5000045050 AND 5000045120'), ('SELECT count(*), sum(val) FROM kr8 WHERE id = 1')) t(q) ORDER BY 1;
SELECT blobs > 0 AS has_blobs, intact = blobs AS intact_decodes, matching = blobs AS intact_match_chunks, cut_accepted = 0 AS cuts_refused, missed = 0 AS accepted_differ_from_chunk
FROM sift._test_col_set_damage('kr8_i', 40);
DROP TABLE kr8;

-- a unique id scattered over the units (no runs, no list fits 3% of a unit): a bloom filter of what the share has left, a fraction of the units read for a key
CREATE TABLE kf (id int PRIMARY KEY, set_id int NOT NULL, val int, x1 float8, x2 float8, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE kf ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kf SELECT row_number() OVER (ORDER BY hashint4(g))::int, 1 + (hashint4(g / 100) & 2147483647) % 400, g % 1000, hashint4(g + 1) / 7.0, hashint4(g + 2) / 11.0, repeat('x', 150) FROM generate_series(1, 90000) g ORDER BY g;
VACUUM (FREEZE, ANALYZE) kf;
CREATE INDEX kf_i ON kf USING sift_col (id, set_id, val, x1, x2) WITH (write_buffer_limit = 4096, key_cut = 'set_id');
SELECT kr_option('kf_i') AS option;
SELECT sift.col_verify('kf_i', true);
SELECT q, kr_rows(q, true) = kr_rows(q, false) AS same, kr_units(q) AS units FROM (VALUES
	('SELECT count(*), sum(val) FROM kf WHERE id = 45050'), ('SELECT count(*), sum(val) FROM kf WHERE id IN (45050, 7001, 88999)'),
	('SELECT count(*), sum(val) FROM kf WHERE id = 100000')) t(q) ORDER BY 1;
SELECT bytes > 0 AS filters_take_bytes FROM sift.col_sections('kf_i') WHERE section = 'sets';
SELECT blobs > 0 AS has_blobs, bloom > 0 AS has_bloom, intact = blobs AS intact_decodes, matching = blobs AS intact_match_chunks, cut_accepted = 0 AS cuts_refused, missed = 0 AS accepted_differ_from_chunk
FROM sift._test_col_set_damage('kf_i', 40);
DROP TABLE kf;

-- a unit of a few keys that no list fits keeps a filter of at least 64 bits (a shorter one is refused by the decoder: the summary of a promotion of 14 rows is malformed)
CREATE TABLE kt (tenant_id int, a float8, b float8, c float8, d float8);
INSERT INTO kt SELECT n * 100003, hashint4(n) / 7.0, hashint4(n + 1) / 7.0, hashint4(n + 2) / 7.0, hashint4(n + 3) / 7.0 FROM generate_series(1, 14) n;
CREATE INDEX kt_i ON kt USING sift_col (tenant_id, a, b, c, d) WITH (key_columns = 'auto: tenant_id');
SELECT sift.col_verify('kt_i', true);
SELECT blobs > 0 AS has_blobs, bloom > 0 AS has_bloom, intact = blobs AS intact_decodes, matching = blobs AS intact_match_chunks, cut_accepted = 0 AS cuts_refused, missed = 0 AS accepted_differ_from_chunk
FROM sift._test_col_set_damage('kt_i', 5);
DROP TABLE kt;

-- 'off' writes none
DROP INDEX kr_i;
CREATE INDEX kr_off ON kr USING sift_col (id, set_id, grp, val) WITH (write_buffer_limit = 4096, key_cut = 'set_id', key_columns = 'off');
SELECT array_agg(DISTINCT format ORDER BY format)::text AS minors, kr_units('SELECT count(*), sum(val) FROM kr WHERE id = 45050') >= 8 AS reads_every_unit FROM sift._test_col_formats('kr_off');

DROP TABLE kr;
DROP FUNCTION kr_rows(text, bool), kr_units(text), kr_option(text), kr_queries();
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
