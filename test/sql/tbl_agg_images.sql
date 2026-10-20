-- SiftAgg over a sift_table: keys, aggregates and conjuncts of the columns the table keeps as exact images (numeric(p > 18), interval, uuid, bytea,
-- inet, arrays) or by size (money, time, enum) are read through a dictionary of the datums, as the column index reads them. Every query runs with
-- and without the pushdown; the results must be the same, in column groups, in write buffers, after DML and in segments that lack a column.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TYPE im_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE im (id int, g int, n38 numeric(38, 6), nf numeric, m money, iv interval, u uuid, b bytea, ip inet, ia int[], t time, e im_mood, i4 int) USING sift_table;

CREATE FUNCTION im_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION im_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE im_q (q text);
INSERT INTO im_q VALUES
	('SELECT n38, count(*), sum(i4) FROM im GROUP BY n38'),
	('SELECT g, sum(n38), min(n38), max(nf::float8), count(DISTINCT nf) FROM im GROUP BY g'),
	('SELECT nf::float8, count(*) FROM im GROUP BY nf::float8'),
	('SELECT m, count(*), sum(i4) FROM im GROUP BY m'),
	('SELECT g, sum(m), min(m), max(m) FROM im GROUP BY g'),
	('SELECT iv, count(*) FROM im GROUP BY iv'),
	('SELECT g, min(iv), max(iv), sum(iv) FROM im GROUP BY g'),
	('SELECT u, count(*) FROM im GROUP BY u'),
	('SELECT g, count(DISTINCT u), count(DISTINCT b) FROM im GROUP BY g'),
	('SELECT b, count(*) FROM im GROUP BY b'),
	('SELECT ip, count(*), min(ip), max(ip) FROM im GROUP BY ip'),
	('SELECT ia, count(*) FROM im GROUP BY ia'),
	('SELECT t, count(*) FROM im GROUP BY t'),
	('SELECT g, min(t), max(t) FROM im GROUP BY g'),
	('SELECT e, count(*), sum(i4) FROM im GROUP BY e'),
	('SELECT g, count(*) FROM im WHERE e >= ''ok'' AND t > ''12:00'' GROUP BY g'),
	('SELECT g, count(*) FROM im WHERE ip << inet ''10.0.0.0/9'' OR u IS NULL GROUP BY g'),
	('SELECT g, count(*) FROM im WHERE ia @> ARRAY[3] GROUP BY g'),
	('SELECT g, count(*) FILTER (WHERE m > ''10.00''), count(*) FILTER (WHERE n38 > 100) FROM im GROUP BY g'),
	('SELECT length(b) AS len, host(ip) AS h, count(*) FROM im GROUP BY 1, 2');

CREATE FUNCTION im_compare(OUT query text, OUT pushed bool, OUT same bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
BEGIN
	FOR query IN SELECT q FROM im_q ORDER BY q COLLATE "C" LOOP
		pushed := im_pushed(query);
		same := im_rows(query, true) IS NOT DISTINCT FROM im_rows(query, false);
		RETURN NEXT;
	END LOOP;
END $$;

INSERT INTO im SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE i * 1000000000000000.000001 / 10 END,
	CASE WHEN i % 13 = 0 THEN 'NaN' WHEN i % 17 = 0 THEN NULL ELSE (i % 50) / 2.0 END,
	CASE WHEN i % 19 = 0 THEN NULL ELSE ((i % 40) * 1.25)::numeric::money END,
	CASE WHEN i % 23 = 0 THEN NULL ELSE ((i % 5) || ' days')::interval + ((i % 3) || ' months')::interval END,
	CASE WHEN i % 29 = 0 THEN NULL ELSE md5((i % 40)::text)::uuid END,
	CASE WHEN i % 31 = 0 THEN NULL ELSE decode(md5((i % 25)::text), 'hex') END,
	CASE WHEN i % 37 = 0 THEN NULL ELSE ('10.' || i % 5 || '.0.' || i % 60 || '/24')::inet END,
	CASE WHEN i % 41 = 0 THEN NULL ELSE ARRAY[i % 4, i % 6] END,
	CASE WHEN i % 43 = 0 THEN NULL ELSE time '00:00' + (i % 90) * interval '17 minutes 5 seconds' END,
	CASE WHEN i % 47 = 0 THEN NULL ELSE (ARRAY['sad', 'ok', 'happy'])[1 + i % 3]::im_mood END, i
	FROM generate_series(1, 3000) i;
SELECT * FROM im_compare();

-- write buffer rows, tombstones and a segment that predates the columns
VACUUM im;
SELECT * FROM im_compare();
UPDATE im SET n38 = n38 + 1, m = m + '1.00'::money, iv = iv + interval '1 day', u = NULL, t = t + interval '1 hour' WHERE id % 5 = 0;
DELETE FROM im WHERE id % 9 = 0;
INSERT INTO im SELECT 10000 + i, i % 7, i, i % 40, (i % 30)::numeric::money, (i % 4 || ' days')::interval, md5(i::text)::uuid, '\x00ff'::bytea,
	'192.168.1.1', ARRAY[i], time '23:59', 'happy', i FROM generate_series(1, 700) i;
SELECT * FROM im_compare();
VACUUM im;
SELECT * FROM im_compare();

ALTER TABLE im ADD COLUMN m2 money DEFAULT '12.00';
ALTER TABLE im ADD COLUMN u2 uuid DEFAULT '00000000-0000-0000-0000-00000000000a';
ALTER TABLE im ADD COLUMN t2 time DEFAULT '01:02:03';
ALTER TABLE im ADD COLUMN e2 im_mood DEFAULT 'ok';
ALTER TABLE im ADD COLUMN n2 numeric DEFAULT 4.5;
INSERT INTO im (id, g, m2, u2, t2, e2, n2) SELECT 20000 + i, i % 7, (i % 25)::numeric::money, md5(i::text)::uuid, time '10:00' + i * interval '1 minute',
	'sad', i % 9 FROM generate_series(1, 500) i;
TRUNCATE im_q;
INSERT INTO im_q VALUES
	('SELECT m2, count(*) FROM im GROUP BY m2'),
	('SELECT u2, count(*) FROM im GROUP BY u2'),
	('SELECT t2, count(*) FROM im GROUP BY t2'),
	('SELECT e2, count(*), sum(m2) FROM im GROUP BY e2'),
	('SELECT n2, count(*) FROM im GROUP BY n2'),
	('SELECT g, count(*) FROM im WHERE m2 > ''12.00'' AND e2 = ''ok'' GROUP BY g');
SELECT * FROM im_compare();
VACUUM im;
SELECT * FROM im_compare();

DROP TABLE im, im_q;
DROP TYPE im_mood;
DROP FUNCTION im_rows, im_pushed, im_compare;
DROP EXTENSION pg_sift;
