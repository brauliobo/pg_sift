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
SET smoke.version = :'to';
DO $$ BEGIN ASSERT (SELECT extversion FROM pg_extension WHERE extname = 'pg_sift') = current_setting('smoke.version'); END $$;
CALL pg_temp.check_sift();
