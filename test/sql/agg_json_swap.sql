-- A query that reads a table with a column index calls sift.json_build_object where it names json_build_object (the same text, the type
-- lookups of an argument made once per call site); sift.enable_agg = off keeps the core function. A query that reads no such table, a call with no
-- argument, jsonb_build_object, and a role that may not execute sift's function keep the core one.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

CREATE TABLE jt (id int PRIMARY KEY, g int, label text, price numeric, day date);
INSERT INTO jt SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'l"' || i % 5 || E'\n' END, i / 4.0, '2026-01-01'::date + i % 30 FROM generate_series(1, 3000) i;
CREATE INDEX jt_col ON jt USING sift_col (id, g, label, price, day);
CREATE TABLE jp AS SELECT * FROM jt;
VACUUM ANALYZE jt;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE s_q (name text, q text);
INSERT INTO s_q VALUES
 ('object per group', $q$SELECT g, json_build_object('g', g, 'n', count(*), 'avg', avg(price), 'first', min(day), 'label', min(label))::text AS o FROM jt GROUP BY g$q$),
 ('object above a sub-select', $q$SELECT json_build_object('g', g, 'n', n)::text AS o FROM (SELECT g, count(*) AS n FROM jt GROUP BY g) s$q$),
 ('object and a sublink over a CTE', $q$WITH c AS (SELECT g, count(*) AS n FROM jt GROUP BY g) SELECT json_build_object('top', (SELECT max(n) FROM c), 'g', g)::text AS o FROM c$q$),
 ('object in a window', $q$SELECT json_build_object('g', g, 'prev', lag(n) OVER (ORDER BY g))::text AS o FROM (SELECT g, count(*) AS n FROM jt GROUP BY g) s$q$),
 ('VARIADIC array', $q$SELECT json_build_object(VARIADIC ARRAY['g', g::text])::text AS o FROM jt WHERE id < 20$q$),
 ('no argument', $q$SELECT g, json_build_object()::text AS o, count(*) AS n FROM jt GROUP BY g$q$),
 ('jsonb_build_object', $q$SELECT g, jsonb_build_object('n', count(*))::text AS o FROM jt GROUP BY g$q$),
 ('table with no column index', $q$SELECT g, json_build_object('g', g, 'n', count(*))::text AS o FROM jp GROUP BY g$q$);

-- the call the plan makes, and the rows of the plan with and without the setting
CREATE FUNCTION s_call(qq text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; m text[]; r text[] := '{}';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || qq LOOP
		FOR m IN SELECT regexp_matches(l, '((?:sift\.)?json_build_object)\(', 'g') LOOP
			IF NOT m[1] = ANY (r) THEN r := r || m[1]; END IF;
		END LOOP;
	END LOOP;
	RETURN CASE WHEN r = '{}' THEN 'none' ELSE array_to_string(r, ',') END;
END $$;

CREATE FUNCTION s_diff(qq text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg_temp(qq) $$;

SELECT name, s_call(q) AS call, s_diff(q) AS differing_rows FROM s_q ORDER BY name;
SET sift.enable_agg = off;
SELECT name, s_call(q) AS call FROM s_q WHERE name = 'object per group';
SET sift.enable_agg = on;

-- a role that may not execute sift's function runs the statement with the core one
REVOKE EXECUTE ON FUNCTION sift.json_build_object("any") FROM PUBLIC;
CREATE ROLE jsw_plain;
GRANT SELECT ON jt, s_q TO jsw_plain;
SET ROLE jsw_plain;
SELECT s_call(q) AS call FROM s_q WHERE name = 'object per group';
SELECT json_build_object('g', g, 'n', count(*)) AS o FROM jt GROUP BY g ORDER BY g LIMIT 2;
RESET ROLE;
GRANT EXECUTE ON FUNCTION sift.json_build_object("any") TO PUBLIC;
DROP OWNED BY jsw_plain;
DROP ROLE jsw_plain;

DROP FUNCTION s_call(text), s_diff(text);
DROP TABLE s_q, jt, jp;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
