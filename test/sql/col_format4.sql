-- Column format minor 4 beside minor 3 (docs/COL-JSONB-31.md 5.2): an index reads the segments of both, whatever sift.col_format_minor is
-- when they are written, and a merge or a VACUUM rewrite writes the setting's minor. fmt lists the minors of the segments, oldest first.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;

CREATE FUNCTION fmt(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(format::text, ',' ORDER BY gen) FROM sift._test_col_formats(idx) $$;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_col = on; SET LOCAL sift.enable_agg = on;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO a;
	SET LOCAL sift.enable_col = off; SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

SHOW sift.col_format_minor;
SET sift.col_format_minor = 2;
SET sift.col_format_minor = 5;

CREATE TABLE f (id int, grp int, v numeric(10,2));
INSERT INTO f SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(1, 30000) g;
SET sift.col_format_minor = 3;
CREATE INDEX f_col ON f USING sift_col (id, grp, v);
SELECT fmt('f_col');
RESET sift.col_format_minor;
INSERT INTO f SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(30001, 60000) g;
SELECT sift.col_promote('f_col');
SELECT fmt('f_col');
SELECT same('SELECT grp, count(*), sum(v), avg(v) FROM f GROUP BY grp') AS mixed_agg,
       same('SELECT count(*), sum(v) FROM f WHERE id BETWEEN 25000 AND 35000') AS mixed_filter;
SELECT sift.col_verify('f_col', true);

-- the merge of both writes the setting's minor
SELECT sift.col_merge('f_col', true);
SELECT fmt('f_col');
SELECT same('SELECT grp, count(*), sum(v) FROM f GROUP BY grp') AS merged_agg;
SELECT sift.col_verify('f_col', true);

-- 3.0 compatibility: a merge under minor 3 writes minor 3 again
SET sift.col_format_minor = 3;
INSERT INTO f SELECT g, g % 9, 1 FROM generate_series(60001, 90000) g;
SELECT sift.col_promote('f_col');
SELECT sift.col_merge('f_col', true);
SELECT fmt('f_col');
SELECT sift.col_verify('f_col', true);
RESET sift.col_format_minor;

-- REINDEX writes minor 4
REINDEX INDEX f_col;
SELECT fmt('f_col');
SELECT same('SELECT grp, count(*), sum(v) FROM f GROUP BY grp') AS reindexed_agg;

DROP TABLE f;
DROP FUNCTION fmt(regclass), same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
