-- A sum over a numeric column without a precision (a wide leaf: each unit builds a dictionary and spells every new entry as text for numeric_in) costs the
-- scan about 0.75 microseconds for each entry, so a column whose values are nearly all distinct in a unit is slower on the scan than on the parallel
-- sequential scan, while a column of few values is not. The scan is priced for the entries each unit builds, which follows from the column's distinct
-- values and the rows of a unit, not for a share of the table's rows. Every statement is also run with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 2;
CREATE TABLE items (id int PRIMARY KEY, kind text NOT NULL, typed numeric(10, 2), many numeric, few numeric);
SELECT setseed(0.42);
INSERT INTO items
SELECT g, 'cat_' || chr(97 + g % 4), round((random() * 500)::numeric, 2), round((random() * 500)::numeric, 2), round((random() * 12)::numeric, 0)
FROM generate_series(1, 100000) g;
CREATE INDEX items_col ON items USING sift_col (id, kind, typed, many, few);
VACUUM (FREEZE, ANALYZE) items;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN 'SiftAgg'; END IF;
	END LOOP;
	RETURN 'normal';
END $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- the column with a precision is a scaled integer; the one with few values has a small dictionary: the scan. The one with many values: the normal plan
SELECT c AS col, plan_of(format('SELECT kind, sum(%I) FROM items GROUP BY kind', c)) AS plan, differences(format('SELECT kind, sum(%I) FROM items GROUP BY kind', c)) AS differences
FROM (VALUES ('typed'), ('many'), ('few')) v(c) ORDER BY c;

DROP FUNCTION plan_of(text), differences(text);
DROP TABLE items;
DROP EXTENSION pg_sift;
