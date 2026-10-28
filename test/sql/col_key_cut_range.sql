-- key_cut by default for the start lane of a range or the node of a network: CREATE INDEX and REINDEX sample the heap and cut the
-- units by the lane when a range of its values reads at least 8 times the units its rows fill (and 64 units), no other leaf follows the order of the table, and the index does
-- not grow; a containment test then reads the few units whose lane meets it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- 70 superblocks (fillfactor 10 keeps the rows few); ranges and addresses scattered over them, a range column in heap order, a sequential id
CREATE TABLE kr (id int, r int4range, ordered int4range, ip inet, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kr SELECT (random() * 1000000)::int, int4range(a, a + 1 + (random() * 5000)::int), int4range(i * 4, i * 4 + 3),
	('20.0.0.0'::inet + (random() * 16777215)::int), repeat('x', 100)
FROM (SELECT i, (random() * 30000000)::int a FROM generate_series(1, 125000) i) s;
VACUUM (FREEZE, ANALYZE) kr;

CREATE FUNCTION kopt(i regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = i AND o ~ '^key_cut'), 'none') $$;

-- chosen: the start of the ranges is scattered over the units, nothing else follows the table
CREATE INDEX kr_auto ON kr USING sift_col (id, r, ip);
SELECT kopt('kr_auto');
SELECT sift.col_verify('kr_auto', true);

-- a range column that follows the heap keeps its zones: not chosen
CREATE INDEX kr_ord ON kr USING sift_col (id, r, ordered, ip);
SELECT kopt('kr_ord');

-- the switch and an explicit list
CREATE INDEX kr_off ON kr USING sift_col (id, r, ip) WITH (key_cut = 'off');
SELECT kopt('kr_off');
CREATE INDEX kr_list ON kr USING sift_col (id, r, ip) WITH (key_cut = 'r#1');
SELECT kopt('kr_list');
SELECT leaf, decision FROM sift.key_cut_decision('kr_auto');

-- the answers are the table's, and the cut index reads fewer units than the one by superblock
-- the units a containment test keeps, of all (the plan lists the kept and the pruned ones)
CREATE FUNCTION kept_share(q text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE l text; kept numeric := 0; pruned numeric := 0;
BEGIN
	PERFORM set_config('sift.agg_force', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ '^ *Units: ' THEN kept := substring(l from '[0-9]+')::numeric; END IF;
		IF l ~ '(\d+) pruned' THEN pruned := substring(l from '(\d+) pruned')::numeric; END IF;
	END LOOP;
	RETURN round(kept / nullif(kept + pruned, 0), 1);
END $$;
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
DROP INDEX kr_off, kr_list, kr_ord;
SELECT kept_share('SELECT count(*) FROM kr WHERE r @> 12121212') < 0.2, kept_share('SELECT count(*) FROM kr WHERE r && int4range(15000000, 15000100)') < 0.2;

-- a test that holds in two ways (adjacent ranges, a network that overlaps or contains the constant) prunes the units that neither way can hold
SELECT same('SELECT count(*) FROM kr WHERE r -|- int4range(15000000, 15000100)'), same('SELECT count(*) FROM kr WHERE ip && ''20.5.0.0/20'''), same('SELECT count(*) FROM kr WHERE ip << ''20.5.0.0/20''');
SELECT kept_share('SELECT count(*) FROM kr WHERE r -|- int4range(15000000, 15000100)') < 0.2;

REINDEX INDEX kr_auto;
SELECT kopt('kr_auto');

-- an index of the address alone: the image of the head is dictionary-coded in the order of the rows and a unit of the cut lists it in the order of the node, so the cut
-- is priced as the shrink it is (7% smaller on 3M random addresses, where the head was priced at one byte a row and the cut at a growth), chosen with no option, and a
-- containment test reads the few units of its node
CREATE TABLE ki (ip inet, pad text) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO ki SELECT '20.0.0.0'::inet + (random() * 16777215)::int, repeat('x', 100) FROM generate_series(1, 250000);
VACUUM (FREEZE, ANALYZE) ki;
CREATE INDEX ki_ip ON ki USING sift_col (ip);
SELECT kopt('ki_ip');
SELECT leaf, decision, growth_pct < 0 AS shrinks FROM sift.key_cut_decision('ki_ip');
SELECT same('SELECT count(*) FROM ki WHERE ip <<= ''20.5.0.0/20'''), same('SELECT count(*) FROM ki WHERE ip >>= ''20.5.7.9'''), same('SELECT count(*) FROM ki WHERE ip = ''20.5.7.9''');
SELECT kept_share('SELECT count(*) FROM ki WHERE ip <<= ''20.5.0.0/20''') < 0.2;
DROP TABLE ki;

-- ... and of IPv6 addresses, whose offset lane (lane 2 of a network) is NULL in every row: it is no leaf that follows the order of the table
CREATE TABLE k6 (ip inet, pad text) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO k6 SELECT format('2001:db8:%s:%s:%s::1', to_hex((random() * 65535)::int), to_hex((random() * 65535)::int), to_hex((random() * 65535)::int))::inet, repeat('x', 100) FROM generate_series(1, 250000);
VACUUM (FREEZE, ANALYZE) k6;
CREATE INDEX k6_ip ON k6 USING sift_col (ip);
SELECT kopt('k6_ip');
SELECT same('SELECT count(*) FROM k6 WHERE ip <<= ''2001:db8:5::/48'''), same('SELECT count(*) FROM k6 WHERE ip >>= ''2001:db8:5:7::1'''), same('SELECT count(*) FROM k6 WHERE ip = ''2001:db8:5:7::1''');
DROP TABLE k6;
DROP TABLE kr;

-- another leaf whose zones skip about half of the units (a date that drifts over the table): the cut by a lane would keep all of them for it, so it is not chosen
CREATE TABLE kp (r int4range, d int, ip inet, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kp SELECT int4range(a, a + 1 + (random() * 5000)::int), (i::bigint * 1000000 / 125000)::int + (random() * 600000)::int,
	('20.0.0.0'::inet + (random() * 16777215)::int), repeat('x', 100)
FROM (SELECT i, (random() * 30000000)::int a FROM generate_series(1, 125000) i) s;
VACUUM (FREEZE, ANALYZE) kp;
-- no order array: it answers the range of d from the heap addresses, with no unit counted pruned, and this asks what the zones skip
CREATE INDEX kp_auto ON kp USING sift_col (r, d, ip) WITH (order_array_columns = off);
SELECT kopt('kp_auto');
SELECT leaf, decision, reason FROM sift.key_cut_decision('kp_auto');
SELECT kept_share('SELECT count(*) FROM kp WHERE d BETWEEN 500000 AND 501000') < 0.6;
DROP TABLE kp;
DROP FUNCTION kopt(regclass), kept_share(text), same(text);
DROP EXTENSION pg_sift;
