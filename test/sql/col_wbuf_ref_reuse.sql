-- sift.col_wbuf_ref: a heap address that VACUUM freed is taken by a later tuple. The record of the first tuple was flagged dead by the vacuum of the index, so the reader never
-- fetches from the address for it, and the record of the new tuple (another xmin, the same TID) gets its own values: the answers are the heap's, before and after the promotion.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.col_auto_merge = off;
SET max_parallel_workers_per_gather = 0;
SET check_function_bodies = off;

CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS $$
  SELECT md5(concat_ws('|',
    (SELECT count(*) || ',' || sum(v) FROM r),
    (SELECT count(*) || ',' || sum(v) FROM r WHERE g = 3),
    (SELECT string_agg(id::text, ',' ORDER BY v DESC, id) FROM (SELECT id, v FROM r ORDER BY v DESC, id LIMIT 30) x),
    (SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT g, count(*), sum(v) FROM r GROUP BY g) x))) $$;

CREATE FUNCTION same_as_heap() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE native text; sift text;
BEGIN
  PERFORM set_config('sift.enable_agg', 'off', true);
  native := answers();
  PERFORM set_config('sift.enable_agg', 'on', true);
  sift := answers();
  RETURN native = sift;
END $$;

CREATE TABLE r (id int PRIMARY KEY, g int NOT NULL, v int, pad text) WITH (autovacuum_enabled = false);
CREATE INDEX r_ix ON r USING sift_col (id, g, v);
SET sift.col_wbuf_ref = on;
INSERT INTO r SELECT i, i % 10, i, 'x' FROM generate_series(1, 2000) i;
CREATE TEMP TABLE freed AS SELECT ctid AS tid FROM r WHERE id % 4 = 0;
DELETE FROM r WHERE id % 4 = 0;
VACUUM r;
SELECT same_as_heap() AS same_after_vacuum;

-- new tuples with other values at the freed addresses (the table is one run of pages, the free line pointers are taken first)
INSERT INTO r SELECT 5000 + i, (i * 7) % 10, 100000 + i, 'n' FROM generate_series(1, 500) i;
SELECT count(*) > 0 AS addresses_reused FROM r WHERE ctid IN (SELECT tid FROM freed);
SELECT same_as_heap() AS same_reused;
SELECT sift.col_verify('r_ix', true);
SELECT sift.col_promote('r_ix') >= 0 AS promoted;
SELECT same_as_heap() AS same_promoted;
SELECT sift.col_verify('r_ix', true);

-- the same address freed and taken again in the buffer that the promotion opened
DELETE FROM r WHERE id > 5000 AND id % 2 = 0;
VACUUM r;
INSERT INTO r SELECT 9000 + i, i % 10, 200000 + i, 'm' FROM generate_series(1, 300) i;
SELECT same_as_heap() AS same_reused_again;
SELECT sift.col_verify('r_ix', true);
SELECT sift.col_promote('r_ix') >= 0 AS promoted_again;
SELECT same_as_heap() AS same_promoted_again;
SELECT sift.col_verify('r_ix', true);

DROP FUNCTION same_as_heap();
DROP FUNCTION answers();
DROP TABLE r;
DROP EXTENSION pg_sift CASCADE;
