-- An insert whose value the index cannot store (a string in a float8 path) or a JSON null in a leaf the directory did not flag yet flags the leaf: that reads the
-- catalog, which reloads the relcache entry of the index and frees its cached state. A server built with assertions reloads the entry at every such point
-- (debug_discard_caches); the other servers run the test as it is: the answers are the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE ins (id int, grp int, doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO ins SELECT i, i % 4, jsonb_build_object('v', i + 0.5, 'w', 'a') FROM generate_series(1, 20) i;
CREATE INDEX ins_col ON ins USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8, doc.w astext', write_buffer_limit = 4096);
SELECT coalesce((SELECT max_val::int > 0 FROM pg_settings WHERE name = 'debug_discard_caches'), false) AS discard \gset
\if :discard
SET debug_discard_caches = 1;
\endif
INSERT INTO ins SELECT i, i % 4, jsonb_build_object('v', 'text ' || i, 'w', 'b') FROM generate_series(21, 30) i;
INSERT INTO ins SELECT i, i % 4, jsonb_build_object('v', i + 0.5, 'w', NULL) FROM generate_series(31, 40) i;
INSERT INTO ins SELECT i, i % 4, jsonb_build_object('v', i + 0.5, 'w', 'c') FROM generate_series(41, 50) i;
\if :discard
RESET debug_discard_caches;
\endif
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.enable_agg = on;
SELECT grp, count(*), sum((doc ->> 'v')::float8) FILTER (WHERE jsonb_typeof(doc -> 'v') = 'number') FROM ins GROUP BY grp ORDER BY grp;
SET sift.enable_agg = off;
SELECT grp, count(*), sum((doc ->> 'v')::float8) FILTER (WHERE jsonb_typeof(doc -> 'v') = 'number') FROM ins GROUP BY grp ORDER BY grp;
RESET sift.enable_agg;
SELECT sift.col_verify('ins_col', true);
DROP TABLE ins;
DROP EXTENSION pg_sift;
