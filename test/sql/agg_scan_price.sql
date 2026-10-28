-- The scan is priced from what it takes to read a row (a few tenths of a nanosecond to test a value of a column, a few nanoseconds for a key of a free key object): a
-- range of one month of a date column answered by the scan beats the btree plan on the instructions and the milliseconds (6 against 13 million, 0.34 against
-- 0.5 ms at 500,000 rows; a sum beside it, 27 against 33 million and 1.6 against 5 ms), and so do two or four keys of one object, which cost the normal plan
-- 15 times as long. The rows of a window of a twentieth of the table come from the index through a bitmap, not from the sequential scan.
-- The tables are 100,000 rows priced as 10 and 30 times as many (sift.plan_scale).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, d date NOT NULL, k int NOT NULL, amount numeric(10,2) NOT NULL, metrics jsonb);
SELECT setseed(0.42);
INSERT INTO t SELECT g, date '2022-01-01' + (random() * 1094)::int, g % 1000, round((random() * 1400)::numeric, 2),
       jsonb_build_object('m1', 1 + (random() * 9)::int, 'm2', 1 + (random() * 9)::int, 'm3', 1 + (random() * 9)::int, 'm4', 1 + (random() * 9)::int,
                          'm5', 1 + (random() * 9)::int, 'm6', 1 + (random() * 9)::int, 'm7', 1 + (random() * 9)::int, 'm8', 1 + (random() * 9)::int,
                          'm9', 1 + (random() * 9)::int, 'm10', 1 + (random() * 9)::int, 'm11', 1 + (random() * 9)::int, 'm12', 1 + (random() * 9)::int)
FROM generate_series(1, 100000) g;
CREATE INDEX t_d ON t (d);
CREATE INDEX t_col ON t USING sift_col (id, d, k, amount, metrics) WITH (paths = 'auto: metrics.* int4');
VACUUM (FREEZE, ANALYZE) t;

CREATE FUNCTION scan_used(q text, scale float8) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.plan_scale', scale::text, true);
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SELECT scan_used('SELECT count(*) FROM t WHERE d >= date ''2023-06-01'' AND d < date ''2023-07-01''', 10);
SELECT scan_used('SELECT count(*), sum(amount) FROM t WHERE d >= date ''2023-06-01'' AND d < date ''2023-07-01''', 10);
SELECT scan_used('SELECT k, avg((metrics ->> ''m1'')::int), avg((metrics ->> ''m2'')::int) FROM t GROUP BY k', 10);
SELECT scan_used('SELECT k, avg((metrics ->> ''m1'')::int), avg((metrics ->> ''m2'')::int), avg((metrics ->> ''m3'')::int), avg((metrics ->> ''m4'')::int) FROM t GROUP BY k', 10);

-- the rows of a window of a twentieth of a table of 3,000,000 rows (here 100,000 priced as 30 times as many) come from the index through a bitmap, not from the
-- sequential scan (24 ms against 63 ms measured)
CREATE TABLE w (id int PRIMARY KEY, d date NOT NULL, k int NOT NULL, amount float8 NOT NULL);
INSERT INTO w SELECT g, date '2022-01-01' + (random() * 1094)::int, g % 1000, random() * 1400 FROM generate_series(1, 100000) g;
CREATE INDEX w_col ON w USING sift_col (id, d, k, amount);
VACUUM (FREEZE, ANALYZE) w;
SET enable_indexonlyscan = off;
SET enable_indexscan = off;
SET sift.plan_scale = 30;
EXPLAIN (COSTS OFF) SELECT id, amount FROM w WHERE d >= date '2023-06-01' AND d < date '2023-08-01';

-- a semi join whose lookup table is read whole (6,000,000 rows here priced as 60 times 100,000) costs 29 operators a row, 86 ns (61 to 127 ns measured for 1,500,000 to
-- 6,000,000 rows): the orders of three months stay with the normal plan, which probes the index for them (2.6 ms against 772 ms)
CREATE TABLE o (o_orderkey int PRIMARY KEY, o_orderdate date NOT NULL, o_prio int NOT NULL);
CREATE TABLE l (l_orderkey int NOT NULL, l_commitdate date NOT NULL, l_receiptdate date NOT NULL);
INSERT INTO o SELECT g, date '1992-01-01' + (random() * 2400)::int, g % 5 FROM generate_series(1, 25000) g;
INSERT INTO l SELECT 1 + (random() * 24999)::int, d, d + (random() * 60 - 20)::int FROM (SELECT date '1992-01-01' + (random() * 2400)::int d FROM generate_series(1, 100000)) x;
CREATE INDEX l_k ON l (l_orderkey);
CREATE INDEX o_col ON o USING sift_col (o_orderkey, o_orderdate, o_prio);
CREATE INDEX l_col ON l USING sift_col (l_orderkey, l_commitdate, l_receiptdate);
VACUUM (FREEZE, ANALYZE) o, l;
SELECT scan_used('SELECT o_prio, count(*) FROM o WHERE o_orderdate >= date ''1993-07-01'' AND o_orderdate < date ''1993-10-01'' AND EXISTS (SELECT 1 FROM l WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate) GROUP BY 1', 60);
DROP FUNCTION scan_used(text, float8);
DROP TABLE t, w, o, l;
DROP EXTENSION pg_sift;
