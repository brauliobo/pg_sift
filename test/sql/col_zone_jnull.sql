-- The zone flag of a JSON null (SIFT_COLZ_JNULL) is a column format minor 6 flag: a segment written for readers of 3.1 (sift.col_format_minor 4, or 5 with
-- wide leaves) must not carry it, because the zone check of 3.1 compares the stored flags exactly and would call the index corrupt.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE zj (id int, doc jsonb, amt money) WITH (autovacuum_enabled = off);
INSERT INTO zj SELECT g, CASE g % 4 WHEN 0 THEN '{"a": null}'::jsonb WHEN 1 THEN '{"a": 1}'::jsonb WHEN 2 THEN '{"a": "x"}'::jsonb ELSE '{}'::jsonb END, g::numeric::money
FROM generate_series(1, 2000) g;

SET sift.col_format_minor = 3;
CREATE INDEX zj_m3 ON zj USING sift_col (id, doc) WITH (paths = 'doc.a astext');
SET sift.col_format_minor = 4;
CREATE INDEX zj_m4 ON zj USING sift_col (id, doc) WITH (paths = 'doc.a astext');
CREATE INDEX zj_m5 ON zj USING sift_col (id, doc, amt) WITH (paths = 'doc.a astext');
RESET sift.col_format_minor;
CREATE INDEX zj_m6 ON zj USING sift_col (id, doc) WITH (paths = 'doc.a astext');

SELECT i AS idx, array_agg(DISTINCT format) AS minors, (f).units > 0 AS has_units, (f).jnull_units > 0 AS jnull_flag
FROM (VALUES ('zj_m3'), ('zj_m4'), ('zj_m5'), ('zj_m6')) x(i),
     LATERAL (SELECT sift._test_col_zone_flags(i::regclass) AS f) z, LATERAL sift._test_col_formats(i::regclass)
GROUP BY i, f ORDER BY 1;
SELECT sift.col_verify('zj_m3', true), sift.col_verify('zj_m4', true), sift.col_verify('zj_m5', true), sift.col_verify('zj_m6', true);

DROP TABLE zj;
DROP EXTENSION pg_sift_test CASCADE;
