-- sift.col_wbuf_ref: the arena has a few slots, one for each buffer in use, and a row that finds none is written with its values. The buffer that was written longest ago is asked back by the
-- row that found no slot (it is sealed and promoted as any buffer is), its slot is free when it is promoted and the next rows of the other buffer are kept in the arena. The slot of an index
-- that is dropped is given back too. The answers are the heap's throughout.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.col_auto_merge = off;
SET max_parallel_workers_per_gather = 0;
SET check_function_bodies = off;

CREATE FUNCTION same_as_heap(t regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE native text; sift text; q text := format('SELECT md5(concat_ws(''|'', (SELECT count(*) || '','' || sum(v) FROM %1$s), (SELECT string_agg(x::text, '','' ORDER BY x::text) FROM (SELECT g, count(*), sum(v) FROM %1$s GROUP BY g) x)))', t);
BEGIN
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE q INTO native;
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE q INTO sift;
  RETURN native = sift;
END $$;

CREATE TABLE mark AS SELECT * FROM sift.col_arena_status();
CREATE FUNCTION arena_delta(OUT slots_used int, OUT stored int8, OUT not_stored int8, OUT rebuilt int8, OUT forced int8) LANGUAGE sql AS $$
  SELECT s.slots_in_use, s.rows_stored - m.rows_stored, s.put_fallbacks - m.put_fallbacks, s.rows_rebuilt - m.rows_rebuilt, s.seals_forced - m.seals_forced FROM sift.col_arena_status() s, mark m $$;
CREATE PROCEDURE arena_mark() LANGUAGE sql AS $$ TRUNCATE mark; INSERT INTO mark SELECT * FROM sift.col_arena_status() $$;
CREATE FUNCTION kinds(ix regclass) RETURNS text LANGUAGE sql AS $$
  SELECT coalesce(string_agg(kind || ' ' || rows, ', ' ORDER BY kind), 'none') FROM sift.col_segments(ix) WHERE kind <> 'tombstones' $$;

CREATE TABLE t1 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
CREATE TABLE t2 (LIKE t1 INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE t3 (LIKE t1 INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE INDEX t1_ix ON t1 USING sift_col (id, g, v);
CREATE INDEX t2_ix ON t2 USING sift_col (id, g, v);
CREATE INDEX t3_ix ON t3 USING sift_col (id, g, v);
SET sift.col_wbuf_ref = on;

-- two slots: the first two buffers take them, the third has none
SELECT sift._test_arena_gc();
SELECT sift._test_arena_slots((SELECT slots_in_use FROM sift.col_arena_status()) + 2);
CALL arena_mark();
INSERT INTO t1 SELECT i, i % 10, i FROM generate_series(1, 1000) i;
INSERT INTO t2 SELECT i, i % 10, i FROM generate_series(1, 1000) i;
SELECT stored = 2000 AS kept, not_stored = 0 AS none_left_out FROM arena_delta();
INSERT INTO t3 SELECT i, i % 10, i FROM generate_series(1, 1000) i;
SELECT not_stored >= 1 AS third_has_no_slot, forced = 1 AS oldest_asked_back FROM arena_delta();
SELECT same_as_heap('t3') AS same_t3;

-- the row that found no slot asked the oldest buffer back: t1 is sealed, with the rows it had
SELECT kinds('t1_ix') AS t1, kinds('t2_ix') AS t2, kinds('t3_ix') AS t3;
SELECT same_as_heap('t1') AS same_t1, same_as_heap('t2') AS same_t2;
SELECT sift.col_promote('t1_ix') >= 0 AS promoted_t1;
SELECT same_as_heap('t1') AS same_t1_promoted;

-- its slot is free: the next rows of t3 go to the arena
CALL arena_mark();
INSERT INTO t3 SELECT 1000 + i, i % 10, i FROM generate_series(1, 500) i;
SELECT stored >= 1 AS kept_after_give_back FROM arena_delta();
SELECT same_as_heap('t3') AS same_t3_mixed;
SELECT sift.col_verify('t3_ix', true);
SELECT sift.col_promote('t3_ix') >= 0 AS promoted_t3;
SELECT same_as_heap('t3') AS same_t3_promoted;

-- an index that is dropped gives its slot back
CREATE TEMP TABLE before AS SELECT slots_in_use FROM sift.col_arena_status();
DROP INDEX t2_ix;
SELECT sift._test_arena_gc();
SELECT (SELECT slots_in_use FROM sift.col_arena_status()) < (SELECT slots_in_use FROM before) AS slot_given_back;
SELECT same_as_heap('t1') AS same_t1_after_drop;
SELECT sift._test_arena_slots(16);

DROP FUNCTION same_as_heap(regclass);
DROP FUNCTION arena_delta();
DROP PROCEDURE arena_mark();
DROP FUNCTION kinds(regclass);
DROP TABLE t1, t2, t3, mark;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
