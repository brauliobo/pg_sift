-- The pool of the write buffer arena (col/v1/wbuf/arena.c) takes a segment of 1 MB when no page is free and gives back to the system every idle segment at its end, down to none,
-- when the buffers that held its pages are promoted (the pages of promoted buffers are idle memory in /dev/shm otherwise). The rows keep their answers throughout.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE s (id int PRIMARY KEY, g int NOT NULL, note text NOT NULL) WITH (autovacuum_enabled = false);
CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS
  $$ SELECT count(*) || ',' || sum(g) || ',' || sum(length(note)) FROM s $$;
CREATE INDEX s_ix ON s USING sift_col (id, g, note);
SELECT put_fallbacks AS fallbacks_before FROM sift.col_arena_status() \gset

-- a buffer of 3,000 rows of 1,280 bytes holds about four segments of pages
INSERT INTO s SELECT i, i % 10, repeat(md5(i::text), 40) FROM generate_series(1, 3000) i;
SELECT segments >= 4 AS pool_grew, put_fallbacks = :fallbacks_before AS none_left_out FROM sift.col_arena_status();
SELECT answers() AS buffered \gset
SELECT :'buffered' AS buffered;

-- the promotion gives every page back: every segment is given to the system in one go
SELECT sift.col_promote('s_ix') >= 0 AS promoted;
SELECT segments AS segments_after FROM sift.col_arena_status() \gset
SELECT :segments_after = 0 AS pool_shrunk;
SELECT answers() = :'buffered' AS same_promoted;

-- and the pool grows again when a buffer asks for pages
INSERT INTO s SELECT 10000 + i, i % 10, repeat(md5(i::text), 40) FROM generate_series(1, 3000) i;
SELECT segments > :segments_after AS pool_grew_again FROM sift.col_arena_status();
SELECT sift.col_promote('s_ix') >= 0 AS promoted_again;
SELECT segments = 0 AS pool_shrunk_again FROM sift.col_arena_status();
SELECT sift.col_verify('s_ix', true);

DROP FUNCTION answers();
DROP TABLE s;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
