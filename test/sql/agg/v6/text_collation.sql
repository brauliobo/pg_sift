-- The table of strings of the aggregation (src/agg/ht/ht.c) groups them by their bytes. That is PostgreSQL's grouping for every deterministic collation, and not
-- for a nondeterministic one, which equates strings of different bytes: such a table is refused (0A000), so that the groups of PostgreSQL are never split.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE COLLATION agg_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE COLLATION agg_ai (provider = icu, locale = 'und-u-ks-level1', deterministic = false);

CREATE TABLE ct (v text);
INSERT INTO ct VALUES ('apple'), ('Apple'), ('APPLE'), ('apple'), ('étoile'), ('etoile'), ('b'), ('B'), (''), (''), ('a long string of more than twelve bytes'),
                      ('A LONG STRING OF MORE THAN TWELVE BYTES'), ('a long string of more than twelve bytes');

CREATE FUNCTION pg_groups(coll text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (SELECT v COLLATE %I FROM ct GROUP BY 1) AS g', coll) INTO n;
	RETURN n;
END $$;

CREATE FUNCTION engine_groups(coll oid) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	RETURN sift._test_ht_text_groups((SELECT array_agg(v) FROM ct), coll)::text;
EXCEPTION WHEN feature_not_supported THEN
	RETURN 'refused ' || SQLSTATE;
END $$;

SELECT collname, collisdeterministic AS deterministic, pg_groups(collname) AS postgres_groups, engine_groups(oid) AS engine_groups,
       (collisdeterministic AND engine_groups(oid)::bigint = pg_groups(collname)) OR (NOT collisdeterministic AND engine_groups(oid) = 'refused 0A000') AS as_postgres
FROM pg_collation
WHERE collname IN ('C', 'POSIX', 'ucs_basic', 'default', 'und-x-icu', 'agg_ci', 'agg_ai') AND collnamespace IN ('pg_catalog'::regnamespace, 'public'::regnamespace)
ORDER BY collname;

-- a column that has no collation (not a text column) is no string table either
SELECT engine_groups(0) AS no_collation;

DROP FUNCTION engine_groups(oid);
DROP FUNCTION pg_groups(text);
DROP TABLE ct;
DROP COLLATION agg_ci, agg_ai;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
