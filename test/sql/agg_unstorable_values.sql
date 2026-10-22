-- A typed leaf cannot store every value a document holds (a string in a number path, a list too long for a unit). The aggregate scan must not
-- raise where the normal plan answers: the rows with such a value are read again from the heap, or the plan stays normal.
-- Every query runs with and without the pushdown; the rows, or the error, must be the same (a result the normal plan raises for may be answered).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/agg_helpers.sql

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

-- the scan answers where the normal plan does: no error of the scan, whatever the normal plan does
CREATE FUNCTION answers(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' WHEN b LIKE 'ERR%' THEN 'RAISES ' || left(b, 100) WHEN a LIKE 'ERR%' THEN 'answers (the normal plan raises)' ELSE 'DIFF' END;
END $$;

-- ---- a filter on the member of one key, an aggregate over every member: a dirty value of another key leaves the plan on the scan ----
-- a day is a unit apart: the dirty rows sit in days 10 and 600, the filtered key is dirty in day 300
CREATE TABLE events (id int PRIMARY KEY, kind text, day int, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, 'cat_' || chr(97 + i % 3), i / 10, jsonb_build_object('score', i % 11, 'catg_a', (i % 7) + 0.5, 'catg_b', i % 5)
FROM generate_series(1, 6000) i;
UPDATE events SET payload = jsonb_set(payload, '{catg_a}', '"9"') WHERE id IN (100, 6000);
UPDATE events SET payload = jsonb_set(payload, '{score}', '"n/a"') WHERE id = 3001;
CREATE INDEX events_col ON events USING sift_col (id, kind, day, payload) WITH (paths = 'payload.* float8');
VACUUM ANALYZE events;

CREATE FUNCTION eq(keep text, sel text DEFAULT 'avg(CAST(e.value AS float8))', grp text DEFAULT 'e.key') RETURNS text LANGUAGE sql AS $$
	SELECT 'SELECT ' || grp || ', ' || sel || ' FROM events t JOIN LATERAL jsonb_each(t.payload) e ON true WHERE ' || keep || ' GROUP BY ' || grp
$$;

-- a read of the leaf that the scan cannot make again keeps the normal plan
SELECT pushed('SELECT kind, sum((payload ->> $$catg_a$$)::float8) FROM events GROUP BY kind') AS plain_member_pushed;
SELECT same('SELECT kind, sum((payload ->> $$catg_b$$)::float8) FROM events WHERE id < 90 GROUP BY kind') AS plain_member_same;
-- a conjunct on the member of a key keeps the normal plan: the row read again would raise the error of the cast of the aggregate, not of the conjunct's own type
SELECT pushed(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8')) AS key_filter_pushed;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND id < 3000')) AS key_filter_cast;
SELECT same(eq('(t.payload -> $$score$$)::float8 = 8 AND id < 3000')) AS key_filter_jsonb;
SELECT same(eq('(t.payload ->> $$score$$)::float8 = 8 AND id < 3000')) AS key_filter_text;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) >= 8 AND CAST(jsonb_extract_path(t.payload, $$catg_b$$) AS integer) < 3 AND id < 3000')) AS two_key_filters;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND id < 3000', 'count(*), sum(CAST(e.value AS float8)), min(CAST(e.value AS float8))', 't.kind, e.key')) AS grouped;
SELECT same(eq('id < 3000', 'avg(CAST(e.value AS float8)), sum(CAST(jsonb_extract_path(t.payload, $$score$$) AS float8))')) AS key_aggregate;
SELECT same(eq('id < 3000', 'count(*), avg(CAST(e.value AS float8))', 'CAST(jsonb_extract_path(t.payload, $$score$$) AS integer)')) AS key_group;
-- a row whose filtered key is the dirty one: the normal plan raises where it evaluates the cast, and so does the query
SELECT answers(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8')) AS whole_table;
SELECT answers(eq('id BETWEEN 3000 AND 3002 AND CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8')) AS dirty_key_rows;
-- every row that holds a dirty value has the other key's cast raise: both plans do
SELECT same(eq('id IN (100, 6000) AND CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) < 11')) AS dirty_rows_reach_the_cast;

-- the writer's rows: in the write buffer, updated, promoted, merged, vacuumed
INSERT INTO events VALUES (7001, 'cat_a', 10, '{"score": 8, "catg_a": "x"}'), (7002, 'cat_b', 11, '{"score": 3, "catg_a": 4}');
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 3 AND id < 3000')) AS write_buffer;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND id < 3000')) AS write_buffer_dirty_dropped;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND (id < 3000 OR id > 7000)')) AS write_buffer_dirty_reached;
UPDATE events SET payload = '{"score": 8, "catg_a": 1}' WHERE id IN (100, 7001);
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND (id < 3000 OR id > 7000)')) AS after_update;
UPDATE events SET payload = jsonb_set(payload, '{catg_a}', '"7"') WHERE id = 100;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND id < 3000')) AS numeric_string_back;
UPDATE events SET payload = jsonb_set(payload, '{catg_a}', '2') WHERE id = 100;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND id < 3000')) AS after_promotion;
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8 AND id < 3000')) AS after_merge;
DELETE FROM events WHERE id IN (6000, 3001);
VACUUM events;
SELECT same(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8')) AS after_vacuum;
SELECT pushed(eq('CAST(jsonb_extract_path(t.payload, $$score$$) AS integer) = 8')) AS after_vacuum_pushed;

DROP TABLE events;

-- ---- a string "-0" in a float column of a recordset: the leaf never holds -0, the row is read again with the value 0 ----
CREATE TABLE facts (id int PRIMARY KEY, kind text, day int, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 'cat_' || chr(97 + i % 3), i / 10,
	jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 'cat_a', 'score', i % 9), jsonb_build_object('kind', 'cat_b', 'score', i % 3)))
FROM generate_series(1, 2000) i;
UPDATE facts SET payload = jsonb_set(payload, '{parts,0,score}', '"-0"') WHERE id IN (100, 1500);
UPDATE facts SET payload = jsonb_set(payload, '{parts,1,score}', '"-0"') WHERE id = 7;
CREATE INDEX facts_col ON facts USING sift_col (id, kind, day, payload) WITH (paths = 'payload.parts[]{kind text, score float8}');
VACUUM ANALYZE facts;
SELECT pushed('SELECT r.kind, avg(r.score) FROM facts t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true GROUP BY 1') AS recordset_pushed;
SELECT same('SELECT r.kind, avg(r.score), count(r.score), min(r.score), max(r.score) FROM facts t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true GROUP BY 1') AS negative_zero;
SELECT same('SELECT r.kind, avg(r.score), count(r.score), max(r.score) FROM facts t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float4) ON true WHERE day < 200 GROUP BY 1') AS negative_zero_float4;
-- only zeros: the value is the same, the sign of a result that is all zeros is not kept
SELECT runq('SELECT r.kind, sum(r.score) + 0, count(r.score) FROM facts t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true WHERE id IN (100, 1500) GROUP BY 1', false)
     = runq('SELECT r.kind, sum(r.score) + 0, count(r.score) FROM facts t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true WHERE id IN (100, 1500) GROUP BY 1', true) AS only_zeros;
DROP TABLE facts;

-- ---- a list longer than a unit's limit (2^20 elements): the row is read again whole ----
CREATE TABLE lists (id int PRIMARY KEY, kind text, day int, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO lists SELECT i, 'cat_' || chr(97 + i % 3), i / 10,
	jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 'cat_a', 'score', i % 9), jsonb_build_object('kind', 'cat_b', 'score', i % 3)))
FROM generate_series(1, 2000) i;
INSERT INTO lists SELECT 5000, 'cat_a', 500, jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('kind', 'cat_a', 'score', g % 5)) FROM generate_series(1, 1100000) g));
CREATE INDEX lists_col ON lists USING sift_col (id, kind, day, payload) WITH (paths = 'payload.parts[]{kind text, score float8}');
VACUUM ANALYZE lists;
SELECT pushed('SELECT r.kind, avg(r.score), count(*) FROM lists t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true GROUP BY 1') AS long_list_pushed;
SELECT same('SELECT r.kind, avg(r.score), count(*) FROM lists t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true WHERE day > 400 OR day < 10 GROUP BY 1') AS long_list;
SELECT same('SELECT r.kind, avg(r.score), count(*) FROM lists t JOIN LATERAL jsonb_to_recordset(t.payload -> $$parts$$) r(kind text, score float8) ON true WHERE day < 10 GROUP BY 1') AS long_list_dropped;
DROP TABLE lists;

-- ---- an index is built over documents with every kind of value its paths cannot store ----
CREATE FUNCTION build_over(ty text, bad jsonb) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	DROP TABLE IF EXISTS build_t;
	CREATE TABLE build_t (id int PRIMARY KEY, kind text, day int, payload jsonb) WITH (autovacuum_enabled = false);
	INSERT INTO build_t SELECT i, 'cat_' || chr(97 + i % 3), i / 10,
		jsonb_build_object('val', i % 11, 'metrics', jsonb_build_object('m_a', i % 7, 'm_b', i % 5), 'parts', jsonb_build_array(jsonb_build_object('kind', 'cat_a', 'score', i % 9)))
	FROM generate_series(1, 1000) i;
	UPDATE build_t SET payload = jsonb_set(payload, '{val}', bad) WHERE id IN (100, 500);
	UPDATE build_t SET payload = jsonb_set(payload, '{metrics,m_a}', bad) WHERE id = 200;
	UPDATE build_t SET payload = jsonb_set(payload, '{parts,0,score}', bad) WHERE id = 300;
	EXECUTE format('CREATE INDEX build_col ON build_t USING sift_col (id, kind, day, payload) WITH (paths = ''payload.val %s, payload.metrics.* %s, payload.parts[]{kind text, score %s}'')', ty, ty, ty);
	RETURN 'built';
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;

SET client_min_messages = warning;
SELECT ty, bad, r.build
FROM (VALUES ('float8'), ('float4'), ('int8'), ('int4'), ('int2'), ('bool'), ('text')) t(ty),
     (VALUES ('"7"'), ('"n/a"'), ('""'), ('true'), ('{"a": 1}'), ('[1, 2]'), ('null'), ('"-0"'), ('1e300'), ('7.5'), ('-3000000000')) b(bad),
     LATERAL (SELECT build_over(ty, bad::jsonb) AS build) r
WHERE r.build <> 'built';

RESET client_min_messages;
DROP TABLE build_t;
DROP FUNCTION build_over(text, jsonb);
DROP FUNCTION eq(text, text, text);
DROP FUNCTION pushed(text);
DROP FUNCTION answers(text);
DROP FUNCTION same(text);
DROP FUNCTION runq(text, bool);
DROP EXTENSION pg_sift;
