-- The top-N of a text column of a sift_table reads the byte order the column groups store with a dictionary of 1,024 strings or more
-- (sift.table_dict_lookup, column format minor 14): the units are ordered by exact codes, and the rows are those of the normal plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.table_dict_lookup = on;
SET sift.agg_force = on;

CREATE TABLE tn (id int, k text) USING sift_table;
INSERT INTO tn SELECT i, md5(i::text) FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) tn;

CREATE FUNCTION exact_units(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Top-N Units By Order Code: exact %' THEN RETURN split_part(split_part(l, 'exact ', 2), ',', 1)::int > 0; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION top_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

SELECT exact_units('SELECT k FROM tn ORDER BY k COLLATE "C" LIMIT 5') AS by_order,
	top_rows('SELECT k, id FROM tn ORDER BY k COLLATE "C" LIMIT 25', true) = top_rows('SELECT k, id FROM tn ORDER BY k COLLATE "C" LIMIT 25', false) AS same;

DROP FUNCTION exact_units(text), top_rows(text, bool);
DROP TABLE tn;
DROP EXTENSION pg_sift;
