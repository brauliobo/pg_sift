-- Column format minor 6: only a segment of an index with wide leaves (lanes, money, interval) is written as minor 6,
-- whatever sift.col_format_minor says; an index of the older types keeps writing the setting's minor, so 3.1 still opens it. fmt lists the
-- minors of the segments, oldest first.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET lc_monetary = 'C';

CREATE FUNCTION fmt(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(format::text, ',' ORDER BY gen) FROM sift.col_segments(idx) WHERE kind = 'immutable' $$;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_col = on; SET LOCAL sift.enable_agg = on;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO a;
	SET LOCAL sift.enable_col = off; SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

-- the setting's range is the one of 3.1: the wide minor is a property of the schema
SET sift.col_format_minor = 6;

CREATE TABLE g (id int, grp int, p numeric(18, 2), w numeric(30, 4), m money, iv interval, any_n numeric);
INSERT INTO g SELECT i, i % 9, i % 1000 / 8.0, i * 1000000000000.0001, (i % 977 || '.25')::money, make_interval(days => i % 40, mins => i % 7),
	   CASE WHEN i % 11 = 0 THEN 'NaN' ELSE i / 3.0 END FROM generate_series(1, 30000) i;

-- narrow only (numeric(18, 2) is one INT leaf): the setting's minor, as in 3.1
SET sift.col_format_minor = 3;
CREATE INDEX g_narrow ON g USING sift_col (id, grp, p);
SELECT fmt('g_narrow');
RESET sift.col_format_minor;
CREATE INDEX g_narrow4 ON g USING sift_col (id, grp, p);
SELECT fmt('g_narrow4');
-- each wide type alone, under either setting
SET sift.col_format_minor = 3;
CREATE INDEX g_w ON g USING sift_col (w);
CREATE INDEX g_m ON g USING sift_col (m);
CREATE INDEX g_iv ON g USING sift_col (iv);
CREATE INDEX g_n ON g USING sift_col (any_n);
SELECT fmt('g_w'), fmt('g_m'), fmt('g_iv'), fmt('g_n');
RESET sift.col_format_minor;
DROP INDEX g_w, g_m, g_iv, g_n;

-- a narrow index grows nothing of the wide minor; a mixed one is wide as a whole
CREATE INDEX g_all ON g USING sift_col (id, grp, p, w, m, iv, any_n);
SELECT fmt('g_all');
SELECT (SELECT string_agg(format::text, ',' ORDER BY gen) FROM sift._test_col_formats('g_all')) = fmt('g_all') AS hook_agrees;
SELECT sift.col_verify('g_all', true), sift.col_verify('g_narrow', true), sift.col_verify('g_narrow4', true);

-- later segments of the wide index are minor 6 too: the write buffer promoted, then the merge
INSERT INTO g SELECT i, i % 9, 1, i, '1.00'::money, interval '1 day', 1 FROM generate_series(30001, 60000) i;
SELECT sift.col_promote('g_all');
SELECT sift.col_promote('g_narrow');
SELECT fmt('g_all'), fmt('g_narrow');
SELECT sift.col_merge('g_all', true);
SELECT fmt('g_all');
SELECT sift.col_verify('g_all', true), sift.col_verify('g_narrow', true);
SET sift.col_format_minor = 3;
INSERT INTO g SELECT i, i % 9, 1, i, '2.00'::money, interval '2 days', 1 FROM generate_series(60001, 70000) i;
SELECT sift.col_promote('g_all');
SELECT sift.col_merge('g_all', true);
SELECT fmt('g_all');
RESET sift.col_format_minor;
REINDEX INDEX g_all;
SELECT fmt('g_all');
SELECT sift.col_verify('g_all', true);

-- the table's aggregates agree with the normal plan: the wide leaves are not read by the aggregate scan, the narrow ones in the same index are
SELECT same('SELECT grp, count(*), sum(p) FROM g GROUP BY grp') AS narrow_agg_in_wide_index,
       same('SELECT count(*), sum(w), min(m), max(m), max(iv) FROM g WHERE id > 1000') AS wide_agg;

-- the pre-aggregates of the lanes are not a cube of the value: the index has no wide dimension or measure
SELECT leaf, path, type FROM sift.col_leaves('g_all') ORDER BY leaf;

DROP TABLE g;
DROP FUNCTION fmt(regclass), same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
