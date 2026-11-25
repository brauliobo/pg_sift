-- The first VACUUM after a CREATE INDEX CONCURRENTLY of a scoped index. The scope segments the validation writes say their string roots are flagged when the entity segments they come from do
-- (a build flags every segment it writes), so the VACUUM has no heap scan to make. When the segments do not say it (a build of a library that did not flag them: sift.test_no_roots) the
-- VACUUM scans every version of every row for a document that is a string, and the jsonb values it detoasts are freed with the row: the scan holds the same memory for 300 rows as for 1,500.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
SET sift.async_maintenance = off;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + m % 5 FROM generate_series(1, 300) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE docs (id int PRIMARY KEY, dim_a_id int, doc jsonb) WITH (autovacuum_enabled = false);

CREATE FUNCTION load_docs(first_id int, n int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO docs SELECT i, 1 + i % 300, jsonb_build_object('n', i, 'blob', (SELECT string_agg(md5(i::text || j::text), '') FROM generate_series(1, 200) j))
	FROM generate_series(first_id, first_id + n - 1) i $$;

CREATE FUNCTION flagged() RETURNS TABLE (segments bigint, with_roots bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT count(*), count(*) FILTER (WHERE f.roots) FROM sift._test_col_formats('docs_col') f;
END $$;

SELECT load_docs(1, 300);
SELECT bool_and(pg_column_size(doc) > 2000) AS documents_are_large FROM docs;

-- every segment of the index says it, the scope segments included, and the VACUUM that follows scans nothing
CREATE INDEX CONCURRENTLY docs_col ON docs USING sift_col (id, dim_a_id, doc) WITH (compression = 'none', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) > 1 AS entity_and_scope_segments FROM sift.col_segments('docs_col');
SELECT segments = with_roots AS every_segment_flagged FROM flagged();
SELECT count(*) AS reset FROM sift._test_mem_highwater(true) \gset
VACUUM docs;
SELECT count(*) AS scans FROM sift._test_mem_highwater() WHERE scope = 'col_roots_verify';
DROP INDEX docs_col;

-- segments that do not say it: the scan runs on every row, and holds the same bytes at both sizes
CREATE TABLE census (what text, rows int, peak bigint);
CREATE FUNCTION scan_peak(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	TRUNCATE docs;
	PERFORM load_docs(1, n);
	SET sift.test_no_roots = on;
	CREATE INDEX docs_col ON docs USING sift_col (id, dim_a_id, doc) WITH (compression = 'none');
	RESET sift.test_no_roots;
END $$;

SELECT scan_peak(300);
SELECT segments = 1 AS entity_segment, with_roots = 0 AS unflagged FROM flagged();
SELECT count(*) AS reset FROM sift._test_mem_highwater(true) \gset
VACUUM docs;
INSERT INTO census SELECT 'rows', 300, peak FROM sift._test_mem_highwater() WHERE scope = 'col_roots_verify';
SELECT segments = with_roots AS flagged_after_the_scan FROM flagged();
DROP INDEX docs_col;

SELECT scan_peak(1500);
SELECT count(*) AS reset FROM sift._test_mem_highwater(true) \gset
VACUUM docs;
INSERT INTO census SELECT 'rows', 1500, peak FROM sift._test_mem_highwater() WHERE scope = 'col_roots_verify';
SELECT segments = with_roots AS flagged_after_the_scan FROM flagged();

SELECT count(*) = 2 AS both_sizes_scanned, bool_and(peak > 0) AS scope_entered, max(peak) < 1024 * 1024 AS under_1mb,
	max(peak) - min(peak) < 64 * 1024 AS no_growth_with_rows FROM census;

DROP TABLE census, docs, tenant_scope;
DROP FUNCTION load_docs(int, int), flagged(), scan_peak(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
