-- SiftAggV6 over the columns of every PostgreSQL type, from a sift_col index and from a sift_table of format major 2: money, time, timetz, enum, bytea, uuid, inet, cidr, macaddr, bit, arrays, ranges,
-- char(n), name, interval, and a numeric that holds a NaN, an infinity or fifty digits, as keys, as arguments of PostgreSQL's own aggregates and in quals and FILTER clauses; the text and the many
-- columns of a sift_table. Every statement runs over the heap twin (the truth), the index and the table; each prints whether the three answer alike and whether the index and the table ran as SiftAggV6.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;

CREATE TYPE dd_color AS ENUM ('red', 'green', 'blue');
CREATE TABLE dd (id int, g int, m money, tm time, tz timetz, iv interval, u uuid, ba bytea, e dd_color, ia int[], rg int4range, ip inet, cd cidr, mac macaddr, bt bit(8), vb varbit, ch char(5), nm name,
				 n38 numeric(38, 6), n18 numeric(12, 2), nf numeric, t text, i4 int, i8 int8, f8 float8, d date, ts timestamp) WITH (autovacuum_enabled = false);
INSERT INTO dd
SELECT i, i % 5, ((i % 37) / 4.0)::numeric::money, time '00:00' + (i % 24) * interval '1 hour', (ARRAY['12:00+01', '11:00+00', '04:05:06.5-03'])[1 + i % 3]::timetz,
       (i % 9) * interval '1 day' + (i % 4) * interval '1 hour', md5((i % 17)::text)::uuid, decode(md5((i % 13)::text), 'hex'), (ARRAY['red', 'green', 'blue'])[1 + i % 3]::dd_color,
       ARRAY[i % 4, i % 3], int4range(i % 5, i % 5 + 3), (ARRAY['10.0.0.1', '10.0.0.1/24', '192.168.1.5', '::1'])[1 + i % 4]::inet, (ARRAY['10.0.0.0/24', '10.0.0.0/8'])[1 + i % 2]::cidr,
       (ARRAY['08:00:2b:01:02:03', 'aa:bb:cc:dd:ee:ff'])[1 + i % 2]::macaddr, (ARRAY[B'00000000', B'00000001', B'11111111'])[1 + i % 3], (ARRAY['', '0', '101'])[1 + i % 3]::varbit,
       'c' || (i % 7), 'n' || (i % 6), (i % 41) / 7.0, (i % 53) / 3.0,
       CASE WHEN i % 31 = 0 THEN 'NaN' WHEN i % 29 = 0 THEN 'Infinity' WHEN i % 23 = 0 THEN '-Infinity' WHEN i % 19 = 0 THEN 12345678901234567890123456789012345678901234567890 + i
            WHEN i % 17 = 0 THEN NULL ELSE (i % 71) / 9.0 END,
       'tx' || (i % 11) || repeat('z', i % 14), i % 100, i * 1000000000::bigint, (i % 50) / 4.0, DATE '2024-01-01' + i % 60, TIMESTAMP '2024-01-01' + i * interval '1 hour'
FROM generate_series(1, 1200) i;
UPDATE dd SET n38 = 'NaN' WHERE id % 97 = 0;
UPDATE dd SET ts = CASE WHEN id % 211 = 0 THEN 'infinity'::timestamp ELSE '-infinity'::timestamp END WHERE id % 211 = 0 OR id % 223 = 0;
UPDATE dd SET m = NULL, tm = NULL, iv = NULL, u = NULL, ba = NULL, e = NULL, ia = NULL, ip = NULL, ch = NULL, nm = NULL, t = NULL WHERE id % 53 = 0;
CREATE TABLE di (LIKE dd) WITH (autovacuum_enabled = false);
INSERT INTO di SELECT * FROM dd;
CREATE INDEX di_col ON di USING sift_col (id, g, m, tm, tz, iv, u, ba, e, ia, rg, ip, cd, mac, bt, vb, ch, nm, n38, n18, nf, t, i4, i8, f8, d, ts);
CREATE TABLE dt USING sift_table AS SELECT * FROM dd;
INSERT INTO dt SELECT id + 5000, g, m, tm, tz, iv, u, ba, e, ia, rg, ip, cd, mac, bt, vb, ch, nm, n38, n18, nf, t, i4, i8, f8, d, ts FROM dd WHERE id % 10 = 0;
INSERT INTO di SELECT id + 5000, g, m, tm, tz, iv, u, ba, e, ia, rg, ip, cd, mac, bt, vb, ch, nm, n38, n18, nf, t, i4, i8, f8, d, ts FROM dd WHERE id % 10 = 0;
INSERT INTO dd SELECT id + 5000, g, m, tm, tz, iv, u, ba, e, ia, rg, ip, cd, mac, bt, vb, ch, nm, n38, n18, nf, t, i4, i8, f8, d, ts FROM dd WHERE id % 10 = 0;
DELETE FROM dt WHERE id % 50 = 0;
DELETE FROM di WHERE id % 50 = 0;
DELETE FROM dd WHERE id % 50 = 0;
VACUUM dd, di, dt;

-- q has a %s where the relation goes
CREATE FUNCTION answer(rel text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(coalesce(string_agg(r, ''|'' ORDER BY r), '''')) FROM (SELECT row_to_json(x)::text r FROM (%s) x) y', format(q, rel)) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION ran(rel text, q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE format('EXPLAIN (COSTS OFF) %s', format(q, rel)) LOOP
		IF line LIKE '%Custom Scan (SiftAggV6)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE TABLE cases (n serial, q text);
INSERT INTO cases (q) VALUES
 ('SELECT m, count(*), sum(m), max(m), min(m) FROM %s GROUP BY m'),
 ('SELECT tm, count(*), max(tm), min(tm) FROM %s GROUP BY tm'),
 ('SELECT tz, count(*), max(tz) FROM %s GROUP BY tz'),
 ('SELECT iv, count(*), max(iv), min(iv), sum(iv), avg(iv) FROM %s GROUP BY iv'),
 ('SELECT u, count(*) FROM %s GROUP BY u'),
 ('SELECT g, count(DISTINCT u), count(DISTINCT ba), count(DISTINCT iv), count(DISTINCT m) FROM %s GROUP BY g'),
 ('SELECT ba, count(*) FROM %s GROUP BY ba'),
 ('SELECT e, count(*), max(e), min(e) FROM %s GROUP BY e'),
 ('SELECT ia, count(*) FROM %s GROUP BY ia'),
 ('SELECT rg, count(*) FROM %s GROUP BY rg'),
 ('SELECT ip, count(*), max(ip), min(ip) FROM %s GROUP BY ip'),
 ('SELECT cd, count(*), max(cd) FROM %s GROUP BY cd'),
 ('SELECT mac, count(*) FROM %s GROUP BY mac'),
 ('SELECT bt, vb, count(*) FROM %s GROUP BY bt, vb'),
 ('SELECT ch, nm, count(*), max(ch), max(nm) FROM %s GROUP BY ch, nm'),
 ('SELECT nf, count(*) FROM %s GROUP BY nf'),
 ('SELECT n38, count(*) FROM %s GROUP BY n38'),
 ('SELECT g, count(*), sum(nf), max(nf), min(nf), avg(n38), sum(n38), max(n38), count(DISTINCT nf) FROM %s GROUP BY g'),
 ('SELECT g, count(*) FROM %s WHERE nf > 5 GROUP BY g'),
 ('SELECT g, count(*) FROM %s WHERE n38 > 3 AND nf <> 7 GROUP BY g'),
 ('SELECT g, count(*) FROM %s WHERE nf IS NULL GROUP BY g'),
 ('SELECT g, count(*) FROM %s WHERE m > ''2.00'' AND iv >= interval ''3 days'' GROUP BY g'),
 ('SELECT g, count(*) FILTER (WHERE n38 > 5 OR iv > interval ''1 day''), sum(n38) FROM %s GROUP BY g'),
 ('SELECT g, count(*) FILTER (WHERE t LIKE ''tx1%%''), max(i4) FILTER (WHERE e = ''red'') FROM %s GROUP BY g'),
 ('SELECT count(*) FROM %s WHERE ia IS NOT NULL AND cardinality(ia) > 1'),
 ('SELECT g, string_agg(DISTINCT nm::text, '','' ORDER BY nm::text), array_agg(DISTINCT e ORDER BY e) FROM %s GROUP BY g'),
 ('SELECT t, count(*), max(t), min(t) FROM %s GROUP BY t'),
 ('SELECT g, d, count(*), max(t), count(DISTINCT t), sum(i4), sum(i8), max(f8), min(ts) FROM %s GROUP BY g, d'),
 ('SELECT g, count(*), sum(i4), sum(i8), sum(f8), min(d), max(ts), max(t), min(n18), sum(n18) FROM %s GROUP BY g'),
 ('SELECT g, count(*) FILTER (WHERE (t > ''tx5'') IS NOT TRUE), sum(i4) FILTER (WHERE t > ''tx5''), sum(i4) FILTER (WHERE (n38 > 2) IS NOT TRUE) FROM %s GROUP BY g'),
 ('SELECT g, sum(n38 * 2), max(n38 * 2), count(*) FROM %s GROUP BY g'),
 ('SELECT n38 * 2, count(*) FROM %s GROUP BY 1'),
 ('SELECT g, count(*), max(ts), min(ts) FROM %s WHERE ts < ''infinity'' GROUP BY g'),
 ('SELECT ts, count(*) FROM %s WHERE ts <> ''-infinity'' GROUP BY ts');
SELECT n, answer('dd', q) = answer('di', q) AS index_same, answer('dd', q) = answer('dt', q) AS table_same, ran('di', q) AS index_v6, ran('dt', q) AS table_v6 FROM cases ORDER BY n;

DROP TABLE dd, di, dt, cases CASCADE;
DROP FUNCTION answer(text, text), ran(text, text);
DROP TYPE dd_color;
DROP EXTENSION pg_sift;
