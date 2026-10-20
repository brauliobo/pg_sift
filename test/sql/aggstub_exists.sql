-- Existential tests over the elements of an array of objects (EXISTS, jsonb_path_exists, @>) on the test column source: the plan takes
-- the shapes it proves and its rows are the ones of the normal plan for NULL, JSON null, empty and odd documents; the near misses
-- keep the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;

-- doc: any shape (nullable); dc: arrays of objects only (what jsonb_to_recordset accepts), NOT NULL
CREATE TEMP TABLE pat (k int, doc jsonb, dc jsonb);
INSERT INTO pat VALUES
 (0, NULL, '[]'),
 (1, 'null', '[{"type":"reply","role":"staff","n":1}]'),
 (2, '[]', '[{"type":"reply","role":null,"n":2},{"type":"x"}]'),
 (3, '[null]', '[{"type":"","n":3}]'),
 (4, '[1, 2]', '[{"role":"staff"}]'),
 (5, '[{}]', '[{"type":"réplica","role":"staff","n":-1},{"type":"reply","role":"customer","n":5}]'),
 (6, '[{"type":"reply"}]', '[{"type":"reply","n":2},{"type":"reply","role":"staff"}]'),
 (7, '[{"type":"reply","role":"staff"},{"type":"x","role":null}]', '[]'),
 (8, '[{"type":"","role":"staff","n":3},{"type":"réplica","n":-1}]', '[{"type":"x","role":"staff","n":4}]'),
 (9, '[{"type":"a"},{"role":"b"}]', '[{"type":null,"role":"b"}]');
-- r, r8: columns of the row an element is compared with (int4 and bigint, NULL in one row of seven)
CREATE TABLE d (id int PRIMARY KEY, g int, r int, r8 bigint, doc jsonb, dc jsonb NOT NULL, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO d SELECT n * 10 + p.k, n % 3, CASE WHEN (n + p.k) % 7 = 0 THEN NULL ELSE (n + p.k) % 7 - 1 END,
	CASE WHEN (n + p.k) % 7 = 0 THEN NULL WHEN (n + p.k) % 11 = 0 THEN 5000000000 ELSE (n + p.k) % 6 END, p.doc, p.dc, repeat('x', 200)
FROM generate_series(1, 150) n, pat p;
CREATE INDEX d_col ON d USING sift_colstub (id, g, r, r8, doc, dc)
	WITH (paths = 'doc: []{type text, role text, n int4}; dc: []{type text, role text, n int4}');
VACUUM ANALYZE d;

-- 'pushed' when SiftAgg ran (the sequential scan is off so that the cost does not decide), 'plain' when it did not; then whether the
-- rows are the normal plan's
CREATE FUNCTION forced(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; used bool := false; diffs bigint;
BEGIN
	SET enable_seqscan = off;
	SET sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP used := used OR l LIKE '%SiftAgg%'; END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	RESET enable_seqscan;
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO diffs FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN CASE WHEN used THEN 'pushed' ELSE 'plain' END || CASE WHEN diffs = 0 THEN ', same rows' ELSE ', ' || diffs || ' DIFFERENT ROWS' END;
END $$;

-- jsonb_path_exists and @?: strings, integers, && and ||, the empty string, a non-ASCII string, NULL and odd documents
SELECT forced($$SELECT g, count(*) AS n, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.type == "reply" && @.role == "staff")')) AS f FROM d GROUP BY g$$) AS path_and;
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @? '$[*] ? (@.type == "" || @.n < 0)') AS f FROM d GROUP BY g$$) AS path_or_empty_string;
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (3 == @.n)')) AS f FROM d GROUP BY g$$) AS path_integer;
SELECT forced($$SELECT g, count(*) AS n FROM d WHERE jsonb_path_exists(doc, '$[*] ? (@.type == "réplica")') GROUP BY g$$) AS path_where;

-- @>: every object of the right side is contained by some element, not necessarily the same one
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @> '[{"type": "reply"}]') AS one, count(*) FILTER (WHERE doc @> '[{"role": "staff"}, {"type": "réplica"}]') AS two,
				count(*) FILTER (WHERE '[{"n": 3, "role": "staff"}]' <@ doc) AS commuted FROM d GROUP BY g$$) AS contains;

-- EXISTS and NOT EXISTS: never NULL, an absent or empty array has no element
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, role text, n int) WHERE e.type = 'reply' AND e.role = 'staff')) AS yes,
				count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, n int) WHERE e.n > 1 OR e.type IS NULL)) AS no FROM d GROUP BY g$$) AS exists_not_exists;
SELECT forced($$SELECT count(*) FROM d WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(d.dc) e WHERE e->>'role' = 'staff')$$) AS exists_elements;

-- NOT of a test that is NULL for a NULL document only on a NOT NULL column
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT jsonb_path_exists(dc, '$[*] ? (@.type == "reply")')) AS f FROM d GROUP BY g$$) AS not_path_not_null;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT dc @> '[{"role": "staff"}]') AS f FROM d GROUP BY g$$) AS not_contains_not_null;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT jsonb_path_exists(doc, '$[*] ? (@.type == "reply")')) AS f FROM d GROUP BY g$$) AS not_path_nullable;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT doc @> '[{"type": "reply"}]') AS f FROM d GROUP BY g$$) AS not_contains_nullable;

-- an element against a column of its own row: every operator on either side, the int4 member against a bigint column, the NULL
-- column and the NULL member are unknown, NOT EXISTS keeps the rows with no comparison that is true
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r)) AS eq,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n <> d.r)) AS ne,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n < d.r)) AS lt,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE d.r <= e.n)) AS le,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n > d.r)) AS gt,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n >= d.r)) AS ge FROM d GROUP BY g$$) AS exists_outer_column;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r8)) AS eq,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n < d.r8)) AS lt FROM d GROUP BY g$$) AS exists_outer_column_bigint;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r)) AS none_eq,
				count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n IS NULL OR e.n > d.r)) AS none_null_or_gt FROM d GROUP BY g$$) AS not_exists_outer_column;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, n int) WHERE e.type = 'reply' AND e.n = d.g)) AS f FROM d GROUP BY g$$) AS exists_outer_column_same_element;
-- PostgreSQL turns the negated conjunction into the OR of the negated comparisons, which are unknown for a NULL as the others are
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, n int) WHERE NOT (e.type = 'reply' AND e.n = d.r))) AS f FROM d GROUP BY g$$) AS exists_outer_not_and;

-- near misses keep the normal plan: != (a JSON null member is "not equal" in jsonpath), a float literal, a string against an integer,
-- a JSON null or a float in a containment, an element compared with an expression of the outer row, a column of another type class and a limit that is no constant
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.type != "reply")')) AS f FROM d GROUP BY g$$) AS path_not_equal;
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n > 1.5)')) AS f FROM d GROUP BY g$$) AS path_float;
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n == "3")')) AS f FROM d GROUP BY g$$) AS path_string_vs_integer;
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @> '[{"role": null}]') AS f FROM d GROUP BY g$$) AS contains_null;
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @> '[{"n": 2.5}]') AS f FROM d GROUP BY g$$) AS contains_float;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.g + 1)) AS f FROM d GROUP BY g$$) AS exists_outer_expression;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.g::float8)) AS f FROM d GROUP BY g$$) AS exists_outer_other_class;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r LIMIT (SELECT 1))) AS f FROM d GROUP BY g$$) AS exists_outer_limit;
-- the shared database of a plain installcheck goes on with the next test
DROP TABLE pat;
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
