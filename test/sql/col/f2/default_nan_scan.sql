-- A column added with a default that a stored value cannot hold exactly (a numeric NaN) is read by the aggregate scan from the parts that were written before the column existed as the
-- heap reads it: the NaN counts, the sum that meets it is NaN, the minimum of a text default and the length of a wide one are the default. The relation cache is invalidated between the
-- ALTER TABLE and the reads, so that the default is read from the catalog again.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE d (id int, a int) USING sift_table;
INSERT INTO d SELECT i, i FROM generate_series(1, 1000) i;
SELECT sift.table_maintain('d') IS NOT NULL AS maintained;
ALTER TABLE d ADD COLUMN tag text DEFAULT 'legacy_default_value';
ALTER TABLE d ADD COLUMN score numeric(10,2) DEFAULT 'NaN';
ALTER TABLE d ADD COLUMN wide_txt text DEFAULT repeat('w', 20000);
ANALYZE d;
COMMENT ON TABLE d IS 'invalidates the relation cache';

CREATE FUNCTION d_nodes(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'SiftAggV6' THEN RETURN btrim(l); END IF;
	END LOOP;
	RETURN 'not SiftAggV6';
END $$;

\set q 'SELECT count(tag) AS tags, min(tag) AS tag, count(score) AS scores, sum(score) AS score_sum, min(length(wide_txt)) AS wide FROM d'
SELECT d_nodes(:'q');
:q;
SET sift.enable_agg = off;
:q;
SET sift.enable_agg = on;
SELECT count(*) FILTER (WHERE score = 'NaN') AS nan_rows, count(*) FILTER (WHERE tag = 'legacy_default_value') AS tag_rows FROM d;
SELECT score, count(*) FROM d GROUP BY score;

DROP TABLE d;
DROP FUNCTION d_nodes(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
