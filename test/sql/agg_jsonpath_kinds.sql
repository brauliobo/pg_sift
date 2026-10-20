-- jsonpath `!=`, `!()`, a null literal and `@>` with a JSON null on the members of an array of objects (docs/COL-SHRED.md "jsonpath !="): the NULL
-- slots of an element leaf are JSON nulls or missing keys, which only the kinds of column format minor 6 tell apart. A segment of minor 3 has
-- none, so a query that needs them keeps the normal plan (it used to be planned and to raise the REINDEX error), a query that does not (`==`)
-- and a segment with no NULL slot are not in the way. Every query runs with and without the pushdown: `same` is a SiftAgg plan with the rows
-- of the normal plan, `not pushed` the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;

CREATE FUNCTION fmt(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(format::text, ',' ORDER BY gen) FROM sift._test_col_formats(idx) $$;

CREATE FUNCTION t_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION t_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE FUNCTION t_outcome(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE h text;
BEGIN
	RETURN 'rows ' || md5(t_rows(q, push));
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS h = PG_EXCEPTION_HINT;
	RETURN 'error ' || SQLSTATE || CASE WHEN SQLSTATE = '0A000' THEN ' (' || h || ')' ELSE '' END;
END $$;

-- 'same': pushed and the rows of the normal plan; 'not pushed'; else the two outcomes (an error of the pushdown is a failure)
CREATE FUNCTION t_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := t_outcome(q, true); b text := t_outcome(q, false);
BEGIN
	IF a <> b THEN RETURN 'pushed: ' || a || '; normal: ' || b; END IF;
	RETURN CASE WHEN t_pushed(q) THEN 'same' ELSE 'not pushed' END;
END $$;

-- members s (text), n (int4), b (bool): values, a JSON null and a missing key, in arrays of up to three elements, and some NULL documents
CREATE FUNCTION doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN i % 13 = 0 THEN NULL ELSE coalesce(jsonb_agg(
		(ARRAY['{"s": "gift"}', '{"s": ""}', '{"s": "toy"}', '{"s": null}', '{}'])[1 + (i * 3 + j * 7) % 5]::jsonb
		|| (ARRAY['{"n": 1}', '{"n": 3}', '{"n": -2}', '{"n": null}', '{}'])[1 + (i * 3 + j * 11) % 5]::jsonb
		|| (ARRAY['{"b": true}', '{"b": false}', '{"b": null}', '{}'])[1 + (i + j * 5) % 4]::jsonb ORDER BY j), '[]') END
	FROM generate_series(1, i % 4) j $$;
-- the same documents without a JSON null or a missing key: no NULL slot in any leaf
CREATE FUNCTION doc_full(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT jsonb_build_array(jsonb_build_object('s', (ARRAY['gift', '', 'toy'])[1 + i % 3], 'n', i % 5 - 2, 'b', i % 2 = 0)) $$;

CREATE TABLE q (n serial, q text);
INSERT INTO q (q) VALUES
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (@.s != "gift")'')) FROM k GROUP BY grp'),
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (@.n != 3)'')), count(*) FILTER (WHERE doc @? ''$[*] ? (@.b != true)'') FROM k GROUP BY grp'),
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (!(@.s == "toy"))'')) FROM k GROUP BY grp'),
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (@.s == null)'')), count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (@.n != null)'')) FROM k GROUP BY grp'),
	('SELECT grp, count(*) FILTER (WHERE doc @> ''[{"s": null}]'') FROM k GROUP BY grp'),
	('SELECT count(*) FROM k WHERE jsonb_path_exists(doc, ''$[*] ? (@.s != "gift" && @.n != 1)'')'),
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (@.s != $x)'', ''{"x":"gift"}'')) FROM k GROUP BY grp'),
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''strict $[*] ? (@.n != 3)'')) FROM k GROUP BY grp'),
	-- no kinds needed: a comparison that is false for a NULL slot
	('SELECT grp, count(*) FILTER (WHERE jsonb_path_exists(doc, ''$[*] ? (@.s == "gift")'')), count(*) FILTER (WHERE doc @> ''[{"n": 3}]'') FROM k GROUP BY grp');

-- Prove the generator exercises both sides and distinguishes JSON null from a missing key.
SELECT count(*) FILTER (WHERE e->'s' = '"gift"'::jsonb) > 0 AS gift,
       count(*) FILTER (WHERE e->'s' = 'null'::jsonb) > 0 AS json_null,
       count(*) FILTER (WHERE NOT e ? 's') > 0 AS missing
FROM generate_series(1, 100) i CROSS JOIN LATERAL jsonb_array_elements(doc(i)) e;

-- written as minor 3 (a 3.0 index): the NULL slots cannot be told apart
CREATE TABLE k (id int, grp int, doc jsonb);
INSERT INTO k SELECT i, i % 7, doc(i) FROM generate_series(1, 8000) i;
SET sift.col_format_minor = 3;
CREATE INDEX k_col ON k USING sift_col (id, grp, doc) WITH (paths = 'doc[]{s text, n int4, b bool}');
RESET sift.col_format_minor;
VACUUM ANALYZE k;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;

-- minor 6 units beside it: the rows of the minor 3 unit still decide
INSERT INTO k SELECT i, i % 7, doc(i + 5) FROM generate_series(8001, 12000) i;
SELECT sift.col_promote('k_col');
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;

-- a merge that has an input without kinds writes minor 4, which does not claim them (the merged rows used to answer wrongly); REINDEX
-- from the heap writes minor 6, under minor 3 as 3
SELECT sift.col_merge('k_col', true);
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
SELECT sift.col_verify('k_col', true);
SET sift.col_format_minor = 3;
REINDEX INDEX k_col;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
RESET sift.col_format_minor;
REINDEX INDEX k_col;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;

-- the released 3.1 format is readable, but does not prove complete kinds; REINDEX restores that guarantee
SET sift.col_format_minor = 4;
REINDEX INDEX k_col;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
RESET sift.col_format_minor;
REINDEX INDEX k_col;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;

-- rows of the write buffer beside the segments
INSERT INTO k SELECT i, i % 7, doc(i + 9) FROM generate_series(12001, 13000) i;
SELECT n, t_check(q) FROM q ORDER BY n;
SELECT sift.col_verify('k_col', true);
DROP TABLE k;

-- a minor 3 index with no NULL slot in any leaf is not in the way
CREATE TABLE k (id int, grp int, doc jsonb);
INSERT INTO k SELECT i, i % 7, doc_full(i) FROM generate_series(1, 8000) i;
SET sift.col_format_minor = 3;
CREATE INDEX k_col ON k USING sift_col (id, grp, doc) WITH (paths = 'doc[]{s text, n int4, b bool}');
RESET sift.col_format_minor;
VACUUM ANALYZE k;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
DROP TABLE k;

-- a sift_table with declared paths: a group written under minor 3 keeps the plan with the documents, one of minor 6 is a SiftAgg plan
CREATE FUNCTION tfmt(t regclass) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(format::text, ',' ORDER BY format) FROM sift.table_segments(t) WHERE kind = 'columns' $$;
CREATE TABLE kt (id int, grp int, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('kt', 'doc', 'doc[]{s text, n int4, b bool}');
SET sift.col_format_minor = 3;
INSERT INTO kt SELECT i, i % 7, doc(i) FROM generate_series(1, 8000) i;
VACUUM kt;
RESET sift.col_format_minor;
SELECT tfmt('kt');
SELECT n, t_check(replace(q, ' FROM k', ' FROM kt')) FROM q ORDER BY n;
INSERT INTO kt SELECT i, i % 7, doc(i + 5) FROM generate_series(8001, 12000) i;
-- Space compaction copies the legacy group without upgrading its format; uncertain kinds retain the document plan until VACUUM FULL.
VACUUM kt;
SELECT tfmt('kt');
SELECT n, t_check(replace(q, ' FROM k', ' FROM kt')) FROM q ORDER BY n;
VACUUM (FULL) kt;
SELECT tfmt('kt');
SELECT n, t_check(replace(q, ' FROM k', ' FROM kt')) FROM q ORDER BY n;
INSERT INTO kt SELECT i, i % 7, doc(i + 9) FROM generate_series(12001, 13000) i;
SELECT n, t_check(replace(q, ' FROM k', ' FROM kt')) FROM q ORDER BY n;
SET sift.col_format_minor = 4;
VACUUM FULL kt;
SELECT tfmt('kt');
SELECT n, t_check(replace(q, ' FROM k', ' FROM kt')) FROM q ORDER BY n;
RESET sift.col_format_minor;
VACUUM FULL kt;
SELECT tfmt('kt');
SELECT n, t_check(replace(q, ' FROM k', ' FROM kt')) FROM q ORDER BY n;
DROP TABLE kt;

-- Partition parents must combine uncertainty from every child, including a clean minor 6 child beside a legacy one.
CREATE TABLE k (id int, grp int, doc jsonb) PARTITION BY RANGE (id);
CREATE TABLE k_old PARTITION OF k FOR VALUES FROM (0) TO (4001);
CREATE TABLE k_new PARTITION OF k FOR VALUES FROM (4001) TO (9000);
INSERT INTO k SELECT i, i % 7, doc(i) FROM generate_series(1, 8000) i;
SET sift.col_format_minor = 4;
CREATE INDEX k_old_col ON k_old USING sift_col (id, grp, doc) WITH (paths = 'doc[]{s text, n int4, b bool}');
RESET sift.col_format_minor;
CREATE INDEX k_new_col ON k_new USING sift_col (id, grp, doc) WITH (paths = 'doc[]{s text, n int4, b bool}');
VACUUM ANALYZE k_old;
VACUUM ANALYZE k_new;
ANALYZE k;
SELECT n, t_check(q) FROM q ORDER BY n;
REINDEX INDEX k_old_col;
SELECT n, t_check(q) FROM q ORDER BY n;
DROP TABLE k;
CREATE TABLE k (id int, doc jsonb) PARTITION BY RANGE (id);
CREATE TABLE k_old PARTITION OF k FOR VALUES FROM (0) TO (4001);
CREATE TABLE k_new PARTITION OF k FOR VALUES FROM (4001) TO (9000);
INSERT INTO k SELECT i, CASE i % 5 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{"a":null}'::jsonb WHEN 2 THEN '{"a":true}'::jsonb
                  WHEN 3 THEN '{"a":1}'::jsonb ELSE '{"a":"1"}'::jsonb END FROM generate_series(1,8000) i;
SET sift.col_format_minor = 4;
CREATE INDEX k_old_col ON k_old USING sift_col (id,doc) WITH (paths = 'doc->a astext');
RESET sift.col_format_minor;
CREATE INDEX k_new_col ON k_new USING sift_col (id,doc) WITH (paths = 'doc->a astext');
VACUUM ANALYZE k_old;
VACUUM ANALYZE k_new;
ANALYZE k;
SELECT t_check('SELECT jsonb_typeof(doc->''a''), count(*) FROM k GROUP BY 1');
SELECT t_check('SELECT count(*) FROM k WHERE jsonb_typeof(doc->''a'') = ''null''');
REINDEX INDEX k_old_col;
SELECT t_check('SELECT jsonb_typeof(doc->''a''), count(*) FROM k GROUP BY 1');
SELECT t_check('SELECT count(*) FROM k WHERE jsonb_typeof(doc->''a'') = ''null''');
DROP TABLE k;

-- Wide leaves and shredded leaves together use minor 6; an old-minor merge cannot regain complete kinds by changing the setting.
CREATE TABLE k (id int, grp int, doc jsonb, wide numeric(30,4));
INSERT INTO k SELECT i, i % 7, doc(i), i * 1000000000000.0001 FROM generate_series(1, 8000) i;
SET sift.col_format_minor = 4;
CREATE INDEX k_col ON k USING sift_col (id, grp, doc, wide) WITH (paths = 'doc[]{s text, n int4, b bool}');
RESET sift.col_format_minor;
VACUUM ANALYZE k;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
INSERT INTO k SELECT i, i % 7, doc(i), i * 1000000000000.0001 FROM generate_series(8001, 12000) i;
SELECT sift.col_promote('k_col');
SELECT sift.col_merge('k_col', true);
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
REINDEX INDEX k_col;
SELECT fmt('k_col');
SELECT n, t_check(q) FROM q ORDER BY n;
SELECT sift.col_verify('k_col', true);
-- Members outside a declared leaf's type keep exact answers from the original documents.
INSERT INTO k VALUES (13001, 0, '[{"s":1,"n":2147483648,"b":{}},{"s":[],"n":"3","b":1}]', 1);
SELECT n, t_check(q) FROM q ORDER BY n;
DROP TABLE k;
