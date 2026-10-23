-- The cached state of an index is freed when its relcache entry is reloaded, and detoasting a value (which locks the toast table) accepts the invalidation
-- that reloads it. Building the index row of a toasted text column, and the aggregate scan reading again the rows with a string in a float8 path, died on a
-- server built with assertions that reloads the entry at every such point (debug_discard_caches); the other servers run it as it is: the answers are the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE blobs (id int PRIMARY KEY, grp int, body text, doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO blobs SELECT i, i % 5, (SELECT string_agg(md5(i::text || j), ' ') FROM generate_series(1, 200) j),
	jsonb_build_object('v', CASE WHEN i % 6 = 0 THEN to_jsonb('8.5'::text) ELSE to_jsonb((i % 17) + 0.5) END) FROM generate_series(1, 30) i;
CREATE INDEX blobs_col ON blobs USING sift_col (id, grp, body, doc) WITH (paths = 'doc.v float8', write_buffer_limit = 4096);
ANALYZE blobs;
SELECT coalesce((SELECT max_val::int > 0 FROM pg_settings WHERE name = 'debug_discard_caches'), false) AS discard \gset
\if :discard
SET debug_discard_caches = 1;
\endif
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(body) IS NOT NULL) AS toasted FROM blobs;
INSERT INTO blobs SELECT i, i % 5, (SELECT string_agg(md5(i::text || j), ' ') FROM generate_series(1, 200) j), jsonb_build_object('v', 1.5)
FROM generate_series(31, 40) i;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.enable_agg = on;
SELECT grp, sum((doc ->> 'v')::float8), count(*) FROM blobs GROUP BY grp ORDER BY grp;
SET sift.enable_agg = off;
SELECT grp, sum((doc ->> 'v')::float8), count(*) FROM blobs GROUP BY grp ORDER BY grp;
RESET sift.enable_agg;
\if :discard
RESET debug_discard_caches;
\endif
DROP TABLE blobs;
DROP EXTENSION pg_sift;
