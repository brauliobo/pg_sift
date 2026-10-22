\set ECHO none
-- Shared by the search tests that print a plan (\ir test/sift_plan.sql): sift_plan(q) is the EXPLAIN (COSTS OFF) text of q.
CREATE FUNCTION sift_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;
\set ECHO all
