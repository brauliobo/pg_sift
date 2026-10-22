-- A synthetic event analytics workload for the column index (docs/AGG.md, "Workload shapes"): a fact table of event items with jsonb metrics
-- (numbers, and dirty values: numbers as strings, JSON nulls, text), a jsonb notes array and a document with a nested array of parts and the flags
-- of a mapping, dimension tables of entities (typed), sources and mappings, dates and a soft-delete column. Included by fixture.sql (small) and
-- usable alone for a larger table:
--
--   psql -v n=300000 -f workload.sql      then  CREATE INDEX ... (scripts/coldiff.sh col_index t_work, bench/sql/workload/run.sh)
--
-- Every draw is a hash of (item id, draw number, seed): the same arguments give the same rows on every run.
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 4000
\endif
CREATE SCHEMA IF NOT EXISTS coldiff;
CREATE OR REPLACE FUNCTION coldiff.u(id bigint, draw int, seed int DEFAULT 0) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
	AS $$ SELECT (hashint8extended(id, draw * 1000003::bigint + seed) & 16777215)::float8 / 16777216 $$;

DROP TABLE IF EXISTS t_work, t_work_dim, t_work_src, t_work_map CASCADE;

-- a metric value: 80% a number with one decimal, 10% an integer, 4% a number written as a string, 2% a JSON null, 2% text, 2% a boolean
CREATE OR REPLACE FUNCTION coldiff.wl_value(id bigint, draw int, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE WHEN x < 0.04 THEN to_jsonb(d::text) WHEN x < 0.06 THEN 'null'::jsonb WHEN x < 0.08 THEN to_jsonb((ARRAY['n/a', 'none'])[1 + (id % 2)::int])
				WHEN x < 0.10 THEN to_jsonb(d < 5) WHEN x < 0.20 THEN to_jsonb(d::int) ELSE to_jsonb(d::float8) END
	FROM (SELECT coldiff.u(id, draw, seed) AS x, round((coldiff.u(id, draw + 500, seed) * 10)::numeric, 1) AS d) v $$;

-- the metrics object: the keys m_total (95%), catg_j, catg_b, catg_e, value and a key no mapping knows (10%)
CREATE OR REPLACE FUNCTION coldiff.wl_metrics(id bigint, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT coalesce(jsonb_object_agg(k, coldiff.wl_value(id, 100 + n::int, seed)) FILTER (WHERE coldiff.u(id, 90 + n::int, seed) < p), '{}')
	FROM unnest(ARRAY['m_total', 'catg_j', 'catg_b', 'catg_e', 'value', 'unmapped_key'], ARRAY[0.95, 0.7, 0.7, 0.6, 0.4, 0.1]) WITH ORDINALITY t(k, p, n) $$;

CREATE OR REPLACE FUNCTION coldiff.wl_notes(id bigint, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT coalesce(jsonb_agg(jsonb_build_object('type', (ARRAY['general', 'type_a', 'type_b'])[1 + floor(coldiff.u(id, 110 + j, seed) * 3)::int], 'kind_b', 'c' || j) ORDER BY j), '[]')
	FROM generate_series(1, floor(coldiff.u(id, 109, seed) * 3)::int) j $$;

-- one part: a category, a val (empty, NULL or a name), a sign (0.9% a JSON null, the number 1 or true) and a score that is a number, or (3%) a number written as a string, or (0.5%) text
CREATE OR REPLACE FUNCTION coldiff.wl_part(id bigint, j int, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT jsonb_build_object(
		'category', (ARRAY['catg_b', 'catg_j', 'catg_e', 'value', 'catg_c', 'catg_d', 'catg_k', 'misc'])[1 + floor(coldiff.u(id, 120 + j, seed) ^ 1.5 * 8)::int],
		'val', CASE floor(coldiff.u(id, 130 + j, seed) * 6)::int WHEN 0 THEN 'catg_n' WHEN 1 THEN 'catg_g' WHEN 2 THEN NULL ELSE '' END,
		'dir_a', CASE WHEN coldiff.u(id, 141 + j, seed) < 0.004 THEN 'null'::jsonb WHEN coldiff.u(id, 141 + j, seed) < 0.007 THEN '1'::jsonb
							 WHEN coldiff.u(id, 141 + j, seed) < 0.009 THEN 'true'::jsonb
							 ELSE to_jsonb((ARRAY['up', 'down', 'flat'])[1 + floor(coldiff.u(id, 140 + j, seed) * 3)::int]) END,
		'score', CASE WHEN coldiff.u(id, 150 + j, seed) < 0.03 THEN to_jsonb(round((coldiff.u(id, 160 + j, seed) * 10)::numeric, 1)::text)
					  WHEN coldiff.u(id, 150 + j, seed) < 0.035 THEN '"n/a"'::jsonb
					  ELSE to_jsonb(round((coldiff.u(id, 160 + j, seed) * 10)::numeric, 1)::float8) END,
		'words', '["w"]'::jsonb) $$;

-- the document: 0.3% are not objects (an array, a string, a number, a JSON null); the others have a language, the flag mapped (true, false or absent) and
-- 0..5 parts (1.5% have no parts key, 0.5% a null one)
CREATE OR REPLACE FUNCTION coldiff.wl_doc(id bigint, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE WHEN coldiff.u(id, 170, seed) < 0.003
		THEN (ARRAY['[]', '"text"', '12', 'null'])[1 + floor(coldiff.u(id, 171, seed) * 4)::int]::jsonb
		ELSE jsonb_build_object('lang', (ARRAY['en', 'de', 'fr', 'es', 'it'])[1 + floor(coldiff.u(id, 172, seed) * 5)::int])
			|| CASE WHEN coldiff.u(id, 173, seed) < 0.4 THEN '{}'::jsonb ELSE jsonb_build_object('flag_a', coldiff.u(id, 174, seed) < 0.7) END
			|| CASE WHEN coldiff.u(id, 175, seed) < 0.015 THEN '{}'::jsonb WHEN coldiff.u(id, 175, seed) < 0.02 THEN '{"parts": null}'::jsonb
				ELSE jsonb_build_object('parts', (SELECT coalesce(jsonb_agg(coldiff.wl_part(id, j, seed) ORDER BY j), '[]')
													 FROM generate_series(1, floor(coldiff.u(id, 176, seed) ^ 1.3 * 6)::int) j)) END
		END $$;

-- the document with its dirty scores repaired and the documents that are not objects NULL: the form an index of typed leaves reads
CREATE OR REPLACE FUNCTION coldiff.doc_norm(doc jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE
		WHEN jsonb_typeof(doc) <> 'object' THEN NULL
		WHEN jsonb_typeof(doc -> 'parts') <> 'array' THEN doc - 'parts'
		ELSE jsonb_set(doc, '{parts}', (
			SELECT coalesce(jsonb_agg(CASE
				WHEN jsonb_typeof(o) = 'object' AND jsonb_typeof(o -> 'score') = 'string'
				THEN jsonb_set(o, '{score}', CASE WHEN o ->> 'score' ~ '^-?[0-9]+(\.[0-9]+)?$' THEN to_jsonb((o ->> 'score')::numeric) ELSE 'null'::jsonb END)
				ELSE o END ORDER BY ord), '[]')
			FROM jsonb_array_elements(doc -> 'parts') WITH ORDINALITY t(o, ord)))
		END $$;

-- the parts' scores as text (what a client that keeps the dirty values reads): numbers, numbers as strings and text alike
CREATE OR REPLACE FUNCTION coldiff.wl_txt(doc jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE WHEN jsonb_typeof(doc) = 'object' AND jsonb_typeof(doc -> 'parts') = 'array' THEN jsonb_set(doc, '{parts}', (
		SELECT coalesce(jsonb_agg(CASE WHEN jsonb_typeof(o) = 'object' THEN o || jsonb_build_object('score', o ->> 'score') ELSE o END ORDER BY ord), '[]')
		FROM jsonb_array_elements(doc -> 'parts') WITH ORDINALITY t(o, ord))) END $$;

CREATE TABLE t_work_dim (id integer PRIMARY KEY, type text NOT NULL, name text);
CREATE TABLE t_work_src (id integer PRIMARY KEY, name text NOT NULL, kind text);
-- a mapping of (entity type, category, val) to a normalized pair; NULL type rows apply to every type
CREATE TABLE t_work_map (id integer PRIMARY KEY, entity_type text, category text NOT NULL, val text NOT NULL, derived_category text, derived_val text);
INSERT INTO t_work_dim SELECT i, (ARRAY['cat_a', 'cat_b', 'cat_c', 'cat_d'])[1 + i % 4], 'entity ' || i
FROM generate_series(1, greatest(40, least(1000, :n / 300))) i;
INSERT INTO t_work_src SELECT i, 'source ' || i, (ARRAY['web', 'app', 'partner'])[1 + i % 3] FROM generate_series(1, 12) i;
INSERT INTO t_work_map SELECT row_number() OVER (), t, c, s,
	CASE WHEN c = 'misc' AND t = 'cat_d' THEN NULL ELSE 'group_' || (ascii(c) % 5) END, CASE WHEN s = '' THEN '' ELSE 'sub_' || s END
FROM unnest(ARRAY['cat_a', 'cat_b', 'cat_c', 'cat_d']) t, unnest(ARRAY['catg_b', 'catg_j', 'catg_e', 'value', 'catg_c', 'catg_d', 'catg_k', 'misc']) c,
	 unnest(ARRAY['', 'catg_n', 'catg_g']) s
WHERE NOT (t = 'cat_b' AND c = 'catg_d' AND s = 'catg_g');
INSERT INTO t_work_map SELECT 1000 + row_number() OVER (), NULL, c, '', 'any_' || c, '' FROM unnest(ARRAY['catg_b', 'catg_j']) c;

CREATE TABLE t_work (
	id integer PRIMARY KEY, dim_a_id integer NOT NULL, foreign_id integer, foreign_id_b integer, item_date date, updated_at timestamp, deleted_at timestamp,
	country_code varchar(2), mix text, segment text, metrics jsonb DEFAULT '{}', tags_a jsonb DEFAULT '[]', doc jsonb DEFAULT '{}', title text
) WITH (fillfactor = 90);

-- items first_id .. last_id: entity ~ u^2 over the entities, 3% without a source, 60% without a product, dates 2022-01-01 .. 2025-10-31 (1% NULL), 3% soft-deleted
CREATE OR REPLACE FUNCTION coldiff.fx_wl(first_id int, last_id int, seed int DEFAULT 0) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (
		INSERT INTO t_work
		SELECT i, 1 + floor(coldiff.u(i, 1, seed) ^ 2 * (SELECT max(id) FROM t_work_dim))::int,
			   CASE WHEN coldiff.u(i, 2, seed) < 0.03 THEN NULL ELSE 1 + floor(coldiff.u(i, 3, seed) * 12)::int END,
			   CASE WHEN coldiff.u(i, 4, seed) < 0.6 THEN NULL ELSE 1 + floor(coldiff.u(i, 5, seed) * 200)::int END,
			   CASE WHEN coldiff.u(i, 6, seed) < 0.01 THEN NULL ELSE date '2022-01-01' + floor(coldiff.u(i, 7, seed) * 1400)::int END,
			   timestamp '2022-01-01' + floor(coldiff.u(i, 8, seed) * 1400) * interval '1 day' + floor(coldiff.u(i, 9, seed) * 86400) * interval '1 second',
			   CASE WHEN coldiff.u(i, 10, seed) < 0.03 THEN timestamp '2025-11-02' + floor(coldiff.u(i, 11, seed) * 60) * interval '1 day' END,
			   CASE WHEN coldiff.u(i, 12, seed) < 0.05 THEN NULL ELSE (ARRAY['US', 'GB', 'DE', 'FR', 'ES', 'IT', 'NL', 'BR', 'JP', 'CA'])[1 + floor(coldiff.u(i, 13, seed) ^ 2 * 10)::int] END,
			   CASE WHEN coldiff.u(i, 14, seed) < 0.1 THEN NULL ELSE (ARRAY['solo', 'couple', 'family', 'group', 'business', 'other'])[1 + floor(coldiff.u(i, 15, seed) * 6)::int] END,
			   CASE WHEN coldiff.u(i, 16, seed) < 0.2 THEN NULL ELSE (ARRAY['leisure', 'work', 'event', 'transit', 'other'])[1 + floor(coldiff.u(i, 17, seed) * 5)::int] END,
			   coldiff.wl_metrics(i, seed), coldiff.wl_notes(i, seed), coldiff.wl_doc(i, seed), 't' || i % 50
		FROM generate_series(first_id, last_id) i RETURNING 1)
	SELECT count(*) FROM ins $$;
SELECT coldiff.fx_wl(1, :n);
