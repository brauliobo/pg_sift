-- paths = auto for the array columns: a sample of the table says whether the arrays differ (their elements stand in place of the whole-array image) or repeat (the image stays)
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = notice;

CREATE TABLE aa (id int, uniq text[], rep text[], ids int4[]) WITH (autovacuum_enabled = false);
INSERT INTO aa SELECT i, ARRAY['a' || i, 'b' || i % 50, 'c' || i % 7], ARRAY['r' || i % 5, 's' || i % 3], ARRAY[i, i % 100]
FROM generate_series(1, 12000) i;
ANALYZE aa;
CREATE INDEX aa_col ON aa USING sift_col (id, uniq, rep, ids);
SELECT regexp_replace(reloptions::text, '(auto: )[^,}]*', '\1...') AS options FROM pg_class WHERE relname = 'aa_col';
SELECT leaf, path, type FROM sift.col_leaves('aa_col') ORDER BY leaf;
SELECT sift.col_verify('aa_col', true);
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SELECT count(*) FROM aa WHERE uniq @> ARRAY['b7', 'c0'];
SELECT count(*) FROM aa WHERE rep @> ARRAY['r2'];
SELECT count(*) FROM aa WHERE ids && ARRAY[5, 99999];
SELECT rep, count(*) FROM aa GROUP BY 1 ORDER BY 1 LIMIT 3;

-- a table too small for a sample gets neither
CREATE TABLE ab AS SELECT i AS id, ARRAY['a' || i] AS uniq FROM generate_series(1, 500) i;
CREATE INDEX ab_col ON ab USING sift_col (id, uniq);
SELECT leaf, path FROM sift.col_leaves('ab_col') ORDER BY leaf;
DROP TABLE aa, ab;
DROP EXTENSION pg_sift;
