-- key_cut by default: CREATE INDEX and REINDEX of an index without the option sample the heap and cut the units by the first key-shaped column when
-- an equality on it reads at least 8 times the units its rows fill (and 64 units), no other leaf follows the order of the table, and the index is wide enough for the
-- addresses of the rows not to dominate it; key_cut = 'off' and an explicit list are never changed.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- 100 tenants in heap order but for 15% of the rows, which land anywhere; ten leaves that follow nothing; 70 superblocks (fillfactor 10 keeps the rows few)
CREATE TABLE ka (id int, tenant_id int, seq int, a1 int, a2 int, a3 int, a4 int, a5 int, a6 int, a7 int, a8 int, a9 int, a10 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ka SELECT i, CASE WHEN random() < 0.15 THEN (random() * 100)::int ELSE i / 1250 END, i,
	(i * 31) % 1000, (i * 37) % 1000, (i * 41) % 1000, (i * 43) % 1000, (i * 47) % 1000, (i * 53) % 1000, (i * 59) % 1000, (i * 61) % 1000, (i * 67) % 1000, (i * 71) % 1000, repeat('x', 100)
FROM generate_series(1, 125000) i;
VACUUM (FREEZE, ANALYZE) ka;

CREATE FUNCTION kopt(i regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = i AND o ~ '^key_cut'), 'none') $$;

-- chosen: the key is scattered over the units, nothing else follows the table
CREATE INDEX ka_auto ON ka USING sift_col (tenant_id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
SELECT kopt('ka_auto');
SELECT kind, format FROM sift.col_segments('ka_auto') WHERE kind = 'immutable';
SELECT sift.col_verify('ka_auto', true);

-- a leaf that follows the heap (seq) keeps its zones: not chosen
CREATE INDEX ka_seq ON ka USING sift_col (tenant_id, seq, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
SELECT kopt('ka_seq');

-- a narrow index is cut too: the key and its pre-aggregates take less in the units of the key than the addresses cost
CREATE INDEX ka_narrow ON ka USING sift_col (tenant_id, a1, a2);
SELECT kopt('ka_narrow');

-- the switch and an explicit list
CREATE INDEX ka_off ON ka USING sift_col (tenant_id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) WITH (key_cut = 'off');
SELECT kopt('ka_off');
CREATE INDEX ka_list ON ka USING sift_col (tenant_id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) WITH (key_cut = 'a1');
SELECT kopt('ka_list');

-- the answers are those of the table, with the units of the key and without
DROP INDEX ka_seq, ka_narrow, ka_list;
SET enable_indexscan = off; SET enable_bitmapscan = off;
CREATE FUNCTION kunits(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; u text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Index: %' OR l LIKE '%Units: %' THEN u := u || ' ' || trim(l); END IF;
	END LOOP;
	RETURN u;
END $$;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
UPDATE pg_index SET indisvalid = false WHERE indexrelid IN ('ka_off'::regclass);
SELECT kunits('SELECT count(*), sum(a1) FROM ka WHERE tenant_id = 42') AS key_cut;
SELECT count(*), sum(a1), min(id), max(id) FROM ka WHERE tenant_id = 42;
UPDATE pg_index SET indisvalid = (indexrelid = 'ka_off'::regclass) WHERE indexrelid IN ('ka_auto'::regclass, 'ka_off'::regclass);
SELECT kunits('SELECT count(*), sum(a1) FROM ka WHERE tenant_id = 42') AS by_superblock;
SELECT count(*), sum(a1), min(id), max(id) FROM ka WHERE tenant_id = 42;
UPDATE pg_index SET indisvalid = true WHERE indexrelid IN ('ka_auto'::regclass, 'ka_off'::regclass);

-- a rebuild decides again: the choice of the build is written as 'auto: col', 'off' stays
REINDEX INDEX ka_auto;
REINDEX INDEX ka_off;
SELECT kopt('ka_auto'), kopt('ka_off');
ALTER INDEX ka_auto SET (key_cut = 'off');
REINDEX INDEX ka_auto;
SELECT kopt('ka_auto');
SELECT format FROM sift.col_segments('ka_auto') WHERE kind = 'immutable';

-- the NOTICE says what was chosen and how to turn it off
SET client_min_messages = notice;
DROP INDEX ka_auto, ka_off;
CREATE INDEX ka_auto ON ka USING sift_col (tenant_id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);

-- an index that would grow: the build says what the cut would read and cost, with the command; sift.key_cut_decision asks the same of an index that exists
-- (small rows, 20 to a page: the prediction counts the addresses of the rows, not what the cut saves in the pre-aggregates, so it is an upper bound)
DROP INDEX ka_auto;
SET client_min_messages = warning;
CREATE TABLE kg (id int, tenant_id int, a1 int, a2 int, a3 int) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kg SELECT i, (random() * 100)::int, (random() * 1000)::int, (random() * 1000)::int, (random() * 1000)::int FROM generate_series(1, 400000) i;
VACUUM (FREEZE, ANALYZE) kg;
SET client_min_messages = notice;
CREATE INDEX kg_narrow ON kg USING sift_col (tenant_id, a1, a2, a3);
SET client_min_messages = warning;
CREATE INDEX ka_seq ON ka USING sift_col (tenant_id, seq, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
CREATE INDEX ka_wide ON ka USING sift_col (tenant_id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
SELECT 'narrow' AS idx, leaf, decision, growth_pct > 3 AS grows, reason FROM sift.key_cut_decision('kg_narrow')
UNION ALL SELECT 'seq', leaf, decision, growth_pct > 3, reason FROM sift.key_cut_decision('ka_seq')
UNION ALL SELECT 'wide', leaf, decision, growth_pct > 3, reason FROM sift.key_cut_decision('ka_wide');
-- a table that was never analyzed has no statistics: a unit aims at the rows a superblock holds (the key cut of its build used to aim at 1024, 4 times too small
-- here, and its units cost 30% of the index), and the decision reads the rows of a superblock from the heap
CREATE TABLE ku (id int, tenant_id int, a1 int, a2 int, a3 int) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ku SELECT i, (random() * 100)::int, (random() * 1000)::int, (random() * 1000)::int, (random() * 1000)::int FROM generate_series(1, 400000) i;
CREATE INDEX ku_cut ON ku USING sift_col (tenant_id, a1, a2, a3) WITH (key_cut = 'tenant_id');
SELECT sum(units) BETWEEN pg_relation_size('ku') / 8192 / 256 / 2 AND 2 * pg_relation_size('ku') / 8192 / 256 AS units_of_the_superblocks FROM sift.col_segments('ku_cut') WHERE kind = 'immutable';
SELECT leaf, decision FROM sift.key_cut_decision('ku_cut');
-- a time leaf: when no key is worth cutting by, a range of a date or timestamp that holds the rows of one unit is tested the same way; a timestamp that follows the heap is not
CREATE TABLE kt (id int, tenant int, ts timestamptz, tarr timestamptz, a1 int, a2 int, a3 int, a4 int, a5 int, a6 int, a7 int, a8 int, a9 int, a10 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kt SELECT i, tenant, ts, '2024-01-01'::timestamptz + i * interval '5 minutes', d, d / 7, d / 30, d % 7, d % 30,
	(i * 53) % 1000, (i * 59) % 1000, (i * 61) % 1000, (i * 67) % 1000, (i * 71) % 1000, repeat('x', 100)
FROM (SELECT i, (random() * 100)::int AS tenant, '2024-01-01'::timestamptz + random() * interval '730 days' AS ts FROM generate_series(1, 125000) i) s,
	LATERAL (SELECT floor(extract(epoch FROM ts) / 86400)::int AS d) l;
VACUUM (FREEZE, ANALYZE) kt;
CREATE INDEX kt_auto ON kt USING sift_col (tenant, ts, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
CREATE INDEX kt_arrival ON kt USING sift_col (tenant, tarr, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
CREATE INDEX kt_both ON kt USING sift_col (tenant, ts, tarr, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
CREATE INDEX kt_narrow ON kt USING sift_col (tenant, ts, a6, a7);
SELECT kopt('kt_auto'), kopt('kt_arrival'), kopt('kt_both'), kopt('kt_narrow');
SELECT 'auto' AS idx, leaf, decision, reason FROM sift.key_cut_decision('kt_auto')
UNION ALL SELECT 'arrival', leaf, decision, reason FROM sift.key_cut_decision('kt_arrival')
UNION ALL SELECT 'both', leaf, decision, reason FROM sift.key_cut_decision('kt_both')
UNION ALL SELECT 'narrow', leaf, decision, reason FROM sift.key_cut_decision('kt_narrow');
CREATE INDEX kt_off ON kt USING sift_col (tenant, ts, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) WITH (key_cut = 'off');
SET enable_indexscan = off; SET enable_bitmapscan = off; SET seq_page_cost = 10000; SET random_page_cost = 10000;
UPDATE pg_index SET indisvalid = false WHERE indexrelid IN ('kt_off'::regclass);
SELECT kunits($$SELECT count(*), sum(a1) FROM kt WHERE ts >= '2024-06-01' AND ts < '2024-06-02'$$) AS time_cut;
SELECT count(*), sum(a1), min(id) FROM kt WHERE ts >= '2024-06-01' AND ts < '2024-06-02';
UPDATE pg_index SET indisvalid = (indexrelid = 'kt_off'::regclass) WHERE indexrelid IN ('kt_auto'::regclass, 'kt_off'::regclass);
SELECT kunits($$SELECT count(*), sum(a1) FROM kt WHERE ts >= '2024-06-01' AND ts < '2024-06-02'$$) AS by_superblock;
SELECT count(*), sum(a1), min(id) FROM kt WHERE ts >= '2024-06-01' AND ts < '2024-06-02';
UPDATE pg_index SET indisvalid = true WHERE indexrelid IN ('kt_auto'::regclass, 'kt_off'::regclass);
SELECT sift.col_verify('kt_auto', true);
DROP TABLE kt;
DROP TABLE ka, kg, ku;
DROP FUNCTION kopt(regclass), kunits(text);
DROP EXTENSION pg_sift;
