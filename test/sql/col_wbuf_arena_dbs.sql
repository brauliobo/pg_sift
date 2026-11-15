-- sift.col_wbuf_ref: the arena is shared by the databases of the server. The slot of a buffer of a database that is dropped is given back by the first row that finds none, in another database,
-- and when every slot is a live buffer of another database the one written longest ago gives its slot: its rows that lost their values are read from the heap again.
-- The answers are the heap's throughout.
CREATE DATABASE arena_dbs_t TEMPLATE template0;
\c arena_dbs_t
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
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
CREATE FUNCTION arena_delta(OUT slots_used int, OUT stored int8, OUT not_stored int8) LANGUAGE sql AS $$
  SELECT s.slots_in_use, s.rows_stored - m.rows_stored, s.put_fallbacks - m.put_fallbacks FROM sift.col_arena_status() s, mark m $$;
CREATE PROCEDURE arena_mark() LANGUAGE sql AS $$ TRUNCATE mark; INSERT INTO mark SELECT * FROM sift.col_arena_status() $$;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
CREATE TABLE u (LIKE t INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE INDEX t_ix ON t USING sift_col (id, g, v);
CREATE INDEX u_ix ON u USING sift_col (id, g, v);
\c postgres
CREATE DATABASE arena_dbs_a TEMPLATE arena_dbs_t;
CREATE DATABASE arena_dbs_b TEMPLATE arena_dbs_t;
CREATE DATABASE arena_dbs_c TEMPLATE arena_dbs_t;

-- a and b hold a slot each with the values of 500 rows
\c arena_dbs_a
SET sift.async_maintenance = off;
SET sift.col_wbuf_ref = on;
INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 500) i;
\c arena_dbs_b
SET sift.async_maintenance = off;
SET sift.col_wbuf_ref = on;
INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 500) i;

-- a is dropped and every slot is taken: the first row of c has none, and a slot of a dead buffer is given back
\c postgres
DROP DATABASE arena_dbs_a WITH (FORCE);
\c arena_dbs_c
SET sift.async_maintenance = off;
SET sift.col_wbuf_ref = on;
SET max_parallel_workers_per_gather = 0;
SELECT sift._test_arena_slots((SELECT slots_in_use FROM sift.col_arena_status()));
CALL arena_mark();
INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 500) i;
INSERT INTO t SELECT 500 + i, i % 10, i FROM generate_series(1, 500) i;
SELECT stored >= 500 AS kept, not_stored < 500 AS first_rows_only FROM arena_delta();
SELECT same_as_heap('t') AS same_t;

-- c has no buffer now and every slot is taken, by live buffers of other databases: the oldest gives its slot, the rows of u find it after the first statement and the answers are right
SELECT sift.col_promote('t_ix') >= 0 AS promoted_t;
SELECT sift._test_arena_slots((SELECT slots_in_use FROM sift.col_arena_status()));
CALL arena_mark();
INSERT INTO u SELECT i, i % 10, i FROM generate_series(1, 500) i;
INSERT INTO u SELECT 500 + i, i % 10, i FROM generate_series(1, 500) i;
SELECT stored >= 500 AS kept, not_stored < 500 AS first_rows_only FROM arena_delta();
SELECT same_as_heap('u') AS same_u, same_as_heap('t') AS same_t;
SELECT sift._test_arena_slots(256);
\c arena_dbs_b
SET max_parallel_workers_per_gather = 0;
SELECT same_as_heap('t') AS same_b;
INSERT INTO t SELECT 500 + i, i % 10, i FROM generate_series(1, 100) i;
SELECT same_as_heap('t') AS same_b_after_more_rows;
SELECT sift.col_verify('t_ix', true);

\c postgres
DROP DATABASE arena_dbs_b WITH (FORCE);
DROP DATABASE arena_dbs_c WITH (FORCE);
DROP DATABASE arena_dbs_t WITH (FORCE);
