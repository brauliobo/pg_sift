-- sift.json_build_object in PL/pgSQL: a key read from a variable is the value it has at each call, and a NULL one is an error, as json_build_object answers
CREATE EXTENSION pg_sift;
CREATE FUNCTION jbp_keys(fn text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE k text; i int := 0; r text;
BEGIN
	FOREACH k IN ARRAY ARRAY['k1', 'k2', NULL] LOOP
		i := i + 1;
		BEGIN
			IF fn = 'sift' THEN
				r := sift.json_build_object(k, i)::text;
			ELSE
				r := json_build_object(k, i)::text;
			END IF;
		EXCEPTION WHEN OTHERS THEN
			r := 'error ' || SQLSTATE;
		END;
		RETURN NEXT r;
	END LOOP;
END $$;
SELECT * FROM jbp_keys('sift') AS sift;
SELECT * FROM jbp_keys('core') AS core;
-- a constant key is the same at every call
CREATE FUNCTION jbp_const() RETURNS SETOF text LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..3 LOOP
		RETURN NEXT sift.json_build_object('k', i, 'n', i * 10)::text;
	END LOOP;
END $$;
SELECT * FROM jbp_const();
DROP FUNCTION jbp_keys(text);
DROP FUNCTION jbp_const();
DROP EXTENSION pg_sift;
