-- A text key is read as dictionary codes, and the table of its ids is given every entry of the dictionary of each unit the scan reads, not only
-- the values of the rows that pass the filters (0.04 to 0.2 microseconds an entry as the table grows, and a walk of the table of ids each time it outgrows the memory of a
-- hash aggregate, work_mem times hash_mem_multiplier). A near-unique key under a selective filter costs the scan its units' entries for the few groups it
-- returns, so the normal plan, which hashes only the rows that pass, is kept; a key of a few values has few entries and costs what an integer key does.
-- The statements run with enable_agg on and off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE items (id int PRIMARY KEY, tag text NOT NULL, cat text NOT NULL, part int NOT NULL, amount float8 NOT NULL);
SELECT setseed(0.42);
INSERT INTO items SELECT g, md5(g::text), 'c' || (g % 17), g % 100, random() * 100 FROM generate_series(1, 100000) g;
CREATE INDEX items_col ON items USING sift_col (id, tag, cat, part, amount);
VACUUM (FREEZE, ANALYZE) items;

CREATE FUNCTION scan_cost(q text) RETURNS numeric LANGUAGE sql AS $$ SELECT scan_cost::numeric FROM sift.explain_why(q) $$;
CREATE FUNCTION scan_used(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT bool_or(used) FROM sift.explain_why(q) $$;
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

-- the entries of the near-unique key cost the scan well over the integer key of the same groups; the 17 values of the other cost it the same
SELECT key,
       scan_cost(format('SELECT %s, count(*) AS n, max(amount) AS s FROM items WHERE part = 7 GROUP BY %s', g, g)) /
       scan_cost('SELECT id, count(*) AS n, max(amount) AS s FROM items WHERE part = 7 GROUP BY id') > 5 AS entries_priced,
       differences(format('SELECT %s, count(*) AS n, max(amount) AS s FROM items WHERE part = 7 GROUP BY %s', g, g)) AS differences
FROM (VALUES ('near-unique text key', 'tag'), ('17 text values', 'cat')) v(key, g) ORDER BY key;

-- the near-unique key keeps the normal plan, the 17 values take the scan
SELECT key, scan_used(format('SELECT %s, count(*) AS n, max(amount) AS s FROM items WHERE part = 7 GROUP BY %s', g, g)) AS scan
FROM (VALUES ('near-unique text key', 'tag'), ('17 text values', 'cat')) v(key, g) ORDER BY key;

DROP FUNCTION scan_cost(text), scan_used(text), differences(text);
DROP TABLE items;
DROP EXTENSION pg_sift;
