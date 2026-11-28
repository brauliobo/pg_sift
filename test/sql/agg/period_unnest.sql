-- A range-period join (fact.ts <@ periods.range) beside an unnest of the fact row: each row is multiplied by the periods that hold its date, independently of its elements, in SiftAggV6, on both
-- formats. Overlapping periods count a row once per period; an empty or NULL range holds no date, a NULL date is in no period. A LEFT join of the periods is the normal plan's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

\ir test/common/sql/agg/base.sql

\ir test/common/sql/agg/same.sql

CREATE FUNCTION used(q text) RETURNS bool LANGUAGE sql AS $$ SELECT coalesce(bool_or(used), false) FROM sift.explain_why(q) $$;

CREATE TABLE events (id int, parent_id int, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, i % 50, CASE WHEN i % 97 = 0 THEN NULL ELSE date '2024-01-01' + (hashint4(i) & 1023) % 730 END,
	jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('kind', 'cat_' || chr(97 + (i + j) % 4), 'score', 1 + (i * j) % 10)) FROM generate_series(1, 1 + i % 3) j),
		'tags', jsonb_build_array('t' || i % 5, 't' || i % 7))
FROM generate_series(1, 8000) i;
INSERT INTO events VALUES (9001, 1, '2025-07-01', '{"parts": [{"kind": "cat_a", "score": "3"}, {}], "tags": ["t1", null]}'), (9002, 2, '2025-07-02', '{"parts": []}'), (9003, 3, '2024-07-03', NULL);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, doc) WITH (paths = 'doc.parts[]{kind text, score float8}, doc.tags[] text');
CREATE TABLE periods (period text, range daterange);
INSERT INTO periods VALUES ('cur', daterange('2025-06-01', '2025-12-31', '[]')), ('prev', daterange('2024-06-01', '2024-12-31', '[]')), ('y25', daterange('2025-01-01', '2026-01-01')),
	('none', 'empty'), ('nul', NULL);
CREATE TABLE dims (id int PRIMARY KEY, tier text);
INSERT INTO dims SELECT g, 'tier_' || chr(97 + g % 3) FROM generate_series(0, 49) g;
VACUUM (FREEZE, ANALYZE) events, periods, dims;

CREATE TABLE q (n int, sql text);
INSERT INTO q VALUES
	(1, $$SELECT p.period, o.kind, avg(o.score), count(*) FROM events e JOIN periods p ON e.ts <@ p.range CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(kind text, score float8) GROUP BY 1, 2$$),
	(2, $$SELECT p.period, count(*), sum(o.score) FROM events e JOIN periods p ON e.ts <@ p.range CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(kind text, score float8) WHERE o.kind = 'cat_a' GROUP BY 1$$),
	(3, $$SELECT p.period, t, count(*) FROM events e JOIN periods p ON e.ts <@ p.range CROSS JOIN LATERAL jsonb_array_elements_text(e.doc -> 'tags') t GROUP BY 1, 2$$),
	(4, $$SELECT p.period, e2 ->> 'kind', count(*), sum((e2 ->> 'score')::float8) FROM events e JOIN periods p ON e.ts <@ p.range CROSS JOIN LATERAL jsonb_array_elements(e.doc -> 'parts') e2 GROUP BY 1, 2$$),
	(5, $$SELECT p.period, d.tier, o.kind, count(*) FROM events e JOIN periods p ON e.ts <@ p.range JOIN dims d ON d.id = e.parent_id
	      CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(kind text, score float8) GROUP BY 1, 2, 3$$),
	(6, $$SELECT p.period, o.kind, count(*) FROM events e JOIN (VALUES ('cur', daterange(DATE '2025-06-01', DATE '2025-12-31', '[]')), ('prev', daterange(DATE '2024-06-01', DATE '2024-12-31', '[]'))) p(period, range)
	      ON e.ts <@ p.range CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(kind text, score float8) GROUP BY 1, 2$$),
	(7, $$SELECT p.period, count(o.kind), count(*) FROM events e JOIN periods p ON e.ts <@ p.range LEFT JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(kind text, score float8) ON true GROUP BY 1$$),
	-- a LEFT join of the periods keeps the rows of no period
	(8, $$SELECT p.period, o.kind, count(*) FROM events e LEFT JOIN periods p ON e.ts <@ p.range CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(kind text, score float8) GROUP BY 1, 2$$);
SELECT n, same(sql), pushed(sql) AS scan, used(sql) FROM q ORDER BY n;

DROP TABLE q, events, periods, dims;
DROP FUNCTION used(text), runq(text, bool), same(text), pushed(text), outcome(text, bool), runs_in_scan(text);
DROP EXTENSION pg_sift;
