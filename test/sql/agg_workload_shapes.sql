-- The shapes of an event analytics workload: documents with an array of sections held by an index expression, a lookup of the entities that an
-- ARRAY sub-select returns, EXISTS and nested jsonpath tests over the sections, and per-item arms over a text leaf with the cast in the aggregate
-- (docs/AGG.md, "Workload shapes"). Every statement is run with the pushdown and with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- the table is small: page costs that keep the btree and bitmap plans from looking cheap as well
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ents (id int PRIMARY KEY, type text NOT NULL);
INSERT INTO ents SELECT i, (ARRAY['site', 'museum', 'diner', 'ferry'])[1 + i % 4] FROM generate_series(1, 60) i;

-- the sections of an item: a category, a sign and a score that is a number, a numeric string or text
CREATE FUNCTION op_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN i % 301 = 0 THEN '[]'::jsonb ELSE jsonb_build_object('lang', (ARRAY['en', 'de', 'fr'])[1 + i % 3], 'sections', (
		SELECT coalesce(jsonb_agg(jsonb_build_object(
			'category', (ARRAY['service', 'food', 'view', 'price'])[1 + (i * j) % 4],
			'sign', (ARRAY['positive', 'negative', 'neutral'])[1 + (i + j) % 3],
			'score', CASE (i + j) % 11 WHEN 0 THEN to_jsonb('n/a'::text) WHEN 1 THEN to_jsonb(((i * j) % 10)::text) ELSE to_jsonb(((i * j) % 100) / 10.0) END) ORDER BY j), '[]')
		FROM generate_series(1, i % 5) j)) END $$;
-- the same with the scores as text (what a client that keeps the values as they are reads); a document that is no object is NULL
CREATE FUNCTION op_txt(doc jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN jsonb_typeof(doc) = 'object' THEN jsonb_set(doc, '{sections}', (
		SELECT coalesce(jsonb_agg(o || jsonb_build_object('score', o ->> 'score') ORDER BY ord), '[]')
		FROM jsonb_array_elements(doc -> 'sections') WITH ORDINALITY t(o, ord))) END $$;

CREATE TABLE ev (id int PRIMARY KEY, entity_id int, deleted_at timestamptz, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + (i * 7) % 60, CASE WHEN i % 17 = 0 THEN now() END, op_doc(i) FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, entity_id, deleted_at, (op_txt(doc)))
	WITH (paths = 'op_txt.lang astext, op_txt.sections[]{category astext, sign astext, score astext}');
VACUUM (FREEZE, ANALYZE) ev;
ANALYZE ents;

-- the shape of the plan: the number of SiftAgg nodes and the aggregates one pushes
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN n := n + 1; END IF;
		IF l LIKE '%Pushed Aggregates:%' THEN r := r || trim(l) || '; '; END IF;
	END LOOP;
	RETURN CASE n WHEN 0 THEN 'normal' WHEN 1 THEN 'SiftAgg' ELSE n || ' SiftAgg' END || CASE WHEN r = '' THEN '' ELSE ' (' || r || ')' END;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- x = ANY (ARRAY (sub-select)): the sub-select runs once and its rows are a lookup table of the leaf; a sub-select that reads the row, is volatile or
-- may raise keeps the normal plan
SELECT * FROM same($$SELECT count(*) AS n, sum(id) AS s FROM ev WHERE deleted_at IS NULL AND entity_id = ANY (ARRAY(SELECT id FROM ents WHERE type = 'site'))$$);
SELECT * FROM same($$SELECT entity_id % 3 AS g, count(*) AS n FROM ev WHERE entity_id = ANY (ARRAY(SELECT id FROM ents WHERE type IN ('diner', 'ferry'))) GROUP BY 1$$);
SELECT * FROM same($$SELECT count(*) AS n FROM ev WHERE entity_id = ANY (ARRAY(SELECT id FROM ents WHERE type = 'none'))$$);
SELECT * FROM same($$SELECT count(*) AS n FROM ev WHERE entity_id = ANY (ARRAY(SELECT id FROM ents WHERE id = ev.id % 60))$$);
SELECT * FROM same($$SELECT count(*) AS n FROM ev WHERE entity_id = ANY (ARRAY(SELECT id FROM ents WHERE random() < 2))$$);
SELECT plan_of($$SELECT count(*) AS n FROM ev WHERE entity_id = ANY (ARRAY(SELECT 60 / (id - 7) FROM ents))$$) AS raising_division;

-- EXISTS over the sections of an index expression, with the members of one element and the other elements not counting
SELECT * FROM same($$SELECT count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(op_txt(doc) -> 'sections') AS o(category text, sign text)
	WHERE o.category = 'service' AND o.sign = 'negative')) AS service_negative, count(*) AS n FROM ev$$);
SELECT * FROM same($$SELECT op_txt(doc) ->> 'lang' AS lang, count(*) AS n FROM ev
	WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(op_txt(doc) -> 'sections') AS o(category text) WHERE o.category IN ('view', 'price')) GROUP BY 1$$);
SELECT * FROM same($$SELECT count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(op_txt(doc) -> 'sections') AS o(category text) WHERE o.category = 'food')$$);
-- nested jsonpath tests over the sections
SELECT * FROM same($$SELECT count(*) AS n FROM ev WHERE jsonb_path_exists(op_txt(doc), '$.sections[*] ? (@.category == "service" && @.sign == "negative")')$$);
SELECT * FROM same($$SELECT count(*) AS n FROM ev WHERE op_txt(doc) @? '$.sections[*] ? (@.sign != "neutral")'$$);
-- a score is text in the leaf: a number literal is not the text of a string score, so the comparison keeps the normal plan
SELECT plan_of($$SELECT count(*) AS n FROM ev WHERE jsonb_path_exists(op_txt(doc), '$.sections[*] ? (@.score == 7)')$$) AS number_against_text_leaf;
-- a strict path raises for a document that has no such key: the heap raises, the scan must not answer instead
SELECT plan_of($$SELECT count(*) AS n FROM ev WHERE jsonb_path_exists(op_txt(doc), 'strict $.sections[*] ? (@.category == "food")')$$) AS strict_path;

-- per-item arms under an outer aggregate over a text leaf with the cast in the aggregate (guarded: a score may be text): one fused scan
SELECT * FROM same($$WITH b AS (SELECT r.id AS rid, i.category AS k, CASE WHEN i.score ~ '^[0-9.]+$' THEN i.score::float8 END AS n
	FROM ev r, jsonb_to_recordset(op_txt(r.doc) -> 'sections') AS i(category text, score text) WHERE r.deleted_at IS NULL)
	SELECT k, avg(a) AS value, count(DISTINCT rid) AS items, sum(s) AS total
	FROM (SELECT rid, k, avg(n) AS a, sum(n) AS s FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n), sum(n) FROM b GROUP BY rid) x GROUP BY k$$);
-- the arms share a WHERE that keeps the cast off the text scores: it is the scan's own, and the scan casts only what passes it
SELECT * FROM same($$WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
	FROM ev r, jsonb_to_recordset(op_txt(r.doc) -> 'sections') AS i(category text, score text) WHERE r.deleted_at IS NULL)
	SELECT k, avg(a) AS value, count(DISTINCT rid) AS items
	FROM (SELECT rid, k, avg(n::float8) AS a FROM b WHERE n ~ '^[0-9.]+$' GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b WHERE n ~ '^[0-9.]+$' GROUP BY rid) x GROUP BY k$$);
-- a WHERE of each arm's own (they differ) would have the fused scan cast every item, the text scores too: not fused (an arm may be its own scan)
SELECT * FROM same($$WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
	FROM ev r, jsonb_to_recordset(op_txt(r.doc) -> 'sections') AS i(category text, score text) WHERE r.deleted_at IS NULL)
	SELECT k, avg(a) AS value, count(DISTINCT rid) AS items
	FROM (SELECT rid, k, avg(n::float8) AS a FROM b WHERE n ~ '^[0-9.]+$' GROUP BY rid, k
		  UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b WHERE n ~ '^[0-9.]+$' AND k <> 'view' GROUP BY rid) x GROUP BY k$$);
-- the plain cast raises for a text score, in both plans
SELECT * FROM same($$WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
	FROM ev r, jsonb_to_recordset(op_txt(r.doc) -> 'sections') AS i(category text, score text) WHERE r.deleted_at IS NULL)
	SELECT k, avg(a) AS value, count(DISTINCT rid) AS items
	FROM (SELECT rid, k, avg(n::float8) AS a FROM b GROUP BY rid, k UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b GROUP BY rid) x GROUP BY k$$);
SET sift.enable_agg = off;
SELECT count(*) AS n FROM ev r, jsonb_to_recordset(op_txt(r.doc) -> 'sections') AS i(category text, score text) WHERE i.score::float8 > 5;
RESET sift.enable_agg;

DROP TABLE ev;
DROP TABLE ents;
DROP FUNCTION same(text), plan_of(text), op_txt(jsonb), op_doc(int);
DROP EXTENSION pg_sift;
