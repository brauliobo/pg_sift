-- The caches of a few entries that replace the oldest entry in turn (core/ring.h) count their slots in 32 unsigned bits: a signed count passed INT_MAX into undefined behavior and then
-- indexed the array of a memo before its start. sift._test_ring_walk takes slots of a ring across 2^31 and 2^32 for each size in use; sift._test_ring_start puts the count of every ring three slots
-- from the wrap of 32 bits, and the statements that fill each cache past its size follow: the answers are those of the plain statements, and every ring that took slots crossed the wrap.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

SELECT slots, sift._test_ring_walk(slots, 100) AS walks_across_the_wraps FROM unnest(ARRAY[1, 2, 4, 8, 16, 32]) slots;

SELECT sift._test_ring_start(4294967293);

CREATE FUNCTION scope_same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO b;
	RETURN a = b;
END $$;

-- the estimates of a BM25 index for twelve queries
CREATE TABLE bm (id int PRIMARY KEY, tenant int, body text);
INSERT INTO bm SELECT i, i % 40, 'word' || (i % 30) || ' filler' || i FROM generate_series(1, 3000) i;
CREATE INDEX bm_body ON bm USING sift (body);
ANALYZE bm;
SELECT format('EXPLAIN (COSTS OFF) SELECT count(*) FROM bm WHERE body @@> ''word%s'' AND tenant < 20', g) FROM generate_series(1, 12) g \gexec

-- the tests of a scan over twenty segments, and the order codes of their dictionaries
CREATE TABLE s (id int PRIMARY KEY, name text, v int) WITH (autovacuum_enabled = false);
CREATE INDEX s_c ON s USING sift_col (id, name, v);
SELECT q FROM generate_series(1, 20) g, LATERAL (VALUES
 (1, format('INSERT INTO s SELECT %1$s * 1000 + i, ''name'' || (i %% 50) || ''-'' || %1$s, i %% 7 FROM generate_series(1, 600) i', g)),
 (2, 'SELECT sift.col_promote(''s_c'')')) v(o, q) ORDER BY g, o \gexec
SELECT scope_same($$SELECT count(*), sum(v) FROM s WHERE name IN ('name1-3', 'name2-4', 'name5-5', 'name7-6', 'name9-7', 'name11-8', 'name13-9', 'name15-10', 'name17-11', 'name19-12', 'name21-13', 'name23-14', 'name25-15', 'name27-16', 'name29-17', 'name31-18', 'name33-19', 'name35-20')$$)
	AS scan_tests_equal_the_heap;
SELECT scope_same('SELECT name, v FROM s ORDER BY name, v, id LIMIT 5') AS order_codes_equal_the_heap;

-- every ring that handed out slots crossed the wrap of its count
SELECT site, wraps >= 1 AS crossed_the_wrap FROM sift._test_ring_sites() ORDER BY site;
SELECT sift._test_ring_start(0);

DROP FUNCTION scope_same(text);
DROP TABLE s, bm;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
