-- A numeric IN list is compared constant by constant up to 64 constants (sift.test_limits num_in_list lowers it so that short lists reach the other path); a longer one is
-- a hash set of the scaled values, with NaN equal to NaN and to no number, NULL as in the normal plan, and constants no stored value can equal (a scale the column does not
-- have, a number beyond its digits) left out. Every list runs as the scan and as the normal plan, at the limit and one constant above it.
CREATE EXTENSION pg_sift;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE nn (id int PRIMARY KEY, g int, n numeric(12,2), w numeric(10,0)) WITH (autovacuum_enabled = false);
INSERT INTO nn
SELECT i, i % 7, CASE WHEN i % 97 = 0 THEN 'NaN' WHEN i % 11 = 0 THEN NULL ELSE round(((i * 37) % 4001 - 2000) / 7.0, 2) END, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 31) % 500 - 250 END
FROM generate_series(1, 6000) i;
CREATE INDEX nn_col ON nn USING sift_col (id, g, n, w);
VACUUM (FREEZE, ANALYZE) nn;

-- the lists: the values of the table, ones it does not have, NaN, NULL, a scale the column lacks, a number beyond its digits
CREATE TABLE lists (name text, items text);
INSERT INTO lists VALUES
	('present', '-7.25, 1.5, 2.43, 0.29'),
	('absent', '1000.5, -999.25, 3.33'),
	('with NaN', '-7.25, 1.5, ''NaN'', 2.43'),
	('only NaN', '''NaN'', ''NaN'', ''NaN'''),
	('with NULL', '-7.25, NULL, 2.43, 0.29'),
	('finer scale', '-7.25, 1.005, 2.43, 0.299'),
	('beyond digits', '-7.25, 1e30, -1e30, 2.43');

CREATE FUNCTION plan_has(q text, pat text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false; hit bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan := true; END IF;
		IF l ~ pat THEN hit := true; END IF;
	END LOOP;
	RETURN scan AND hit;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND b >= 0;
END $$;

-- each list as IN and NOT IN, with the limit below the list (a hash set), at the list (comparisons) and the default (comparisons)
CREATE FUNCTION runs(lim text) RETURNS TABLE (name text, op text, as_set bool, ok bool) LANGUAGE plpgsql AS $$
DECLARE l record; o text; q text;
BEGIN
	EXECUTE format('SET sift.test_limits = %L', lim);
	FOR l IN SELECT * FROM lists ORDER BY name LOOP
		FOREACH o IN ARRAY ARRAY['IN', 'NOT IN'] LOOP
			q := format('SELECT g, count(*) AS c, sum(id) AS s FROM nn WHERE n %s (%s) GROUP BY g', o, l.items);
			name := l.name; op := o;
			as_set := plan_has(q, 'hash set');
			ok := same(q);
			RETURN NEXT;
		END LOOP;
	END LOOP;
	RESET sift.test_limits;
END $$;
SELECT * FROM runs('num_in_list=3');
SELECT * FROM runs('num_in_list=4');
SELECT * FROM runs('num_in_list=64');

-- the lists of 64, 65 and 200 constants of the default limit, from the values of the table and NaN
SELECT n_items, plan_has(q, 'hash set') AS as_set, same(q) AS ok
FROM (SELECT k AS n_items, format('SELECT g, count(*) AS c, sum(id) AS s FROM nn WHERE n IN (%s) GROUP BY g', string_agg(v, ', ')) AS q
      FROM (SELECT k, CASE WHEN j = 1 THEN '''NaN''' ELSE (SELECT coalesce(quote_literal(n::text), '''0.5''') FROM nn WHERE id = j * 3) END AS v
            FROM unnest(ARRAY[64, 65, 200]) k, LATERAL generate_series(1, k) j) x
      GROUP BY k) f ORDER BY 1;
-- a list on a column of whole numbers, and one of 300 distinct values with a NaN
SELECT same('SELECT g, count(*) FROM nn WHERE w NOT IN (' || (SELECT string_agg((j * 3 - 250)::text, ', ') FROM generate_series(1, 90) j) || ') GROUP BY g') AS whole_numbers;
SELECT same('SELECT g, count(*) FROM nn WHERE n IN (' || (SELECT string_agg(quote_literal(n::text), ', ') FROM (SELECT DISTINCT n FROM nn WHERE n IS NOT NULL LIMIT 300) d) || ') AND id < 3000 GROUP BY g') AS many_distinct;

DROP FUNCTION runs(text), same(text), plan_has(text, text);
DROP TABLE lists, nn;
DROP EXTENSION pg_sift;
