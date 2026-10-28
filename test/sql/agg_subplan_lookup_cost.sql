-- A lookup table that is a plan (a UNION ALL of two fact tables) is made of the rows of the plan, and the scan reads a dimension: each of its rows is joined with the rows of the plan
-- that carry its key, so the tuples that reach the groups are the rows PostgreSQL expects of the join (500,000 here, not the 2,000 rows of the dimension), and each row of
-- the plan costs the scan its tuple, a hash of the key and a copy of each numeric column (about 1,000 instructions a row with one numeric column, four times a count(*) over it).
-- Seven numeric sums of a CASE over the day of the dimension took 540 ms against the normal plan's 215 ms, priced a third of it. The statements run with enable_agg on and off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sp_dim (id int PRIMARY KEY, week int NOT NULL, day text NOT NULL);
INSERT INTO sp_dim SELECT g, g / 7, (ARRAY['Sun','Mon','Tue','Wed','Thu','Fri','Sat'])[1 + g % 7] FROM generate_series(1, 2000) g;
CREATE TABLE sp_a (dim_id int NOT NULL, price numeric(7,2) NOT NULL);
INSERT INTO sp_a SELECT 1 + (hashint4(g) & 2147483647) % 2000, ((hashint4(g + 1) & 2147483647) % 100000) / 100.0 FROM generate_series(1, 60000) g;
CREATE TABLE sp_b (dim_id int NOT NULL, price numeric(7,2) NOT NULL);
INSERT INTO sp_b SELECT 1 + (hashint4(g + 5) & 2147483647) % 2000, ((hashint4(g + 6) & 2147483647) % 100000) / 100.0 FROM generate_series(1, 90000) g;
CREATE INDEX sp_dim_col ON sp_dim USING sift_col (id, week, day);
CREATE INDEX sp_a_col ON sp_a USING sift_col (dim_id, price);
CREATE INDEX sp_b_col ON sp_b USING sift_col (dim_id, price);
VACUUM (FREEZE, ANALYZE) sp_dim, sp_a, sp_b;

CREATE FUNCTION why(q text) RETURNS TABLE (used boolean, slug text) LANGUAGE sql AS $$ SELECT used, slug FROM sift.explain_why(q) LIMIT 1 $$;
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

-- the dimension scanned, the plan its lookup: seven numeric sums and a count of the 150,000 tuples
SELECT * FROM why($q$
SELECT week, sum(CASE WHEN day = 'Sun' THEN price END) AS s0, sum(CASE WHEN day = 'Mon' THEN price END) AS s1, sum(CASE WHEN day = 'Tue' THEN price END) AS s2, sum(CASE WHEN day = 'Wed' THEN price END) AS s3,
       sum(CASE WHEN day = 'Thu' THEN price END) AS s4, sum(CASE WHEN day = 'Fri' THEN price END) AS s5, sum(CASE WHEN day = 'Sat' THEN price END) AS s6
FROM (SELECT dim_id, price FROM sp_a UNION ALL SELECT dim_id, price FROM sp_b) w, sp_dim WHERE id = dim_id GROUP BY week
$q$);
SELECT differences($q$
SELECT week, sum(CASE WHEN day = 'Sun' THEN price END) AS s0, sum(CASE WHEN day = 'Mon' THEN price END) AS s1, sum(CASE WHEN day = 'Tue' THEN price END) AS s2, sum(CASE WHEN day = 'Wed' THEN price END) AS s3,
       sum(CASE WHEN day = 'Thu' THEN price END) AS s4, sum(CASE WHEN day = 'Fri' THEN price END) AS s5, sum(CASE WHEN day = 'Sat' THEN price END) AS s6
FROM (SELECT dim_id, price FROM sp_a UNION ALL SELECT dim_id, price FROM sp_b) w, sp_dim WHERE id = dim_id GROUP BY week
$q$) AS differences;

-- the fact scanned, the dimension a plan of 2,000 rows with a unique key: one tuple a row, the scan stays
SELECT * FROM why($q$
SELECT week, sum(price) FROM sp_a, (SELECT id, week FROM sp_dim) d WHERE id = dim_id GROUP BY week
$q$);

DROP FUNCTION why(text), differences(text);
DROP TABLE sp_dim, sp_a, sp_b;
DROP EXTENSION pg_sift;
