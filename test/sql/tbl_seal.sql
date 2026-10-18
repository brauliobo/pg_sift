-- Sealing: the rows of a sift_table move from the row format write buffer to column segments with the same TIDs. Everything that works
-- before must work after, against a heap twin: scans, indexes, updates and deletes of sealed rows, VACUUM, ANALYZE, rewrites, ALTER TABLE.
CREATE EXTENSION pg_sift;

CREATE TABLE s (id int PRIMARY KEY, i2 int2, i8 int8, n numeric(12,3), nu numeric, r4 float4, r8 float8, d date, ts timestamptz, b bool,
	t text, vc varchar(20), c char(5), y bytea, u uuid, j jsonb, a int[], iv interval, o "char", w text) USING sift_table WITH (vacuum_truncate = false);
CREATE TABLE h (LIKE s INCLUDING ALL) USING heap;
CREATE INDEX s_i2 ON s (i2, i8);
CREATE INDEX h_i2 ON h (i2, i8);

CREATE FUNCTION seal_fill(lo int, hi int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO s SELECT g, CASE WHEN g % 17 = 0 THEN NULL ELSE (g % 300)::int2 END, g * 1000003::int8 % 99991, (g % 977) / 8.0,
		CASE WHEN g % 31 = 0 THEN NULL WHEN g % 29 = 0 THEN 'NaN' WHEN g % 23 = 0 THEN 'Infinity' ELSE g::numeric / 7 * 1e10 END,
		CASE WHEN g % 19 = 0 THEN NULL ELSE (g % 100) / 4.0 END, CASE g % 41 WHEN 0 THEN '-0' WHEN 1 THEN 'NaN' WHEN 2 THEN '-Infinity' ELSE g / 3.0 END,
		date '2020-01-01' + g % 1000, timestamptz '2024-06-01 12:00:00+00' + g * interval '37 seconds', g % 3 = 0,
		CASE WHEN g % 11 = 0 THEN NULL WHEN g % 53 = 0 THEN repeat('long ', 100) ELSE 'text' || g % 40 END, 'v' || g % 7, 'c' || g % 5,
		decode(md5(g::text), 'hex'), md5(g::text)::uuid, jsonb_build_object('k', g, 'a', jsonb_build_array(g % 3, 'x')),
		ARRAY[g, g + 1], g * interval '1 minute', CASE WHEN g % 2 = 0 THEN 'a' ELSE 'b' END::"char", CASE WHEN g % 7 = 0 THEN repeat('w', 5000) END
	FROM generate_series(lo, hi) g;
	INSERT INTO h SELECT * FROM s WHERE id BETWEEN lo AND hi;
$$;

CREATE FUNCTION seal_compare(what text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint; b bigint; ia text; ib text; ba text; bb text;
BEGIN
	SELECT count(*) INTO a FROM (SELECT s::text FROM s EXCEPT ALL SELECT h::text FROM h) x;
	SELECT count(*) INTO b FROM (SELECT h::text FROM h EXCEPT ALL SELECT s::text FROM s) x;
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SELECT count(*) || ':' || coalesce(sum(i8), 0) INTO ia FROM s WHERE i2 BETWEEN 100 AND 140 AND i8 > 5000;
	SELECT count(*) || ':' || coalesce(sum(i8), 0) INTO ib FROM h WHERE i2 BETWEEN 100 AND 140 AND i8 > 5000;
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_bitmapscan = on;
	SELECT count(*) || ':' || coalesce(sum(i8), 0) INTO ba FROM s WHERE i2 BETWEEN 100 AND 140 AND i8 > 5000;
	SELECT count(*) || ':' || coalesce(sum(i8), 0) INTO bb FROM h WHERE i2 BETWEEN 100 AND 140 AND i8 > 5000;
	RETURN what || ': ' || CASE WHEN a = 0 AND b = 0 AND ia = ib AND ba = bb THEN 'same' ELSE format('DIFFERENT s-h=%s h-s=%s index %s vs %s bitmap %s vs %s', a, b, ia, ib, ba, bb) END;
END $$;

CREATE FUNCTION seal_layout() RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(kind || ' x' || n, ', ' ORDER BY kind) FROM (SELECT kind, count(*) n FROM sift.table_segments('s') GROUP BY kind) x
$$;

SELECT seal_fill(1, 3000);
SELECT seal_compare('bulk insert'), seal_layout();
VACUUM s;
SELECT seal_compare('sealed by VACUUM'), seal_layout();

-- point reads of sealed rows by every access path
SELECT s::text = h::text AS same FROM s JOIN h USING (id) WHERE id IN (1, 41, 42, 1500, 2999, 3000) ORDER BY id;
SELECT count(*) FROM s WHERE id BETWEEN 100 AND 2000 AND i2 IS NOT NULL;
SELECT ctid = (SELECT ctid FROM s s2 WHERE s2.id = 500) AS ctid_found FROM s WHERE id = 500;

-- changes of sealed rows: tombstones apply by TID
UPDATE s SET w = 'upd', i8 = i8 + 1 WHERE id % 7 = 0;
UPDATE h SET w = 'upd', i8 = i8 + 1 WHERE id % 7 = 0;
DELETE FROM s WHERE id % 5 = 0;
DELETE FROM h WHERE id % 5 = 0;
SELECT seal_compare('update and delete of sealed rows'), seal_layout();
-- an aborted change and a savepoint
BEGIN; DELETE FROM s WHERE id < 100; ROLLBACK;
BEGIN; SAVEPOINT a; UPDATE s SET i8 = 0 WHERE id BETWEEN 10 AND 20; ROLLBACK TO a; COMMIT;
SELECT seal_compare('aborted changes');

VACUUM s;
SELECT seal_compare('VACUUM clears the dead rows'), seal_layout();
SELECT sum(dead_rows) > 0 AS dead_rows_counted FROM sift.table_segments('s') WHERE kind = 'columns';

-- statistics
ANALYZE s;
SELECT reltuples::int = (SELECT count(*) FROM s) AS reltuples_exact FROM pg_class WHERE relname = 's';

-- more rows, more segments, merged
SELECT seal_fill(3001, 4000);
UPDATE s SET vc = 'again' WHERE id > 3500;
UPDATE h SET vc = 'again' WHERE id > 3500;
VACUUM s;
SELECT seal_fill(4001, 4500);
VACUUM s;
SELECT seal_fill(4501, 5000);
VACUUM s;
SELECT seal_compare('segments added'), seal_layout();

-- segments that are mostly dead are rewritten without their dead rows
DELETE FROM s WHERE id % 10 <> 0 AND id > 1000;
DELETE FROM h WHERE id % 10 <> 0 AND id > 1000;
VACUUM s;
SELECT seal_compare('mostly dead segments merged'), seal_layout();
SELECT sum(dead_rows) AS dead_rows, sum(rows) = (SELECT count(*) FROM s) AS rows_live FROM sift.table_segments('s') WHERE kind = 'columns';

-- special values survive exactly
SELECT id, r8::text, r4::text, nu::text, length(w), t IS NULL AS tnull FROM s WHERE id IN (41, 82, 42, 58, 87, 253) ORDER BY id;

-- rewrites
VACUUM FULL s;
VACUUM FULL h;
SELECT seal_compare('VACUUM FULL'), seal_layout();
CLUSTER s USING s_i2;
CLUSTER h USING h_i2;
SELECT seal_compare('CLUSTER'), seal_layout();

-- ALTER TABLE after sealing: old segments keep their layout
VACUUM s;
ALTER TABLE s ADD COLUMN extra int DEFAULT 7;
ALTER TABLE h ADD COLUMN extra int DEFAULT 7;
SELECT seal_compare('add column'), (SELECT count(*) FROM s WHERE extra = 7) AS defaulted;
ALTER TABLE s DROP COLUMN w;
ALTER TABLE h DROP COLUMN w;
SELECT seal_compare('drop column');
INSERT INTO s SELECT id + 100000, i2, i8, n, nu, r4, r8, d, ts, b, t, vc, c, y, u, j, a, iv, o, 9 FROM s WHERE id < 50;
INSERT INTO h SELECT id + 100000, i2, i8, n, nu, r4, r8, d, ts, b, t, vc, c, y, u, j, a, iv, o, 9 FROM h WHERE id < 50;
VACUUM s;
SELECT seal_compare('new layout beside the old one'), seal_layout();
ALTER TABLE s ALTER COLUMN i8 TYPE numeric;
ALTER TABLE h ALTER COLUMN i8 TYPE numeric;
SELECT seal_compare('type change rewrites');

TRUNCATE s;
SELECT count(*), seal_layout() FROM s;
DROP TABLE s, h;
DROP FUNCTION seal_fill(int, int), seal_compare(text), seal_layout();
DROP EXTENSION pg_sift;
