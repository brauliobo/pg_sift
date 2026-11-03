-- The blob of a dense term (8 kB or more) is kept by the backend under the segment's relation file and generation: the second
-- query reads none of it again, and no query is ever answered with the blob of another segment, of another index of the same
-- layout, or of an index that was rebuilt. The budget (the limit segment_cache_kb) only decides what is kept: a backend that keeps
-- nothing and one that keeps less than its working set (blobs evicted, others in use) answer alike.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE FUNCTION body(g int) RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT 'dim_a' || g % 6 || ' dim_a' || (g + 1) % 6 || ' dim_a' || (g + 2) % 6 || ' w' || g % 50 $$;
CREATE TABLE docs_a (id int, body text, pad text);
CREATE TABLE docs_b (id int, body text, pad text);
INSERT INTO docs_a SELECT g, body(g), repeat('p', 700) FROM generate_series(1, 40960) g;
INSERT INTO docs_b SELECT 1000000 + g, body(g), repeat('p', 700) FROM generate_series(1, 40960) g;
CREATE INDEX docs_a_idx ON docs_a USING sift (body);
CREATE INDEX docs_b_idx ON docs_b USING sift (body);

-- the 25 best of a table by score, then by id up or down
CREATE FUNCTION top(tbl regclass, q text, up bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id FROM
		(SELECT id FROM %s WHERE body @@> $1 ORDER BY sift.score(ctid) DESC, CASE WHEN $2 THEN id ELSE -id END LIMIT 25) x) y', tbl) INTO r USING q, up;
	RETURN r;
END $$;
-- the bytes of blobs copied out of the index since the last reset, and the blobs found in the cache instead
CREATE FUNCTION copied() RETURNS bigint LANGUAGE sql AS $$ SELECT value FROM sift._test_read_stats() WHERE key = 'blob_bytes' $$;
CREATE FUNCTION found() RETURNS bigint LANGUAGE sql AS $$ SELECT value FROM sift._test_read_stats() WHERE key = 'blob_hits' $$;
CREATE TEMP TABLE qs (q text);
INSERT INTO qs VALUES ('dim_a0'), ('dim_a1 OR dim_a2'), ('dim_a3 dim_a4'), ('dim_a5 OR dim_a1'), ('dim_a0 OR dim_a1 OR dim_a2 OR dim_a3 OR dim_a4 OR dim_a5');

-- nothing kept: the reference (every pass copies all)
SET sift.test_limits = 'segment_cache_kb=0';
CREATE TEMP TABLE ref AS SELECT t, q, up, top(t, q, up) AS a FROM qs, unnest(ARRAY['docs_a', 'docs_b']::regclass[]) t, (VALUES (true), (false)) u (up);
SELECT found() AS found_nothing;
SELECT copied() AS all_bytes \gset
RESET sift.test_limits;

-- the first pass finds what its own queries repeat, the second one copies only the blobs under 8 kB, which are not kept
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT t, q, up FROM ref WHERE top(t, q, up) IS DISTINCT FROM a;
SELECT copied() AS first_pass \gset
SELECT :first_pass < :all_bytes AS first_pass_copies_less, found() > 0 AS first_pass_found;
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT t, q, up FROM ref WHERE top(t, q, up) IS DISTINCT FROM a;
SELECT copied() AS second_pass \gset
SELECT :second_pass < :first_pass AS second_pass_copies_less, found() > 0 AS found;
-- two indexes of one layout (same generation, offsets and sizes), each with its own rows
SELECT t, up, a FROM ref WHERE q = 'dim_a0' ORDER BY t, up;

-- a rebuilt index has a file of its own: the first pass copies again, with the same answers
REINDEX INDEX docs_a_idx;
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT t, q, up FROM ref WHERE t = 'docs_a'::regclass AND top(t, q, up) IS DISTINCT FROM a;
SELECT copied() AS rebuilt_first \gset
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT t, q, up FROM ref WHERE t = 'docs_a'::regclass AND top(t, q, up) IS DISTINCT FROM a;
SELECT :rebuilt_first > copied() AS copies_again;

-- a second segment of the index (same layout, another generation) is read from its own blobs
INSERT INTO docs_a SELECT 2000000 + g, body(g), repeat('p', 700) FROM generate_series(1, 40960) g;
SELECT sift.promote('docs_a_idx') > 0 AS promoted;
INSERT INTO docs_a SELECT 3000000 + g, body(g), repeat('p', 700) FROM generate_series(1, 40960) g;
SELECT sift.promote('docs_a_idx') > 0 AS promoted;
SELECT gen, kind, ndocs, sb_lo, sb_hi, postings_bytes FROM sift.segments('docs_a_idx'::regclass) ORDER BY gen;
SELECT q, top('docs_a', q, false) AS newest_first FROM qs WHERE q IN ('dim_a0', 'dim_a1 OR dim_a2');
CREATE TEMP TABLE ref_a AS SELECT q, up, top('docs_a', q, up) AS a FROM qs, (VALUES (true), (false)) u (up);

-- and a rewritten one (the merge, then VACUUM)
SELECT sift.merge('docs_a_idx', true) >= 0 AS merged;
SELECT q, up FROM ref_a WHERE top('docs_a', q, up) IS DISTINCT FROM a;
DELETE FROM docs_a WHERE id % 3 = 0;
VACUUM docs_a;
SELECT q, up, top('docs_a', q, up) AS vacuumed FROM ref_a WHERE q = 'dim_a0' ORDER BY up;
SELECT count(*) AS deleted_found FROM docs_a WHERE body @@> 'dim_a0' AND id % 3 = 0;

-- a budget below the working set (the blobs of the six terms are 9 kB each and are kept up to a quarter of the budget, so 4 fit; the shared blob
-- of 35 kB is not kept): blobs go out as others come in, none that a cursor holds (the query of the six terms finds the budget full of its own
-- and copies the others for itself), and the answers are those of the backend that keeps nothing
SET sift.test_limits = 'segment_cache_kb=40';
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT t, q, up FROM ref WHERE top(t, q, up) IS DISTINCT FROM a AND t = 'docs_b'::regclass;
SELECT t, q, up FROM ref WHERE top(t, q, up) IS DISTINCT FROM a AND t = 'docs_b'::regclass;
SELECT max(value) FILTER (WHERE key = 'blob_hits') > 0 AS found, max(value) FILTER (WHERE key = 'segcache_evicted') > 0 AS evicted,
	   max(value) FILTER (WHERE key = 'segcache_refused') > 0 AS refused, max(value) FILTER (WHERE key = 'segcache_bytes') <= 40 * 1024 AS kept_within_budget
FROM sift._test_read_stats();
RESET sift.test_limits;

DROP TABLE docs_a;
DROP TABLE docs_b;
DROP FUNCTION copied();
DROP FUNCTION found();
DROP FUNCTION top(regclass, text, bool);
DROP FUNCTION body(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
