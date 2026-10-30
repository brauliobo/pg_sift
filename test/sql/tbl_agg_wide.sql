-- SiftAgg over a sift_table: a WHERE conjunct on numeric(p > 18), unconstrained numeric, money and interval is tested in the source on the
-- exact values. Every query runs with and without the pushdown; the results must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE wd (id int, g int, n38 numeric(38, 6), nf numeric, m money, iv interval, i4 int) USING sift_table;

CREATE FUNCTION wd_rows(q text, push bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg(q, push) $$;

CREATE FUNCTION wd_pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_mentions_siftagg(q) $$;

CREATE TABLE wd_q (q text);
INSERT INTO wd_q VALUES
	('SELECT g, count(*), sum(i4) FROM wd WHERE n38 > 100.5 GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE n38 = 70000000000000000000.000007 GROUP BY g'),
	('SELECT count(*) FROM wd WHERE nf <> 3 AND nf <= 40'),
	('SELECT count(*) FROM wd WHERE nf = ''NaN'' OR n38 IS NULL'),
	('SELECT count(*) FROM wd WHERE nf IN (1, 2.50, 3) OR i4 = 1'),
	('SELECT g, count(*) FROM wd WHERE nf IN (1, 2.50, 3, 7.0) GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE nf NOT IN (1, 2.50, 3) GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE nf NOT IN (1, NULL) GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE nf NOT IN (SELECT 1 WHERE false) GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE n38 IS NULL GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE nf IS NOT NULL AND n38 IS NOT NULL GROUP BY g'),
	('SELECT g, count(*), sum(i4) FROM wd WHERE m > ''10.00'' GROUP BY g'),
	('SELECT count(*) FROM wd WHERE m BETWEEN ''5.00'' AND ''20.00'' AND m <> ''7.00'''),
	('SELECT g, count(*) FROM wd WHERE m IN (''1.00'', ''2.00'', ''30.00'') GROUP BY g'),
	('SELECT g, count(*) FROM wd WHERE iv >= interval ''10 days'' GROUP BY g'),
	('SELECT count(*) FROM wd WHERE iv = interval ''1 month'''),
	('SELECT count(*) FROM wd WHERE iv < interval ''1 month'' AND m > ''0'''),
	('SELECT count(*) FROM wd WHERE iv NOT IN (interval ''1 month'', interval ''2 days'')');

CREATE FUNCTION wd_compare(OUT query text, OUT pushed bool, OUT same bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
BEGIN
	FOR query IN SELECT q FROM wd_q ORDER BY q COLLATE "C" LOOP
		pushed := wd_pushed(query);
		same := wd_rows(query, true) IS NOT DISTINCT FROM wd_rows(query, false);
		RETURN NEXT;
	END LOOP;
END $$;

INSERT INTO wd SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE i * 1000000000000000.000001 / 10 END,
	CASE WHEN i % 13 = 0 THEN 'NaN' WHEN i % 17 = 0 THEN NULL ELSE (i % 50) / 2.0 END,
	CASE WHEN i % 19 = 0 THEN NULL ELSE ((i % 40) * 1.25)::numeric::money END,
	CASE WHEN i % 23 = 0 THEN NULL ELSE ((i % 5) || ' days')::interval + ((i % 3) || ' months')::interval END, i
	FROM generate_series(1, 3000) i;
INSERT INTO wd VALUES (5000, 1, 70000000000000000000.000007, 3.00, '7.00', '30 days', 1), (5001, 2, 70000000000000000000.000007, 1, '7.00', '1 month', 2);
SELECT * FROM wd_compare();

-- write buffer rows, tombstones and a segment that predates the columns
VACUUM wd;
SELECT * FROM wd_compare();
UPDATE wd SET n38 = n38 + 1, m = m + '1.00'::money, iv = iv + interval '1 day' WHERE id % 5 = 0;
DELETE FROM wd WHERE id % 9 = 0;
INSERT INTO wd SELECT 10000 + i, i % 7, i, i % 40, (i % 30)::numeric::money, (i % 4 || ' days')::interval, i FROM generate_series(1, 700) i;
SELECT * FROM wd_compare();
VACUUM wd;
SELECT * FROM wd_compare();

ALTER TABLE wd ADD COLUMN m2 money DEFAULT '12.00';
ALTER TABLE wd ADD COLUMN n2 numeric DEFAULT 4.5;
INSERT INTO wd (id, g, m2, n2) SELECT 20000 + i, i % 7, (i % 25)::numeric::money, i % 9 FROM generate_series(1, 500) i;
TRUNCATE wd_q;
INSERT INTO wd_q VALUES
	('SELECT g, count(*) FROM wd WHERE m2 > ''12.00'' GROUP BY g'),
	('SELECT count(*) FROM wd WHERE m2 = ''12.00'''),
	('SELECT count(*) FROM wd WHERE m2 IS NULL OR n2 IS NULL'),
	('SELECT g, count(*) FROM wd WHERE n2 >= 4.5 GROUP BY g'),
	('SELECT count(*) FROM wd WHERE n2 NOT IN (4.5, 4.50, 3)');
SELECT * FROM wd_compare();
VACUUM wd;
SELECT * FROM wd_compare();

DROP TABLE wd, wd_q;
DROP FUNCTION wd_rows, wd_pushed, wd_compare;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
