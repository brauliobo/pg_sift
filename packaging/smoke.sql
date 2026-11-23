-- Smoke test of an installed package: psql -v from=<version to create> -v to=<expected final version>.
CREATE FUNCTION pg_temp.explain(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN FOR l IN EXECUTE 'EXPLAIN ' || q LOOP RETURN NEXT l; END LOOP; END $$;

CREATE PROCEDURE pg_temp.check_sift() LANGUAGE plpgsql AS $$
DECLARE plan text;
BEGIN
	ASSERT (SELECT array_agg(id ORDER BY id) FROM docs WHERE body @@> 'battery friendly') = '{1,2}';
	ASSERT (SELECT count(*) FROM docs WHERE body @@> 'friendly') = 3;
	ASSERT (SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM docs
			WHERE body @@> 'case OR clip' ORDER BY s DESC LIMIT 10) top) = '{2,1,5}';
	SELECT string_agg(l, E'\n') INTO plan FROM
		(SELECT * FROM pg_temp.explain('SELECT id FROM docs WHERE body @@> ''case'' ORDER BY sift.score(ctid) DESC LIMIT 3') l) p;
	ASSERT plan ~ 'Sift', plan;
END $$;

CREATE EXTENSION pg_sift VERSION :'from';
CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs VALUES
  (1, 'Battery was great and the support was friendly. The case was squeaky.'),
  (2, 'Friendly support, no battery, but a sturdy case and clip.'),
  (3, 'A good résumé is short. Cafés near the office help.'),
  (4, 'Runners were running; she runs every day.'),
  (5, 'The headphones were squeaky at night, squeak everywhere. Friendly support though. Case.');
CREATE INDEX docs_body ON docs USING sift (body);
CALL pg_temp.check_sift();

ALTER EXTENSION pg_sift UPDATE;
SET sift.simd = 'scalar';
CALL pg_temp.check_sift();
RESET sift.simd;
SET smoke.version = :'to';
DO $$ BEGIN ASSERT (SELECT extversion FROM pg_extension WHERE extname = 'pg_sift') = current_setting('smoke.version'); END $$;
CALL pg_temp.check_sift();
DO $$ BEGIN ASSERT to_regprocedure('sift.table_find_stale_versions(regclass,integer)') IS NOT NULL; END $$;

CREATE TABLE facts (id int, g int, v int);
INSERT INTO facts SELECT i, i % 7, i FROM generate_series(1, 4000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, g, v);
CREATE TABLE tfacts (id int, g int, v int) USING sift_table;
INSERT INTO tfacts SELECT id, g, v FROM facts;
VACUUM (ANALYZE) facts, tfacts;
CREATE PROCEDURE pg_temp.check_agg() LANGUAGE plpgsql AS $$
DECLARE
	want text := (SELECT string_agg(g || ':' || c || ':' || s, ',' ORDER BY g) FROM (SELECT i % 7 AS g, count(*) AS c, sum(i) AS s FROM generate_series(1, 4000) i GROUP BY 1) x);
	tbl text; got text;
BEGIN
	SET LOCAL cpu_tuple_cost = 0.5;
	SET LOCAL max_parallel_workers_per_gather = 0;
	FOREACH tbl IN ARRAY ARRAY['facts', 'tfacts'] LOOP
		ASSERT (SELECT string_agg(l, E'\n') FROM pg_temp.explain(format('SELECT g, count(*), sum(v) FROM %s GROUP BY g', tbl)) l) ~ 'Custom Scan \(SiftAgg', tbl;
		EXECUTE format('SELECT string_agg(g || '':'' || c || '':'' || s, '','' ORDER BY g) FROM (SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g) x', tbl) INTO got;
		ASSERT got = want, tbl;
	END LOOP;
END $$;
CALL pg_temp.check_agg();
SET sift.simd = 'scalar';
CALL pg_temp.check_agg();
RESET sift.simd;
