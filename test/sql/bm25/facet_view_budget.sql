-- The live views of dirty segments are part of the work_mem budget of sift.facets(): each view keeps only the query's pages
-- where its segment has live docs, so disjoint segments fit in the least work_mem, and segments that all hold every page
-- (one view of the whole query per segment) stop at the budget instead of growing past it
CREATE EXTENSION pg_sift;
CREATE TABLE d (id int PRIMARY KEY, body text, cat text, pad text) WITH (autovacuum_enabled = off, fillfactor = 50);
CREATE INDEX d_body ON d USING sift (body);
CREATE INDEX d_cat ON d USING sift (cat) WITH (analyzer = keyword, vacuum_segments = 128, dead_rewrite_pct = 100);

-- 40 promoted segments of a few pages each, every one dirty
SELECT format('INSERT INTO d SELECT i, %L, %L || i %% 3, %L FROM generate_series(%s, %s) i', 'common', 'c', '', b * 500 + 1, b * 500 + 500),
	'SELECT sift.promote(''d_cat'') >= 0 AS promoted' FROM generate_series(0, 39) b \gexec
DELETE FROM d WHERE id % 50 = 0;
VACUUM d;
SELECT count(*) AS segments, bool_and(dirty) AS dirty FROM sift.segments('d_cat') WHERE kind = 'immutable';
SET work_mem = '64kB';
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 3);
RESET work_mem;

-- every segment holds a live doc on each of the 1000 pages: a view per segment is the whole query
TRUNCATE d;
INSERT INTO d SELECT i, 'common', 'c' || i % 3, repeat('p', 900) FROM generate_series(1, 4000) i;
SELECT format('UPDATE d SET cat = %L || id %% 3 WHERE id %% 4 = %s', 'v' || r, r),
	'SELECT sift.promote(''d_cat'') >= 0 AS promoted' FROM generate_series(0, 3) r \gexec
DELETE FROM d WHERE id % 8 < 4;
VACUUM d;
SELECT count(*) AS segments, bool_and(dirty) AS dirty FROM sift.segments('d_cat') WHERE kind = 'immutable';
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 3);
SET work_mem = '128kB';
\set VERBOSITY sqlstate
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 3);
\set VERBOSITY default
SET work_mem = '1MB';
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 3);
RESET work_mem;

DROP TABLE d;
DROP EXTENSION pg_sift;
