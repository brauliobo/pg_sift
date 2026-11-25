-- A backend keeps the library across DROP EXTENSION and CREATE EXTENSION, so whatever it remembered of the catalog of the dropped extension must go with the objects: the
-- access methods, the functions and the operators come back under new oids. The same objects are made, read and changed before and after, in one session: the answers are the same.
CREATE FUNCTION plan_has(q text, node text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF position(node in l) > 0 THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

\ir test/common/sql/core/ext_recreate.sql
DROP EXTENSION pg_sift CASCADE;
\ir test/common/sql/core/ext_recreate.sql

DROP FUNCTION plan_has(text, text);
DROP TABLE re_t, re_doc, re_ev, re_pairs;
DROP EXTENSION pg_sift;
