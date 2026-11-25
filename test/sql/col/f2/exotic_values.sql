-- Values a zone or a numeric key cannot hold, answered as the heap answers them: a numeric(p, s) column refuses an infinity before the table sees it (PostgreSQL does), a numeric without a precision
-- keeps NaN and both infinities, and a text that a granule holds as a date or a timestamp (an astext leaf of a jsonb path) is found under a nondeterministic collation by the spellings that collation
-- equates with it ('t' and 'z' for 'T' and 'Z'), not rules out for lacking the canonical spelling.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE COLLATION exotic_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);

CREATE TABLE amounts (id int, fixed numeric(10, 2), free numeric) USING sift_table;
INSERT INTO amounts VALUES (1, 'Infinity', 1);
INSERT INTO amounts VALUES (2, 'NaN', 'Infinity'), (3, 1.25, '-Infinity'), (4, NULL, 'NaN'), (5, 2.50, 3);
SELECT id, fixed, free FROM amounts ORDER BY id;
SELECT count(fixed), count(free), min(free), max(free), sum(fixed) FROM amounts;
CREATE TABLE amounts_heap (id int, fixed numeric(10, 2), free numeric);
INSERT INTO amounts_heap SELECT id, fixed, free FROM amounts;
CREATE INDEX amounts_heap_col ON amounts_heap USING sift_col (id, fixed, free);
SELECT count(fixed), count(free), min(free), max(free), sum(fixed) FROM amounts_heap;

CREATE TABLE stamps (id int, doc jsonb);
INSERT INTO stamps SELECT g, jsonb_build_object('ts', to_char(timestamp '2026-01-01' + g * interval '1 minute', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')) FROM generate_series(1, 6000) g;
CREATE INDEX stamps_col ON stamps USING sift_col (id, doc) WITH (paths = 'doc.ts astext');
VACUUM (ANALYZE) stamps;
SELECT count(*) AS canonical_spelling FROM stamps WHERE doc->>'ts' = '2026-01-01T01:40:00Z' COLLATE exotic_ci;
SELECT count(*) AS other_case FROM stamps WHERE doc->>'ts' = '2026-01-01t01:40:00z' COLLATE exotic_ci;
SELECT count(*) AS other_case_in_list FROM stamps WHERE (doc->>'ts') COLLATE exotic_ci IN ('2026-01-01t01:40:00z', 'none');
SELECT count(*) AS other_case_with_key FROM stamps WHERE id > 0 AND doc->>'ts' = '2026-01-01t01:40:00z' COLLATE exotic_ci;

DROP TABLE amounts, amounts_heap, stamps;
DROP COLLATION exotic_ci;
DROP EXTENSION pg_sift;
