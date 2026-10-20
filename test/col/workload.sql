-- A synthetic review analytics workload for the column index (docs/AGG.md, "Workload shapes"): a fact table of feedback items with jsonb ratings
-- (numbers, and dirty values: numbers as strings, JSON nulls, text), a jsonb comments array and a document with a nested array of opinions and the flags
-- of a mapping, dimension tables of entities (typed), sources and mappings, dates and a soft-delete column. Included by fixture.sql (small) and
-- usable alone for a larger table:
--
--   psql -v n=300000 -f workload.sql      then  CREATE INDEX ... (scripts/coldiff.sh col_index t_wl, bench/sql/workload/run.sh)
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

DROP TABLE IF EXISTS t_wl, t_wl_entity, t_wl_source, t_wl_map CASCADE;

-- a rating value: 80% a number with one decimal, 10% an integer, 4% a number written as a string, 2% a JSON null, 2% text, 2% a boolean
CREATE OR REPLACE FUNCTION coldiff.wl_value(id bigint, draw int, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE WHEN x < 0.04 THEN to_jsonb(d::text) WHEN x < 0.06 THEN 'null'::jsonb WHEN x < 0.08 THEN to_jsonb((ARRAY['n/a', 'none'])[1 + (id % 2)::int])
				WHEN x < 0.10 THEN to_jsonb(d < 5) WHEN x < 0.20 THEN to_jsonb(d::int) ELSE to_jsonb(d::float8) END
	FROM (SELECT coldiff.u(id, draw, seed) AS x, round((coldiff.u(id, draw + 500, seed) * 10)::numeric, 1) AS d) v $$;

-- the ratings object: the keys overall (95%), service, cleanliness, location, value and a key no mapping knows (10%)
CREATE OR REPLACE FUNCTION coldiff.wl_ratings(id bigint, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT coalesce(jsonb_object_agg(k, coldiff.wl_value(id, 100 + n::int, seed)) FILTER (WHERE coldiff.u(id, 90 + n::int, seed) < p), '{}')
	FROM unnest(ARRAY['overall', 'service', 'cleanliness', 'location', 'value', 'unmapped_key'], ARRAY[0.95, 0.7, 0.7, 0.6, 0.4, 0.1]) WITH ORDINALITY t(k, p, n) $$;

CREATE OR REPLACE FUNCTION coldiff.wl_comments(id bigint, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT coalesce(jsonb_agg(jsonb_build_object('type', (ARRAY['general', 'manager', 'owner'])[1 + floor(coldiff.u(id, 110 + j, seed) * 3)::int], 'comment', 'c' || j) ORDER BY j), '[]')
	FROM generate_series(1, floor(coldiff.u(id, 109, seed) * 3)::int) j $$;

-- one opinion: a category, a subrating (empty, NULL or a name), a polarity (0.9% a JSON null, the number 1 or true) and a score that is a number, or (3%) a number written as a string, or (0.5%) text
CREATE OR REPLACE FUNCTION coldiff.wl_opinion(id bigint, j int, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT jsonb_build_object(
		'category', (ARRAY['cleanliness', 'service', 'location', 'value', 'comfort', 'food', 'staff', 'misc'])[1 + floor(coldiff.u(id, 120 + j, seed) ^ 1.5 * 8)::int],
		'subrating', CASE floor(coldiff.u(id, 130 + j, seed) * 6)::int WHEN 0 THEN 'wifi' WHEN 1 THEN 'pool' WHEN 2 THEN NULL ELSE '' END,
		'polarity', CASE WHEN coldiff.u(id, 141 + j, seed) < 0.004 THEN 'null'::jsonb WHEN coldiff.u(id, 141 + j, seed) < 0.007 THEN '1'::jsonb
							 WHEN coldiff.u(id, 141 + j, seed) < 0.009 THEN 'true'::jsonb
							 ELSE to_jsonb((ARRAY['positive', 'negative', 'neutral'])[1 + floor(coldiff.u(id, 140 + j, seed) * 3)::int]) END,
		'score', CASE WHEN coldiff.u(id, 150 + j, seed) < 0.03 THEN to_jsonb(round((coldiff.u(id, 160 + j, seed) * 10)::numeric, 1)::text)
					  WHEN coldiff.u(id, 150 + j, seed) < 0.035 THEN '"n/a"'::jsonb
					  ELSE to_jsonb(round((coldiff.u(id, 160 + j, seed) * 10)::numeric, 1)::float8) END,
		'words', '["w"]'::jsonb) $$;

-- the document: 0.3% are not objects (an array, a string, a number, a JSON null); the others have a language, the flag mapped (true, false or absent) and
-- 0..5 opinions (1.5% have no opinions key, 0.5% a null one)
CREATE OR REPLACE FUNCTION coldiff.wl_doc(id bigint, seed int) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE WHEN coldiff.u(id, 170, seed) < 0.003
		THEN (ARRAY['[]', '"text"', '12', 'null'])[1 + floor(coldiff.u(id, 171, seed) * 4)::int]::jsonb
		ELSE jsonb_build_object('lang', (ARRAY['en', 'de', 'fr', 'es', 'it'])[1 + floor(coldiff.u(id, 172, seed) * 5)::int])
			|| CASE WHEN coldiff.u(id, 173, seed) < 0.4 THEN '{}'::jsonb ELSE jsonb_build_object('mapped', coldiff.u(id, 174, seed) < 0.7) END
			|| CASE WHEN coldiff.u(id, 175, seed) < 0.015 THEN '{}'::jsonb WHEN coldiff.u(id, 175, seed) < 0.02 THEN '{"opinions": null}'::jsonb
				ELSE jsonb_build_object('opinions', (SELECT coalesce(jsonb_agg(coldiff.wl_opinion(id, j, seed) ORDER BY j), '[]')
													 FROM generate_series(1, floor(coldiff.u(id, 176, seed) ^ 1.3 * 6)::int) j)) END
		END $$;

-- the document with its dirty scores repaired and the documents that are not objects NULL: the form an index of typed leaves reads
CREATE OR REPLACE FUNCTION coldiff.wl_norm(doc jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE
		WHEN jsonb_typeof(doc) <> 'object' THEN NULL
		WHEN jsonb_typeof(doc -> 'opinions') <> 'array' THEN doc - 'opinions'
		ELSE jsonb_set(doc, '{opinions}', (
			SELECT coalesce(jsonb_agg(CASE
				WHEN jsonb_typeof(o) = 'object' AND jsonb_typeof(o -> 'score') = 'string'
				THEN jsonb_set(o, '{score}', CASE WHEN o ->> 'score' ~ '^-?[0-9]+(\.[0-9]+)?$' THEN to_jsonb((o ->> 'score')::numeric) ELSE 'null'::jsonb END)
				ELSE o END ORDER BY ord), '[]')
			FROM jsonb_array_elements(doc -> 'opinions') WITH ORDINALITY t(o, ord)))
		END $$;

CREATE TABLE t_wl_entity (id integer PRIMARY KEY, type text NOT NULL, name text);
CREATE TABLE t_wl_source (id integer PRIMARY KEY, name text NOT NULL, kind text);
-- a mapping of (entity type, category, subrating) to a normalized pair; NULL type rows apply to every type
CREATE TABLE t_wl_map (id integer PRIMARY KEY, entity_type text, category text NOT NULL, subrating text NOT NULL, mapped_category text, mapped_subrating text);
INSERT INTO t_wl_entity SELECT i, (ARRAY['accommodation', 'attraction', 'restaurant', 'transport'])[1 + i % 4], 'entity ' || i
FROM generate_series(1, greatest(40, least(1000, :n / 300))) i;
INSERT INTO t_wl_source SELECT i, 'source ' || i, (ARRAY['web', 'app', 'partner'])[1 + i % 3] FROM generate_series(1, 12) i;
INSERT INTO t_wl_map SELECT row_number() OVER (), t, c, s,
	CASE WHEN c = 'misc' AND t = 'transport' THEN NULL ELSE 'group_' || (ascii(c) % 5) END, CASE WHEN s = '' THEN '' ELSE 'sub_' || s END
FROM unnest(ARRAY['accommodation', 'attraction', 'restaurant', 'transport']) t, unnest(ARRAY['cleanliness', 'service', 'location', 'value', 'comfort', 'food', 'staff', 'misc']) c,
	 unnest(ARRAY['', 'wifi', 'pool']) s
WHERE NOT (t = 'attraction' AND c = 'food' AND s = 'pool');
INSERT INTO t_wl_map SELECT 1000 + row_number() OVER (), NULL, c, '', 'any_' || c, '' FROM unnest(ARRAY['cleanliness', 'service']) c;

CREATE TABLE t_wl (
	id integer PRIMARY KEY, entity_id integer NOT NULL, source_id integer, product_id integer, item_date date, updated_at timestamp, deleted_at timestamp,
	country_code varchar(2), composition text, segment text, ratings jsonb DEFAULT '{}', comments jsonb DEFAULT '[]', doc jsonb DEFAULT '{}', title text
) WITH (fillfactor = 90);

-- items first_id .. last_id: entity ~ u^2 over the entities, 3% without a source, 60% without a product, dates 2022-01-01 .. 2025-10-31 (1% NULL), 3% soft-deleted
CREATE OR REPLACE FUNCTION coldiff.fx_wl(first_id int, last_id int, seed int DEFAULT 0) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (
		INSERT INTO t_wl
		SELECT i, 1 + floor(coldiff.u(i, 1, seed) ^ 2 * (SELECT max(id) FROM t_wl_entity))::int,
			   CASE WHEN coldiff.u(i, 2, seed) < 0.03 THEN NULL ELSE 1 + floor(coldiff.u(i, 3, seed) * 12)::int END,
			   CASE WHEN coldiff.u(i, 4, seed) < 0.6 THEN NULL ELSE 1 + floor(coldiff.u(i, 5, seed) * 200)::int END,
			   CASE WHEN coldiff.u(i, 6, seed) < 0.01 THEN NULL ELSE date '2022-01-01' + floor(coldiff.u(i, 7, seed) * 1400)::int END,
			   timestamp '2022-01-01' + floor(coldiff.u(i, 8, seed) * 1400) * interval '1 day' + floor(coldiff.u(i, 9, seed) * 86400) * interval '1 second',
			   CASE WHEN coldiff.u(i, 10, seed) < 0.03 THEN timestamp '2025-11-02' + floor(coldiff.u(i, 11, seed) * 60) * interval '1 day' END,
			   CASE WHEN coldiff.u(i, 12, seed) < 0.05 THEN NULL ELSE (ARRAY['US', 'GB', 'DE', 'FR', 'ES', 'IT', 'NL', 'BR', 'JP', 'CA'])[1 + floor(coldiff.u(i, 13, seed) ^ 2 * 10)::int] END,
			   CASE WHEN coldiff.u(i, 14, seed) < 0.1 THEN NULL ELSE (ARRAY['solo', 'couple', 'family', 'group', 'business', 'other'])[1 + floor(coldiff.u(i, 15, seed) * 6)::int] END,
			   CASE WHEN coldiff.u(i, 16, seed) < 0.2 THEN NULL ELSE (ARRAY['leisure', 'work', 'event', 'transit', 'other'])[1 + floor(coldiff.u(i, 17, seed) * 5)::int] END,
			   coldiff.wl_ratings(i, seed), coldiff.wl_comments(i, seed), coldiff.wl_doc(i, seed), 't' || i % 50
		FROM generate_series(first_id, last_id) i RETURNING 1)
	SELECT count(*) FROM ins $$;
SELECT coldiff.fx_wl(1, :n);
