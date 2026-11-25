-- A scope summary read while the table moves under it: the settled change of the summary (dead rows and new rows by member, kept for the backend), the newest rows of a key from its ring and the
-- delta, the scan of a scoped aggregate kept to the granules of its key, and the VACUUM that leaves no row alive that the heap lost (a TID the heap reuses names a new row, never an old dead one).
-- Every statement runs over the facts with the index and over a heap twin; each state must give the same rows, key by key.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE ov_pairs (member int NOT NULL, grp int NOT NULL, PRIMARY KEY (member, grp)) WITH (autovacuum_enabled = false);
CREATE TABLE ov_facts (id bigint PRIMARY KEY, member int NOT NULL, at timestamp NOT NULL, kind text NOT NULL, amount numeric(12,2) NOT NULL, qty int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ov_pairs SELECT m, 1 + m % 20 FROM generate_series(1, 400) m;
INSERT INTO ov_facts SELECT g, 1 + (g * 7919) % 400, TIMESTAMP '2025-01-01' + g * interval '30 seconds', 'k' || (g % 8), (g % 1000) / 10.0, g % 16 FROM generate_series(1, 40000) g;
CREATE TABLE ov_heap AS SELECT * FROM ov_facts;
ALTER TABLE ov_heap ADD PRIMARY KEY (id);
VACUUM (ANALYZE) ov_facts;
ANALYZE ov_pairs;
CREATE INDEX ov_ix ON ov_facts USING sift_col (id, member, at, kind, amount, qty) WITH (scope = 'grp: member -> ov_pairs(member, grp)');
VACUUM (ANALYZE) ov_facts;

CREATE FUNCTION ov_diff(tag text) RETURNS TABLE (state text, counts int, newest int, by_kind int, in_list int) LANGUAGE plpgsql AS $$
DECLARE
	k int;
	c int := 0; n int := 0; b int := 0; l int := 0;
	a text; h text;
BEGIN
	FOR k IN 1..20 LOOP
		SELECT count(*)::text || '/' || coalesce(sum(f.amount), 0)::text || '/' || coalesce(max(f.at)::text, '') INTO a FROM ov_facts f JOIN ov_pairs p ON p.member = f.member WHERE p.grp = k;
		SELECT count(*)::text || '/' || coalesce(sum(f.amount), 0)::text || '/' || coalesce(max(f.at)::text, '') INTO h FROM ov_heap f JOIN ov_pairs p ON p.member = f.member WHERE p.grp = k;
		c := c + (a IS DISTINCT FROM h)::int;
		SELECT string_agg(id::text, ',' ORDER BY at DESC, id DESC) INTO a FROM (SELECT f.id, f.at FROM ov_facts f JOIN ov_pairs p ON p.member = f.member WHERE p.grp = k ORDER BY f.at DESC, f.id DESC LIMIT 20) s;
		SELECT string_agg(id::text, ',' ORDER BY at DESC, id DESC) INTO h FROM (SELECT f.id, f.at FROM ov_heap f JOIN ov_pairs p ON p.member = f.member WHERE p.grp = k ORDER BY f.at DESC, f.id DESC LIMIT 20) s;
		n := n + (a IS DISTINCT FROM h)::int;
		SELECT string_agg(kind || ':' || cnt || ':' || amt, ',' ORDER BY kind) INTO a FROM (SELECT f.kind, count(*) cnt, sum(f.amount) amt FROM ov_facts f JOIN ov_pairs p ON p.member = f.member WHERE p.grp = k GROUP BY f.kind) s;
		SELECT string_agg(kind || ':' || cnt || ':' || amt, ',' ORDER BY kind) INTO h FROM (SELECT f.kind, count(*) cnt, sum(f.amount) amt FROM ov_heap f JOIN ov_pairs p ON p.member = f.member WHERE p.grp = k GROUP BY f.kind) s;
		b := b + (a IS DISTINCT FROM h)::int;
		SELECT count(*)::text || '/' || coalesce(sum(f.qty), 0)::text INTO a FROM ov_facts f WHERE f.member IN (SELECT member FROM ov_pairs WHERE grp IN (k, 1 + k % 20, 1 + (k + 6) % 20));
		SELECT count(*)::text || '/' || coalesce(sum(f.qty), 0)::text INTO h FROM ov_heap f WHERE f.member IN (SELECT member FROM ov_pairs WHERE grp IN (k, 1 + k % 20, 1 + (k + 6) % 20));
		l := l + (a IS DISTINCT FROM h)::int;
	END LOOP;
	RETURN QUERY SELECT tag, c, n, b, l;
END $$;

SELECT * FROM ov_diff('clean');
-- the pairs change: the members of two keys move, some leave every key
DELETE FROM ov_pairs WHERE member % 50 = 11;
UPDATE ov_pairs SET grp = 1 + (member * 7 + 5) % 20 WHERE member % 50 = 3;
SELECT * FROM ov_diff('pairs edited');
-- rows change: delete, update, insert, in the summary's parts and in the slab
DELETE FROM ov_facts WHERE id % 20 = 3;
DELETE FROM ov_heap WHERE id % 20 = 3;
UPDATE ov_facts SET amount = amount + 1, kind = 'k' || ((hashint8(id) >> 3) & 7) WHERE id % 20 = 7;
UPDATE ov_heap SET amount = amount + 1, kind = 'k' || ((hashint8(id) >> 3) & 7) WHERE id % 20 = 7;
INSERT INTO ov_facts SELECT g, 1 + (g * 7919) % 400, TIMESTAMP '2025-01-01' + g * interval '30 seconds', 'k' || (g % 8), (g % 1000) / 10.0, g % 16 FROM generate_series(40001, 42000) g;
INSERT INTO ov_heap SELECT g, 1 + (g * 7919) % 400, TIMESTAMP '2025-01-01' + g * interval '30 seconds', 'k' || (g % 8), (g % 1000) / 10.0, g % 16 FROM generate_series(40001, 42000) g;
SELECT * FROM ov_diff('rows changed');
SELECT * FROM ov_diff('rows changed, read again');
VACUUM ov_facts;
VACUUM ov_heap;
SELECT * FROM ov_diff('vacuumed');
-- the newest rows of every key are replaced and VACUUM runs: no old version stays a row of the summary
UPDATE ov_facts SET amount = amount + 1 WHERE id > 38000;
UPDATE ov_heap SET amount = amount + 1 WHERE id > 38000;
VACUUM ov_facts;
VACUUM ov_heap;
SELECT * FROM ov_diff('newest replaced, vacuumed');
-- the heap reuses the TIDs that VACUUM freed: delete rows that took them, update others, vacuum again
INSERT INTO ov_facts SELECT g, 1 + (g * 7919) % 400, TIMESTAMP '2025-01-01' + g * interval '30 seconds', 'k' || (g % 8), (g % 1000) / 10.0, g % 16 FROM generate_series(42001, 44000) g;
INSERT INTO ov_heap SELECT g, 1 + (g * 7919) % 400, TIMESTAMP '2025-01-01' + g * interval '30 seconds', 'k' || (g % 8), (g % 1000) / 10.0, g % 16 FROM generate_series(42001, 44000) g;
DELETE FROM ov_facts WHERE id % 97 < 3;
DELETE FROM ov_heap WHERE id % 97 < 3;
UPDATE ov_facts SET amount = amount + 1 WHERE id % 100 < 40;
UPDATE ov_heap SET amount = amount + 1 WHERE id % 100 < 40;
VACUUM ov_facts;
VACUUM ov_heap;
SELECT * FROM ov_diff('tids reused, vacuumed');
SELECT (SELECT count(*) FROM ov_facts) = (SELECT count(*) FROM ov_heap) AS same_rows;
DROP TABLE ov_facts, ov_heap, ov_pairs;
DROP FUNCTION ov_diff(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
