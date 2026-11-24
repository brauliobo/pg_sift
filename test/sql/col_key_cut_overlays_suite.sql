-- The key cuts of a sift_col index. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> col_key_order_cut
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- key_order = 'group, key' with key_cut = 'group' cuts the units by the group: the order arrays answer the lists of one group, the cut the counts, the ranges and the
-- group-bys of the keys, which the arrays alone read like an index with no layout. The answers are the table's.
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

-- ko: 101 groups scattered over the heap, a date scattered, a counter that follows the heap
CREATE TABLE ko (id int PRIMARY KEY, group_id int, d date, seq int, v int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ko SELECT i, i % 101, date '2020-01-01' + (random() * 1500)::int, i, i % 1000, repeat('x', 100) FROM generate_series(1, 60000) i;
CREATE TABLE kp AS SELECT * FROM ko;
VACUUM (FREEZE, ANALYZE) ko;
ANALYZE kp;

-- the lookups: the group cut reads the units of the group, the list is the table's
CREATE INDEX ko_i ON ko USING sift_col (id, group_id, d, seq, v) WITH (key_order = 'group_id, d', key_cut = 'group_id');
CREATE FUNCTION kunits(q text) RETURNS int LANGUAGE plpgsql AS
$$ DECLARE l text; r int;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		IF l ~ 'Units:' THEN r := substring(l FROM 'Units: (\d+) read'); END IF;
	END LOOP;
	RETURN r;
END $$;
SET sift.enable_agg = on;
SET sift.agg_force = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT kunits('SELECT count(*), sum(v) FROM ko WHERE group_id = 17') < 5 AS group_reads_few_units;
SELECT md5(string_agg(t::text, '|')) = (SELECT md5(string_agg(t::text, '|')) FROM (SELECT id, d FROM kp WHERE group_id = 17 ORDER BY d DESC, id DESC LIMIT 20 OFFSET 40) t) AS list_is_the_tables
FROM (SELECT id, d FROM ko WHERE group_id = 17 ORDER BY d DESC, id DESC LIMIT 20 OFFSET 40) t;
SELECT (SELECT (count(*), sum(v), max(d)) FROM ko WHERE group_id BETWEEN 10 AND 30 AND d >= date '2022-01-01') IS NOT DISTINCT FROM
	(SELECT (count(*), sum(v), max(d)) FROM kp WHERE group_id BETWEEN 10 AND 30 AND d >= date '2022-01-01') AS range_is_the_tables;

DROP TABLE ko, kp;
DROP FUNCTION kunits(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< col_key_order_cut

-- >>> col_key_order_auto
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- The order arrays of a time column (the default choice) let ORDER BY ts LIMIT n read the arrays and not every unit, with or without a key_cut of the user's;
-- key_order = 'off' keeps a cut alone, a key_order or an order_array_columns of the user's is left as it is.
SET sift.async_maintenance = off;
SET client_min_messages = warning;
SET lc_messages = 'C';
\set VERBOSITY terse

CREATE TABLE ka (id int, acct_id int, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ka SELECT i, 1 + (hashint4(i) & 2147483647) % 5000, (hashint4(i + 1) & 2147483647) % 1000, repeat('x', 100) FROM generate_series(1, 100000) i;
VACUUM (FREEZE, ANALYZE) ka;
CREATE TABLE kb (id int, t timestamp, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO kb SELECT i, timestamp '2020-01-01' + ((hashint4(i) & 2147483647) % 157766400) * interval '1 second', (hashint4(i + 1) & 2147483647) % 1000, repeat('x', 100) FROM generate_series(1, 100000) i;
VACUUM (FREEZE, ANALYZE) kb;

CREATE FUNCTION ka_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ra;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN used THEN 'array' ELSE 'units' END;
END $$;

CREATE INDEX ka_auto ON ka USING sift_col (acct_id, v) WITH (key_cut = 'acct_id');
-- a key that is not a time column keeps the cut alone
SELECT reloptions @> ARRAY['key_cut=acct_id'] AS cut, NOT reloptions::text LIKE '%key_order%' AS no_key_order FROM pg_class WHERE relname = 'ka_auto';
-- an equality on the key of a table in arrival order is read from the one unit it is cut into, not from a heap page for each of its rows (the Index Scan was priced as if they were in heap order)
CREATE FUNCTION ka_scan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text;
BEGIN
	FOR pl IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		IF pl ~ 'Scan' THEN RETURN substring(pl from '(Index Only Scan|Index Scan|Bitmap Heap Scan|Seq Scan|Custom Scan)'); END IF;
	END LOOP;
END $$;
SELECT ka_scan('SELECT acct_id, v FROM ka WHERE acct_id = 7') AS key_lookup, ka_scan('SELECT acct_id, v FROM ka WHERE acct_id IN (7, 8, 9)') AS key_list;
DROP INDEX ka_auto;

CREATE INDEX kb_auto ON kb USING sift_col (t, v);
SELECT reloptions @> ARRAY['order_array_columns=auto: t, v'] AS order_arrays FROM pg_class WHERE relname = 'kb_auto';
SELECT ka_chk('SELECT t, v FROM kb ORDER BY t, id LIMIT 20') AS top20,
       ka_chk('SELECT t, v FROM kb ORDER BY t, id LIMIT 20 OFFSET 3000') AS offset3000,
       ka_chk('SELECT t, v FROM kb WHERE t BETWEEN ''2021-01-01'' AND ''2021-01-04'' ORDER BY t, id LIMIT 50') AS range_limit;
-- REINDEX decides again, and gets the same
REINDEX INDEX kb_auto;
SELECT reloptions @> ARRAY['order_array_columns=auto: t, v'] AS order_arrays FROM pg_class WHERE relname = 'kb_auto';
SELECT sift.col_verify('kb_auto');
DROP INDEX kb_auto;

-- off keeps the cut alone
CREATE INDEX kb_off ON kb USING sift_col (t, v) WITH (key_cut = 't', key_order = 'off');
SELECT reloptions @> ARRAY['key_cut=t', 'key_order=off'] AS cut_kept, NOT reloptions::text LIKE '%key_order=auto%' AS no_arrays FROM pg_class WHERE relname = 'kb_off';
SELECT ka_chk('SELECT t, v FROM kb ORDER BY t, id LIMIT 20') AS top20_without;
DROP INDEX kb_off;

-- a key_cut of the user's ('off') takes the chosen key_order with it at the next REINDEX
CREATE INDEX kb_cutoff ON kb USING sift_col (t, v);
ALTER INDEX kb_cutoff SET (key_cut = 'off');
REINDEX INDEX kb_cutoff;
SELECT reloptions @> ARRAY['key_cut=off'] AS cut_off, NOT reloptions::text LIKE '%key_order%' AS key_order_gone FROM pg_class WHERE relname = 'kb_cutoff';
DROP INDEX kb_cutoff;

-- a key_order of the user's wins: the rule leaves the cut to it
CREATE INDEX kb_user ON kb USING sift_col (t, v) WITH (key_order = 'v');
SELECT reloptions @> ARRAY['key_order=v'] AS user_wins, NOT reloptions::text LIKE '%key_cut%' AS no_auto_cut FROM pg_class WHERE relname = 'kb_user';
DROP INDEX kb_user;
-- an order_array_columns of the user's (even 'off') is a layout of its own
CREATE INDEX kb_arr ON kb USING sift_col (t, v) WITH (order_array_columns = 'off');
SELECT NOT reloptions::text LIKE '%key_order%' AS no_key_order FROM pg_class WHERE relname = 'kb_arr';
DROP INDEX kb_arr;
-- an entity and a date: the latest rows of one entity read entries of its group, the heap in the order of the dates
CREATE TABLE kc (id int, tenant_id int, event_date date, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO kc SELECT i, 1 + least(999, floor(1000 * power((hashint4(i) & 2147483647) / 2147483647.0, 1.5)))::int, date '2022-01-01' + (i * 730 / 100000 + (hashint4(i + 7) & 2147483647) % 3), (hashint4(i + 13) & 2147483647) % 1000, repeat('x', 100) FROM generate_series(1, 100000) i;
VACUUM (FREEZE, ANALYZE) kc;
CREATE INDEX kc_auto ON kc USING sift_col (tenant_id, event_date, amount);
SELECT reloptions @> ARRAY['key_order=auto: tenant_id, event_date'] AS entity_and_date FROM pg_class WHERE relname = 'kc_auto';
SELECT ka_chk('SELECT event_date FROM kc WHERE tenant_id = 7 ORDER BY event_date DESC, id LIMIT 20') AS latest20,
       ka_chk('SELECT event_date FROM kc WHERE tenant_id = 7 AND event_date < ''2023-01-01'' ORDER BY event_date DESC, id LIMIT 20') AS page20;
SELECT sift.col_verify('kc_auto');
DROP INDEX kc_auto;
CREATE INDEX kc_off ON kc USING sift_col (tenant_id, event_date, amount) WITH (key_order = 'off');
SELECT NOT reloptions::text LIKE '%key_order=auto%' AS off_keeps_plain_arrays FROM pg_class WHERE relname = 'kc_off';
DROP INDEX kc_off;
DROP TABLE kc;
-- 200 entities in a heap that follows the date: the plain choice builds no second order there, the layout would add one (+37% of the index): the plain arrays stay
CREATE TABLE kd (id int, tenant_id int, event_date date, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO kd SELECT i, 1 + least(199, floor(200 * power((hashint4(i) & 2147483647) / 2147483647.0, 1.5)))::int, date '2022-01-01' + (i * 730 / 100000 + (hashint4(i + 7) & 2147483647) % 3), (hashint4(i + 13) & 2147483647) % 1000, repeat('x', 100) FROM generate_series(1, 100000) i;
VACUUM (FREEZE, ANALYZE) kd;
CREATE INDEX kd_auto ON kd USING sift_col (tenant_id, event_date, amount);
SELECT NOT reloptions::text LIKE '%key_order%' AS plain_choice_stays FROM pg_class WHERE relname = 'kd_auto';
DROP TABLE kd;
-- 40 entities in a random order under a cut by the date: the layout would replace the group by another column and gain nothing beyond the noise (B - C of -2 to +7 in four builds), the plain choice stays
CREATE TABLE kf (id int, tenant_id int, event_date date, amount int, status int2, pad text) WITH (autovacuum_enabled = false);
INSERT INTO kf SELECT i, 1 + least(39, floor(40 * power((hashint4(i) & 2147483647) / 2147483647.0, 1.5)))::int, date '2022-01-01' + ((hashint4(i + 7) & 2147483647) % 730), (hashint4(i + 13) & 2147483647) % 1000, 1 + (hashint4(i + 11) & 2147483647) % 5, md5(i::text) FROM generate_series(1, 75000) i;
VACUUM (FREEZE, ANALYZE) kf;
CREATE INDEX kf_auto ON kf USING sift_col (tenant_id, event_date, amount, status) WITH (key_cut = 'event_date');
SELECT reloptions @> ARRAY['key_cut=event_date'] AS cut_by_date, NOT reloptions::text LIKE '%key_order%' AS plain_choice_stays FROM pg_class WHERE relname = 'kf_auto';
DROP TABLE kf;
-- a date cut with 730 values, no entity: runs of equal keys, a top-N with a filter reads the units the cut keeps (12 blocks), not the order of the key (441 heap fetches): the plain choice stays
CREATE TABLE ke (id int, event_date date, status int2, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ke SELECT i, date '2022-01-01' + ((hashint4(i + 7) & 2147483647) % 730), (hashint4(i + 11) & 2147483647) % 5, (hashint4(i + 13) & 2147483647) % 1000, md5(i::text) FROM generate_series(1, 100000) i;
VACUUM (FREEZE, ANALYZE) ke;
CREATE INDEX ke_auto ON ke USING sift_col (event_date, status, amount) WITH (key_cut = 'event_date');
SELECT reloptions @> ARRAY['key_cut=event_date'] AS cut_by_date, NOT reloptions::text LIKE '%key_order%' AS plain_choice_stays FROM pg_class WHERE relname = 'ke_auto';
DROP TABLE ke;
DROP FUNCTION ka_chk(text), ka_scan(text);
DROP TABLE ka, kb;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< col_key_order_auto

-- >>> col_key_cut_range
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- key_cut by the start lane of a range or the node of a network: a containment test reads the few units whose lane meets it.
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- 28 superblocks (fillfactor 10 keeps the rows few); ranges and addresses scattered over them, a range column in heap order, a sequential id
CREATE TABLE kr (id int, r int4range, ordered int4range, ip inet, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kr SELECT (random() * 1000000)::int, int4range(a, a + 1 + (random() * 5000)::int), int4range(i * 4, i * 4 + 3),
	('20.0.0.0'::inet + (random() * 16777215)::int), repeat('x', 100)
FROM (SELECT i, (random() * 30000000)::int a FROM generate_series(1, 50000) i) s;
VACUUM (FREEZE, ANALYZE) kr;

CREATE FUNCTION kopt(i regclass) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.index_key_cut_reloption(i) $$;

-- the start of the ranges is scattered over the units
CREATE INDEX kr_auto ON kr USING sift_col (id, r, ip) WITH (key_cut = 'r#1');
SELECT kopt('kr_auto');
SELECT sift.col_verify('kr_auto', true);

-- the switch and an explicit list
CREATE INDEX kr_off ON kr USING sift_col (id, r, ip) WITH (key_cut = 'off');
SELECT kopt('kr_off');
CREATE INDEX kr_list ON kr USING sift_col (id, r, ip) WITH (key_cut = 'r#1');
SELECT kopt('kr_list');

-- the answers are the table's, and the cut index reads fewer units than the one by superblock
-- the units a containment test keeps, of all (the plan lists the kept and the pruned ones)
CREATE FUNCTION kept_share(q text) RETURNS numeric LANGUAGE sql AS $$ SELECT sift_helpers.agg_units_kept_share(q) $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true); PERFORM set_config('sift.agg_force', 'on', true); EXECUTE q INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true); EXECUTE q INTO b;
	RETURN a = b;
END $$;
SELECT same('SELECT count(*) FROM kr WHERE r @> 12121212'), same('SELECT count(*) FROM kr WHERE r && int4range(15000000, 15000100)'),
       same('SELECT count(*) FROM kr WHERE r <@ int4range(15000000, 15002000)'), same('SELECT count(*) FROM kr WHERE ip <<= ''20.5.0.0/20'''),
       same('SELECT count(*) FROM kr WHERE ip >>= ''20.5.7.9''');
SELECT sift.col_merge('kr_auto', true) >= 0;
SELECT same('SELECT count(*) FROM kr WHERE r @> 12121212'), same('SELECT count(*) FROM kr WHERE r && int4range(15000000, 15000100)');
DROP INDEX kr_off, kr_list;
SELECT kept_share('SELECT count(*) FROM kr WHERE r @> 12121212') < 0.2, kept_share('SELECT count(*) FROM kr WHERE r && int4range(15000000, 15000100)') < 0.2;

-- a test that holds in two ways (adjacent ranges, a network that overlaps or contains the constant) prunes the units that neither way can hold
SELECT same('SELECT count(*) FROM kr WHERE r -|- int4range(15000000, 15000100)'), same('SELECT count(*) FROM kr WHERE ip && ''20.5.0.0/20'''), same('SELECT count(*) FROM kr WHERE ip << ''20.5.0.0/20''');
SELECT kept_share('SELECT count(*) FROM kr WHERE r -|- int4range(15000000, 15000100)') < 0.2;

REINDEX INDEX kr_auto;
SELECT kopt('kr_auto');

DROP TABLE kr;
DROP FUNCTION kopt(regclass), kept_share(text), same(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< col_key_cut_range

-- >>> col_key_cut_second
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- key_cut = 'key, date' sorts the rows of a key by the date leaf: a key with more rows than a unit is split by
-- date and a window of dates skips the units outside it. Sums stay those of the table: exact for integers and numeric, within float rounding for float8.
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- 20 tenants of 3,000 rows (more than a unit holds), dates and measures independent of the tenant and of the heap order; ten leaves that follow nothing
CREATE TABLE kd (id int, tenant_id int, d date, a1 int, a2 int, a3 int, a4 int, a5 int, a6 int, a7 int, a8 int, a9 int, a10 int, n numeric(12,2), f float8, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kd SELECT i, (random() * 20)::int, date '2020-01-01' + (random() * 2000)::int,
	(i * 31) % 1000, (i * 37) % 1000, (i * 41) % 1000, (i * 43) % 1000, (i * 47) % 1000, (i * 53) % 1000, (i * 59) % 1000, (i * 61) % 1000, (i * 67) % 1000, (i * 71) % 1000,
	(random() * 100000)::numeric(12,2), random() * 1000, repeat('x', 100)
FROM generate_series(1, 62500) i;
VACUUM (FREEZE, ANALYZE) kd;

CREATE FUNCTION kopt(i regclass) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.index_key_cut_reloption(i) $$;
CREATE FUNCTION kunits(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_units_lines(q) $$;

-- chosen with the date as the second key; a key list of one column and the time leaf as the key are not extended
CREATE INDEX kd_auto ON kd USING sift_col (tenant_id, d, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, n, f) WITH (key_cut = 'tenant_id, d');
CREATE INDEX kd_key ON kd USING sift_col (tenant_id, d, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, n, f) WITH (key_cut = 'tenant_id');
CREATE INDEX kd_off ON kd USING sift_col (tenant_id, d, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, n, f) WITH (key_cut = 'off');
SELECT kopt('kd_auto'), kopt('kd_key'), kopt('kd_off');
SELECT sift.col_verify('kd_auto', true);

-- a window of dates reads fewer units of the index sorted by the date than of the one cut by the tenant alone
SET enable_indexscan = off; SET enable_bitmapscan = off; SET seq_page_cost = 10000; SET random_page_cost = 10000;
UPDATE pg_index SET indisvalid = false WHERE indexrelid IN ('kd_key'::regclass, 'kd_off'::regclass);
SELECT kunits($$SELECT count(*), sum(a1) FROM kd WHERE tenant_id = 7 AND d >= '2021-01-01' AND d < '2021-03-01'$$) AS second_key;
UPDATE pg_index SET indisvalid = (indexrelid = 'kd_key'::regclass) WHERE indexrelid IN ('kd_auto'::regclass, 'kd_key'::regclass);
SELECT kunits($$SELECT count(*), sum(a1) FROM kd WHERE tenant_id = 7 AND d >= '2021-01-01' AND d < '2021-03-01'$$) AS key_only;
UPDATE pg_index SET indisvalid = true WHERE indexrelid IN ('kd_auto'::regclass, 'kd_key'::regclass, 'kd_off'::regclass);

-- the answers are those of the table: integers and numeric exact, float8 within the rounding a parallel plan has (sums in another order)
CREATE TABLE kq (t int, lo date, hi date);
INSERT INTO kq SELECT t, date '2020-01-01' + w, date '2020-01-01' + w + len FROM generate_series(0, 20, 4) t, (VALUES (0, 2000), (100, 30), (900, 365), (1500, 120)) v(w, len);
CREATE FUNCTION kres(sift bool) RETURNS TABLE (t int, lo date, c bigint, s1 bigint, sn numeric, sf float8) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN sift THEN 'on' ELSE 'off' END, true);
	RETURN QUERY SELECT q.t, q.lo, count(*), coalesce(sum(a1), 0)::bigint, sum(n), sum(f) FROM kq q LEFT JOIN kd ON kd.tenant_id = q.t AND kd.d >= q.lo AND kd.d < q.hi GROUP BY q.t, q.lo ORDER BY 1, 2;
END $$;
SELECT count(*) AS cells,
	bool_and(a.c = b.c AND a.s1 = b.s1 AND a.sn IS NOT DISTINCT FROM b.sn) AS exact,
	bool_and(abs(a.sf - b.sf) <= 1e-9 * abs(b.sf) OR (a.sf IS NULL AND b.sf IS NULL)) AS float_within_rounding
FROM kres(true) a JOIN kres(false) b USING (t, lo);

DROP TABLE kq, kd;
DROP FUNCTION kopt(regclass), kunits(text), kres(bool);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< col_key_cut_second

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
