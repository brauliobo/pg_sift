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

-- 'off' writes none
DROP INDEX kr_i;
CREATE INDEX kr_off ON kr USING sift_col (id, set_id, grp, val) WITH (write_buffer_limit = 4096, key_cut = 'set_id', key_columns = 'off');
SELECT array_agg(DISTINCT format ORDER BY format)::text AS minors, kr_units('SELECT count(*), sum(val) FROM kr WHERE id = 45050') >= 8 AS reads_every_unit FROM sift._test_col_formats('kr_off');

DROP TABLE kr;
DROP FUNCTION kr_rows(text, bool), kr_units(text), kr_option(text), kr_queries();
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
