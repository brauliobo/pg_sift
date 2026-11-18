-- The rows of the key ranges of a build are spooled as columnar chunks whatever they hold: NULL keys, a number the key leaf cannot hold, strings over 255 bytes (not for a dictionary) and
-- of 3,000, the elements of arrays, nested paths and values of the wrong type for a typed path (overflow). Every way a sift_col index is built (CREATE INDEX, REINDEX, CREATE INDEX CONCURRENTLY,
-- REINDEX CONCURRENTLY) writes columnar chunks for them, the index has the rows of the table and answers as the plain scan does, and sift.col_verify finds every row of the heap in it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.col_unit_rows = 1200;
SET sift.test_col_segment_kb = 2000;
SET sift.test_col_spool_share_kb = 5000;

CREATE TABLE sv (id int, k int, kf float8, s text, big text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO sv
SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE (i::bigint * 7919 % 3000)::int END,
	CASE WHEN i % 89 = 0 THEN NULL ELSE (i::bigint * 7919 % 3000) / 7.0 END,
	CASE WHEN i % 50 = 0 THEN repeat('long string ' || i % 7, 30) WHEN i % 11 = 0 THEN NULL ELSE 'str ' || (i % 400) END,
	CASE WHEN i % 600 = 0 THEN repeat('0123456789', 300) || i ELSE 'b' || i % 13 END,
	jsonb_build_object('n', CASE WHEN i % 31 = 0 THEN to_jsonb('not a number ' || i) ELSE to_jsonb(i % 100) END,
		'a', jsonb_build_object('b', jsonb_build_object('c', 'deep ' || i % 17)),
		'items', (SELECT jsonb_agg(jsonb_build_object('t', 'tag' || ((i + e) % 25), 'v', ((i + e) % 10) / 2.0)) FROM generate_series(1, i % 5) e))
FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) sv;

CREATE FUNCTION sv_same() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['SELECT count(*), count(k), sum(k), count(kf), count(s), count(DISTINCT s) FROM sv',
		'SELECT s, count(*), sum(k) FROM sv WHERE k BETWEEN 100 AND 400 GROUP BY 1 ORDER BY 1', 'SELECT count(*), max(length(big)) FROM sv WHERE k IS NULL OR kf IS NULL',
		'SELECT doc->''a''->''b''->>''c'' c, count(*), sum((doc->>''n'')::int) FILTER (WHERE jsonb_typeof(doc->''n'') = ''number'') FROM sv GROUP BY 1 ORDER BY 1',
		'SELECT length(big) l, count(*) FROM sv GROUP BY 1 ORDER BY 1'] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE 'SELECT string_agg(x::text, E''\n'') FROM (' || q || ') x' INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE 'SELECT string_agg(x::text, E''\n'') FROM (' || q || ') x' INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;

CREATE TABLE sv_runs (how text, columns bool, rows_spooled bool, same bool, verified bool);
CREATE FUNCTION sv_note(how text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.col_verify('sv_i', true);
	INSERT INTO sv_runs VALUES (how, current_setting('sift.test_col_spool_used_rows')::int > 0, current_setting('sift.test_col_spill_rows')::int = 24000, sv_same(), true);
END $$;

CREATE INDEX sv_i ON sv USING sift_col (id, k, kf, s, big, doc) WITH (key_cut = 'k', paths = 'doc.n int, doc.a.b.c text, doc.items[]{t text, v float8}');
SELECT sv_note('create index');
REINDEX INDEX sv_i;
SELECT sv_note('reindex');
DROP INDEX sv_i;
CREATE INDEX CONCURRENTLY sv_i ON sv USING sift_col (id, k, kf, s, big, doc) WITH (key_cut = 'k', paths = 'doc.n int, doc.a.b.c text, doc.items[]{t text, v float8}');
SELECT sv_note('create index concurrently');
REINDEX INDEX CONCURRENTLY sv_i;
SELECT sv_note('reindex concurrently');
DROP INDEX sv_i;
CREATE INDEX sv_i ON sv USING sift_col (id, k, kf, s, big, doc) WITH (key_cut = 'kf', paths = 'doc.n int, doc.a.b.c text, doc.items[]{t text, v float8}');
SELECT sv_note('float key');

SELECT how, columns AS chunks_of_columns, rows_spooled AS every_row_spooled, same AS same_rows_as_the_plain_scan, verified AS col_verify_finds_every_row FROM sv_runs;

RESET sift.test_col_segment_kb;
RESET sift.test_col_spool_share_kb;
DROP TABLE sv, sv_runs;
DROP FUNCTION sv_same();
DROP FUNCTION sv_note(text);
DROP EXTENSION pg_sift;
