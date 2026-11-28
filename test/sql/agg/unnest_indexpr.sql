-- SiftAggV6 over jsonb_to_recordset(index expression -> 'key'): the list of objects that a key of an index expression holds is read from the index (a column of the index is an expression, its leaves
-- those of the key), the rows whose document the leaves cannot hold are evaluated from the heap row (the expression is evaluated, then the key taken), and the cast of a text element column runs in
-- PostgreSQL's executor. Each statement runs with the cost settings of the coverage proofs (a page costs 10000) and with sift.enable_agg = off, and the rows must be the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE FUNCTION ux_norm(doc jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN jsonb_typeof(doc) = 'object' AND jsonb_typeof(doc -> 'parts') = 'array' THEN jsonb_set(doc, '{parts}', (
		SELECT coalesce(jsonb_agg(CASE WHEN jsonb_typeof(o -> 'score') = 'string' AND o ->> 'score' !~ '^-?[0-9]+(\.[0-9]+)?$' THEN jsonb_set(o, '{score}', 'null') ELSE o END ORDER BY ord), '[]')
		FROM jsonb_array_elements(doc -> 'parts') WITH ORDINALITY t(o, ord)))
		WHEN jsonb_typeof(doc) = 'object' THEN doc END $$;
CREATE FUNCTION ux_txt(doc jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN jsonb_typeof(doc) = 'object' AND jsonb_typeof(doc -> 'parts') = 'array' THEN jsonb_set(doc, '{parts}', (
		SELECT coalesce(jsonb_agg(CASE WHEN jsonb_typeof(o) = 'object' THEN o || jsonb_build_object('score', o ->> 'score') ELSE o END ORDER BY ord), '[]')
		FROM jsonb_array_elements(doc -> 'parts') WITH ORDINALITY t(o, ord))) END $$;

CREATE TABLE ux (id int PRIMARY KEY, g int, deleted_at timestamp, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ux
SELECT i, i % 7, CASE WHEN i % 17 = 0 THEN timestamp '2025-01-01' END,
	CASE i % 14
		WHEN 0 THEN NULL
		WHEN 1 THEN '[1, 2]'
		WHEN 2 THEN '{"lang": "en"}'
		WHEN 3 THEN '{"parts": []}'
		WHEN 4 THEN '"text"'
		WHEN 5 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'c' || i % 4, 'score', i % 9 + 0.5)))
		WHEN 6 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'c' || i % 3, 'score', i % 5), jsonb_build_object('category', 'c1', 'score', 7.25)))
		WHEN 7 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'c2', 'score', (i % 6)::text), jsonb_build_object('category', 'c0')))
		WHEN 8 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', i % 4, 'score', 'n/a'), jsonb_build_object('category', 'c3', 'score', NULL::text)))
		WHEN 9 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'c0', 'score', 1.5), jsonb_build_object('category', 'c1', 'score', 2.5),
					jsonb_build_object('category', 'c2', 'score', 3.5), jsonb_build_object('category', 'c3', 'score', 4.5)))
		WHEN 10 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', jsonb_build_object('x', i), 'score', 2)))
		WHEN 11 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', true, 'score', '2.5'), jsonb_build_object('category', 'c1', 'score', 1e3)))
		WHEN 12 THEN jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'c2', 'score', -3.5, 'extra', 1)))
		ELSE jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'c' || i % 2, 'score', i % 3)), 'lang', 'de')
	END
FROM generate_series(1, 420) i;
CREATE INDEX ux_col ON ux USING sift_col (id, g, deleted_at, (ux_norm(doc)), (ux_txt(doc)))
	WITH (paths = 'ux_norm.parts[]{category astext, score float8}, ux_txt.parts[]{category astext, score astext}');

CREATE TABLE px (id int PRIMARY KEY, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO px SELECT i, i % 5, CASE WHEN i % 9 = 0 THEN NULL WHEN i % 9 = 1 THEN '{"other": 1}'
		ELSE jsonb_build_object('items', jsonb_build_array(jsonb_build_object('k', 'k' || i % 3, 'v', i % 11 * 0.5), jsonb_build_object('k', 'k' || i % 4, 'v', i % 7))) END
FROM generate_series(1, 280) i;
CREATE INDEX px_col ON px USING sift_col (id, g, doc) WITH (paths = 'doc.items[]{k text, v float8}');
VACUUM (FREEZE, ANALYZE) ux;
VACUUM (FREEZE, ANALYZE) px;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	SET LOCAL seq_page_cost = 10000;
	SET LOCAL random_page_cost = 10000;
	SET LOCAL enable_memoize = off;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6)%' THEN engine := 'SiftAggV6'; END IF;
		IF l LIKE '%Custom Scan (SiftAggV6Two)%' THEN engine := 'SiftAggV6Two'; END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_heap := (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE FUNCTION failure(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL seq_page_cost = 10000;
	SET LOCAL random_page_cost = 10000;
	BEGIN EXECUTE q; a := 'no error'; EXCEPTION WHEN OTHERS THEN a := SQLERRM; END;
	SET LOCAL sift.enable_agg = off;
	BEGIN EXECUTE q; b := 'no error'; EXCEPTION WHEN OTHERS THEN b := SQLERRM; END;
	RESET sift.enable_agg;
	RETURN CASE WHEN a = b THEN 'same: ' || a ELSE 'DIFFERENT: ' || a || ' / ' || b END;
END $$;

CREATE TABLE stmts (id int, q text);
INSERT INTO stmts VALUES
-- the key of an index expression: the leaves of the key, each column of the element, NULL and non-object documents, an empty list, a key that is not there
(1, 'SELECT i.category, count(*), sum(i.score), avg(i.score), min(i.score), max(i.score) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) GROUP BY 1'),
(2, 'SELECT r.g, i.category, count(*), sum(i.score) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) WHERE r.deleted_at IS NULL GROUP BY 1, 2'),
(3, 'SELECT count(*), count(i.score), count(i.category) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8)'),
(4, 'SELECT i.category, count(*) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) WHERE i.score > 2 AND r.id > 100 GROUP BY 1'),
(5, 'SELECT r.id, count(*), sum(i.score) FROM ux r LEFT JOIN LATERAL jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) ON true GROUP BY 1'),
(6, 'SELECT i.category, count(DISTINCT r.id), count(DISTINCT r.g) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) GROUP BY 1'),
-- the text leaf of the other index expression: the scores as text, the regex and the cast in the aggregate (a text that is no number is never cast)
(7, 'SELECT i.category, count(*), count(i.score), min(i.score) FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) GROUP BY 1'),
(8, 'SELECT i.category, avg(CASE WHEN i.score ~ ''^-?[0-9]+(\.[0-9]+)?$'' THEN i.score::float8 END), sum(CASE WHEN i.score ~ ''^-?[0-9]+(\.[0-9]+)?$'' THEN i.score::float8 END),
	count(CASE WHEN i.score ~ ''^-?[0-9]+(\.[0-9]+)?$'' THEN i.score::float8 END) FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) GROUP BY 1'),
(9, 'SELECT r.g, max(CASE WHEN i.score ~ ''^-?[0-9]+(\.[0-9]+)?$'' THEN i.score::float8 END), min(CASE WHEN i.score ~ ''^-?[0-9]+(\.[0-9]+)?$'' THEN i.score::float8 END)
	FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) WHERE r.deleted_at IS NULL GROUP BY 1'),
(10, 'SELECT i.category, sum(i.score::float8) FILTER (WHERE i.score ~ ''^[0-9]+$'') FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) GROUP BY 1'),
-- a CTE read by two arms (a key per item and a total per item): the arms are inlined, each is a scan of the elements, and the outer aggregate counts the items
(11, 'WITH b AS (SELECT r.id AS rid, i.category AS k, CASE WHEN i.score ~ ''^-?[0-9]+(\.[0-9]+)?$'' THEN i.score::float8 END AS n
		FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) WHERE r.deleted_at IS NULL)
	SELECT k, avg(a) AS value, count(DISTINCT rid) AS items, sum(s) AS total
	FROM (SELECT rid, k, avg(n) AS a, sum(n) AS s FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n), sum(n) FROM b GROUP BY rid) x GROUP BY k'),
(12, 'WITH b AS (SELECT r.id rid, r.g pid, i.category k, i.score n FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) WHERE r.deleted_at IS NULL)
	SELECT k, avg(a), count(DISTINCT rid), count(DISTINCT pid)
	FROM (SELECT rid, pid, k, avg(n) a FROM b GROUP BY rid, pid, k UNION ALL SELECT rid, pid, NULL::text, avg(n) FROM b GROUP BY rid, pid) x GROUP BY k'),
-- a key of a plain jsonb column
(13, 'SELECT i.k, count(*), sum(i.v), avg(i.v) FROM px r, jsonb_to_recordset(r.doc -> ''items'') AS i(k text, v float8) GROUP BY 1'),
(14, 'SELECT r.g, count(*), max(i.v) FROM px r LEFT JOIN LATERAL jsonb_to_recordset(r.doc -> ''items'') AS i(k text, v float8) ON true WHERE r.id > 30 GROUP BY 1'),
-- what the index does not keep: a key it has no leaf for, and an expression it does not hold
(15, 'SELECT i.category, count(*) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''other'') AS i(category text, score float8) GROUP BY 1'),
(16, 'SELECT i.category, count(*) FROM ux r, jsonb_to_recordset(coalesce(r.doc, ''{}'') -> ''parts'') AS i(category text, score text) GROUP BY 1'),
(17, 'SELECT i.category, count(*), sum(i.score) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) WHERE r.id IN (10, 11, 25) GROUP BY 1');
SELECT s.id, r.* FROM stmts s, LATERAL same(s.q) r ORDER BY s.id;

-- values only the heap row gives (statement 17): a score written as a string, a category that is a nested object
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SELECT i.category, count(*), sum(i.score) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> 'parts') AS i(category text, score float8)
WHERE r.id IN (10, 11, 25) GROUP BY 1 ORDER BY 3;
RESET seq_page_cost;
RESET random_page_cost;

-- a score that is a number written as a string and a part with a nested object, in the rows the leaves cannot hold: the heap row is evaluated
INSERT INTO ux VALUES
	(1001, 1, NULL, '{"parts": [{"category": "late", "score": "4.5"}, {"category": "late", "score": 6}]}'),
	(1002, 2, NULL, '{"parts": [{"category": {"deep": [1, 2]}, "score": 1}]}');
SELECT * FROM same('SELECT i.category, count(*), sum(i.score) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) GROUP BY 1');
SELECT * FROM same('SELECT i.category, count(*), sum(CASE WHEN i.score ~ ''^[0-9.]+$'' THEN i.score::float8 END) FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) GROUP BY 1');

-- rows changed after the index was built
UPDATE ux SET doc = jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'upd', 'score', id))) WHERE id % 50 = 0;
DELETE FROM ux WHERE id % 61 = 0;
INSERT INTO ux SELECT 2000 + i, i % 7, NULL, jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'new' || i % 3, 'score', i * 0.5))) FROM generate_series(1, 40) i;
SELECT s.id, r.* FROM stmts s, LATERAL same(s.q) r WHERE s.id IN (1, 2, 8, 11, 12) ORDER BY s.id;

-- an error of the function is the error of the statement, from the index and from the heap alike
INSERT INTO ux VALUES (3001, 1, NULL, '{"parts": [{"category": "bad", "score": [1]}]}');
SELECT failure('SELECT i.category, sum(i.score) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) GROUP BY 1');
DELETE FROM ux WHERE id = 3001;
INSERT INTO ux VALUES (3002, 1, NULL, '{"parts": [1, 2]}');
SELECT failure('SELECT i.category, count(*) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) GROUP BY 1');
DELETE FROM ux WHERE id = 3002;
INSERT INTO ux VALUES (3003, 1, NULL, '{"parts": null}');
SELECT failure('SELECT i.category, count(*) FROM ux r, jsonb_to_recordset(ux_norm(r.doc) -> ''parts'') AS i(category text, score float8) GROUP BY 1');
DELETE FROM ux WHERE id = 3003;
SELECT failure('SELECT i.category, sum(CASE WHEN i.score ~ ''^[0-9.]+$'' THEN i.score::float8 END) FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) GROUP BY 1');
INSERT INTO ux VALUES (3004, 1, NULL, '{"parts": [{"category": "a", "score": 1}]}');
SELECT failure('SELECT i.category, sum(i.score::float8) FILTER (WHERE i.score <> ''0'') FROM ux r, jsonb_to_recordset(ux_txt(r.doc) -> ''parts'') AS i(category text, score text) GROUP BY 1');

DROP TABLE stmts, ux, px;
DROP FUNCTION same(text), failure(text), ux_norm(jsonb), ux_txt(jsonb);
DROP EXTENSION pg_sift;
