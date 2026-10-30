-- A sum over a numeric column without a precision (a wide leaf: each unit builds a dictionary of its values) is added from the dictionary's entries, a
-- numeric decoded once to an integer and a scale (0.1 to 0.4 microseconds an entry): the scan is priced for the entries each unit builds,
-- which follows from the column's distinct values and the rows of a unit, and against the parallel plan of PostgreSQL, which the leader-only sum does not
-- match on a large table. A column of precision 30 is a wide leaf too (its entries are built from a 128-bit lane): the same rule. Every statement is also
-- run with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 2;
CREATE TABLE items (id int PRIMARY KEY, kind text NOT NULL, typed numeric(10, 2), many numeric, few numeric, wmany numeric(30, 16), wfew numeric(30, 16));
SELECT setseed(0.42);
INSERT INTO items
SELECT g, 'cat_' || chr(97 + g % 4), round((random() * 500)::numeric, 2), round((random() * 500)::numeric, 2), round((random() * 12)::numeric, 0), round((random() * 500)::numeric, 2) / 7, (g % 40) / 7.0
FROM generate_series(1, 100000) g;
CREATE INDEX items_col ON items USING sift_col (id, kind, typed, many, few, wmany, wfew);
VACUUM (FREEZE, ANALYZE) items;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN 'SiftAgg'; END IF;
	END LOOP;
	RETURN 'normal';
END $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg(q) $$;

-- the column with a precision is a scaled integer; the one with few values has a small dictionary: the scan. The ones with many values: the scan too where it costs less than the parallel plan (the 100,000 rows of this table: 8 ms against 16), else the normal plan
SELECT c AS col, plan_of(format('SELECT kind, sum(%I) FROM items GROUP BY kind', c)) AS plan, differences(format('SELECT kind, sum(%I) FROM items GROUP BY kind', c)) AS differences
FROM (VALUES ('typed'), ('many'), ('few'), ('wmany'), ('wfew')) v(c) ORDER BY c;

DROP FUNCTION plan_of(text), differences(text);
DROP TABLE items;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
