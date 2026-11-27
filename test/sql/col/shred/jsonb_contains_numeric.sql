-- JSONB compares numbers by value: doc @> '{"n": 3}' holds for 3.0, 3.00 and 3e0, and '{"n": 3.0}' for 3. An integer leaf of a sift_col index holds 3 but not 3.0 (it keeps it as
-- an exception, for `(doc ->> 'n')::int4` to raise): the containment, the equality and their negation answer as the heap does for the numbers held, kept aside or out of the range,
-- with and without declared paths, before and after maintenance (S118-23).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

\ir test/common/sql/agg/base.sql

CREATE TABLE jn (id int8, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jn SELECT i, jsonb_build_object('n', i % 7, 'o', jsonb_build_object('n', i % 5), 's', 'a', 'big', i) FROM generate_series(1, 3000) i;
CREATE TABLE jp (LIKE jn) WITH (autovacuum_enabled = false);
INSERT INTO jp SELECT * FROM jn;
CREATE INDEX jn_col ON jn USING sift_col (id, doc);
CREATE INDEX jp_col ON jp USING sift_col (id, doc) WITH (paths = 'doc.n int4, doc.o.n int8, doc.big int8, doc.s text');

CREATE TABLE odd (id int8 GENERATED ALWAYS AS IDENTITY (START 100000), doc jsonb);
INSERT INTO odd (doc) VALUES ('{"n": 3.0, "o": {"n": 3.00}}'), ('{"n": 3.00, "s": "a"}'), ('{"n": 3e0, "o": {"n": 3e0}}'), ('{"n": -0.0, "o": {"n": 0.00}}'),
	('{"n": 0.00}'), ('{"n": 1e2, "o": {"n": 100.0}}'), ('{"n": 100.5}'), ('{"n": 3.0000000000000000001}'), ('{"n": "3"}'), ('{"n": null}'), ('{"n": [3]}'),
	('{"n": {"n": 3}}'), ('{"big": 9223372036854775807.0}'), ('{"big": 92233720368547758070}'), ('{"big": -9223372036854775808.000}'), ('{"n": 2147483648.0}'),
	('[{"n": 3.0}]'), ('3.0'), (NULL);
INSERT INTO jn SELECT * FROM odd;
INSERT INTO jp SELECT * FROM odd;
DROP TABLE odd;
ANALYZE jn, jp;

CREATE TABLE q (n int, sql text);
INSERT INTO q VALUES
	(1, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 3}'$$),
	(2, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 3.0}'$$),
	(3, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 3.00}'$$),
	(4, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 3e0}'$$),
	(5, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 0}'$$),
	(6, $$SELECT count(*) FROM %s WHERE doc @> '{"n": -0.0}'$$),
	(7, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 1e2}'$$),
	(8, $$SELECT count(*) FROM %s WHERE doc @> '{"o": {"n": 3}}'$$),
	(9, $$SELECT count(*) FROM %s WHERE doc @> '{"o": {"n": 100}, "n": 100}'$$),
	(10, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 3, "s": "a"}'$$),
	(11, $$SELECT count(*) FROM %s WHERE doc @> '{"big": 9223372036854775807}'$$),
	(12, $$SELECT count(*) FROM %s WHERE doc @> '{"big": -9223372036854775808}'$$),
	(13, $$SELECT count(*) FROM %s WHERE doc @> '{"n": 2147483648}'$$),
	(14, $$SELECT count(*) FROM %s WHERE '{"n": 3.0}' <@ doc$$),
	(15, $$SELECT count(*) FROM %s WHERE doc -> 'n' = '3'$$),
	(16, $$SELECT count(*) FROM %s WHERE doc -> 'o' -> 'n' = '3.0'$$),
	(17, $$SELECT count(*) FROM %s WHERE NOT (doc @> '{"n": 3}')$$),
	(18, $$SELECT doc -> 'n' AS n, count(*) FROM %s WHERE doc @> '{"n": 3}' GROUP BY 1$$);

CREATE FUNCTION check_all(phase text) RETURNS TABLE (phase text, t text, n int, heap text, same bool, pushed bool) LANGUAGE sql AS $$
	SELECT phase, t, q.n, outcome(format(q.sql, t), false), outcome(format(q.sql, t), false) = outcome(format(q.sql, t), true), runs_in_scan(format(q.sql, t))
	FROM q, unnest(ARRAY['jn', 'jp']) t ORDER BY t, q.n $$;

SELECT * FROM check_all('built');
VACUUM jn, jp;
SELECT count(*) >= 0 AS maintained FROM sift.maintain('jn_col');
SELECT count(*) >= 0 AS maintained FROM sift.maintain('jp_col');
SELECT * FROM check_all('maintained');

DROP FUNCTION check_all(text);
DROP TABLE q, jn, jp;
DROP FUNCTION runq(text, bool), pushed(text), outcome(text, bool), runs_in_scan(text);
DROP EXTENSION pg_sift;
