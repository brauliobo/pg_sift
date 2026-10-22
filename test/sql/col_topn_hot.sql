-- ORDER BY <column> LIMIT n over a sift_col index after a HOT update: an UPDATE that leaves every indexed value as it was (a NULL set to NULL) is
-- not a new index entry, so the entry names the root of the HOT chain, which is a redirect once the page is pruned (here by the next UPDATE).
-- The scan reads the row the entry names as an index scan does, through the chain; the rows and their order are those of the plain plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE hot (id int PRIMARY KEY, parent_id int, amount int) WITH (autovacuum_enabled = false, fillfactor = 75);
INSERT INTO hot SELECT i, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 50 END, i % 10 FROM generate_series(1, 6000) i;
CREATE INDEX hot_col ON hot USING sift_col (id, parent_id, amount);
VACUUM (FREEZE, ANALYZE) hot;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan, and the rows
CREATE FUNCTION hot_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text; res text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE hot_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE hot_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM hot_a EXCEPT ALL SELECT pos, r FROM hot_b) UNION ALL (SELECT pos, r FROM hot_b EXCEPT ALL SELECT pos, r FROM hot_a)) d;
	res := CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || ' ' || (SELECT count(*) FROM hot_b);
	DROP TABLE hot_a, hot_b;
	RETURN res;
END $$;

-- one update of rows whose key is NULL and stays NULL (HOT), and of rows whose key changes (a new entry)
UPDATE hot SET parent_id = NULL WHERE id % 10 = 0;
UPDATE hot SET amount = amount + 1 WHERE id % 10 = 5;
UPDATE hot SET parent_id = 7 WHERE id % 10 = 3;

SELECT hot_check($q$SELECT id, amount FROM hot ORDER BY parent_id DESC NULLS FIRST, id LIMIT 50$q$);
SELECT hot_check($q$SELECT id, parent_id FROM hot ORDER BY amount DESC, id LIMIT 30$q$);
SELECT hot_check($q$SELECT id, amount FROM hot ORDER BY parent_id NULLS LAST, amount, id LIMIT 40 OFFSET 3$q$);

DROP TABLE hot;
DROP FUNCTION hot_check(text);
DROP EXTENSION pg_sift;
