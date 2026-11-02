-- A top-N over a partitioned table that reads one partition walks that partition's order array and fetches from its heap: a key that pins the partition (a constant PostgreSQL
-- prunes with, a parameter of a generic plan or a LATERAL probe, the equality of a hash key), for hash, list and range partitions, and a table PostgreSQL pruned to one. The
-- rows must be the heap's, in order; 'array' says the scan read the array of the partition, 'units' that it read the units (no partition, several, or one whose columns
-- are not the first's).
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

-- hash: 4 partitions of tenants 0 to 39, the last one built with a dropped column so that its attribute numbers are not its siblings'
CREATE TABLE hp (id int NOT NULL, tenant int NOT NULL, ts timestamp NOT NULL, v int, pad text) PARTITION BY HASH (tenant);
CREATE TABLE hp_0 PARTITION OF hp FOR VALUES WITH (MODULUS 4, REMAINDER 0);
CREATE TABLE hp_1 PARTITION OF hp FOR VALUES WITH (MODULUS 4, REMAINDER 1);
CREATE TABLE hp_2 PARTITION OF hp FOR VALUES WITH (MODULUS 4, REMAINDER 2);
CREATE TABLE hp_3 (junk int, id int NOT NULL, tenant int NOT NULL, ts timestamp NOT NULL, v int, pad text);
ALTER TABLE hp_3 DROP COLUMN junk;
ALTER TABLE hp ATTACH PARTITION hp_3 FOR VALUES WITH (MODULUS 4, REMAINDER 3);
INSERT INTO hp SELECT i, i % 40, timestamp '2025-01-01' + (i * 7919 % 7776000) * interval '1 second', CASE WHEN i % 17 = 0 THEN NULL ELSE i % 50 END, repeat('x', 40)
FROM generate_series(1, 12000) i;
CREATE INDEX hp_c ON hp USING sift_col (id, tenant, ts, v) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'tenant');

-- list and range by a month: the key is the first of the order too
CREATE TABLE lp (id int NOT NULL, region int NOT NULL, ts timestamp NOT NULL, v int) PARTITION BY LIST (region);
CREATE TABLE lp_a PARTITION OF lp FOR VALUES IN (1, 2, 3);
CREATE TABLE lp_b PARTITION OF lp FOR VALUES IN (4, 5);
CREATE TABLE lp_c PARTITION OF lp FOR VALUES IN (6, 7, 8, 9);
INSERT INTO lp SELECT i, 1 + i % 9, timestamp '2025-01-01' + (i * 104729 % 5000000) * interval '1 second', i % 30 FROM generate_series(1, 9000) i;
CREATE INDEX lp_c_i ON lp USING sift_col (id, region, ts, v) WITH (order_columns = 'ts', order_array_columns = 'ts');
CREATE TABLE rp (id int NOT NULL, tenant int NOT NULL, ts timestamp NOT NULL) PARTITION BY RANGE (ts);
CREATE TABLE rp_1 PARTITION OF rp FOR VALUES FROM ('2025-01-01') TO ('2025-02-01');
CREATE TABLE rp_2 PARTITION OF rp FOR VALUES FROM ('2025-02-01') TO ('2025-03-01');
CREATE TABLE rp_3 PARTITION OF rp FOR VALUES FROM ('2025-03-01') TO ('2025-04-01');
INSERT INTO rp SELECT i, i % 20, timestamp '2025-01-01' + (i * 7919 % 7776000) * interval '1 second' FROM generate_series(1, 9000) i;
CREATE INDEX rp_c ON rp USING sift_col (id, tenant, ts) WITH (order_columns = 'ts', order_array_columns = 'ts');
CREATE TABLE tn AS SELECT NULL::int AS id UNION ALL SELECT n FROM generate_series(0, 45) n;
VACUUM (FREEZE, ANALYZE) hp, lp, rp, tn;
-- rows after the build: the write buffer, an update and a delete of the partition a probe reads
INSERT INTO hp SELECT 20000 + i, i % 40, timestamp '2025-04-01' + i * interval '1 minute', i % 50, 'y' FROM generate_series(1, 400) i;
UPDATE hp SET ts = timestamp '2026-01-01' + id * interval '1 second' WHERE id % 211 = 0;
DELETE FROM hp WHERE id % 307 = 0;

-- 'array' or 'units' when the rows are those of the normal plan, in order; 'DIFFERS ...' otherwise
CREATE FUNCTION pl_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; scans int := 0; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off; SET LOCAL enable_indexscan = off;
	SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		used := used OR pl LIKE '%Global Order%';
		IF pl LIKE '%Custom Scan (SiftAgg)%' THEN scans := scans + 1; END IF;
	END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN scans = 0 THEN 'no scan' WHEN used THEN 'array' ELSE 'units' END;
END $$;

CREATE TABLE pq (n int, q text);
INSERT INTO pq SELECT n, q FROM (VALUES
	-- constants: hash prunes to one partition, every tenant (the empty ones too)
	(1, 'SELECT id, ts FROM hp WHERE tenant = %1$s ORDER BY ts DESC, id LIMIT 5'),
	(2, 'SELECT id, ts FROM hp WHERE tenant = %1$s ORDER BY ts, id LIMIT 3'),
	(3, 'SELECT id, ts FROM hp WHERE tenant = %1$s ORDER BY ts DESC, id LIMIT 5 OFFSET 2'),
	(4, 'SELECT id, ts FROM hp WHERE tenant = %1$s AND id > 3000 ORDER BY ts DESC, id LIMIT 4'),
	-- a list partition pruned to one (the key is the partition's list), a range pruned to a month
	(5, 'SELECT id, ts FROM lp WHERE region = (%1$s %% 9) + 1 ORDER BY ts DESC, id LIMIT 6'),
	(6, 'SELECT id, ts FROM rp WHERE ts >= date ''2025-02-01'' AND ts < date ''2025-03-01'' ORDER BY ts DESC, id LIMIT 5'),
	-- not one partition: the units
	(7, 'SELECT id, ts FROM hp WHERE tenant IN (%1$s, %1$s + 1, %1$s + 2) ORDER BY ts DESC, id LIMIT 5'),
	(8, 'SELECT id, ts FROM hp ORDER BY ts DESC, id LIMIT 5'),
	(9, 'SELECT id, ts FROM lp WHERE region IN (1, 5) ORDER BY ts DESC, id LIMIT 5')
) v(n, q);

-- every tenant of the table and some that no row holds, per query
SELECT n, result, count(*) FROM pq, LATERAL (SELECT pl_chk(format(pq.q, t)) AS result FROM generate_series(0, 45) t) r GROUP BY n, result ORDER BY n, result;

-- the probes of a LATERAL join: the partition the value belongs to changes at each (the first, the last, one no row holds, and NULL, the first probe: the plan shows the last) and the rows are the same as the normal plan's
CREATE TABLE lq (n int, q text);
INSERT INTO lq VALUES
	(1, 'SELECT d.id, x.id AS xid, x.ts FROM tn d LEFT JOIN LATERAL (SELECT e.id, e.ts FROM hp e WHERE e.tenant = d.id ORDER BY e.ts DESC, e.id LIMIT 1) x ON true'),
	(2, 'SELECT d.id, x.id AS xid, x.ts FROM tn d LEFT JOIN LATERAL (SELECT e.id, e.ts FROM hp e WHERE e.tenant = d.id ORDER BY e.ts, e.id LIMIT 3) x ON true'),
	(3, 'SELECT d.id, x.id AS xid, x.ts FROM tn d LEFT JOIN LATERAL (SELECT e.id, e.ts FROM hp e WHERE e.tenant = d.id AND e.v < 25 ORDER BY e.ts DESC, e.id LIMIT 2 OFFSET 1) x ON true'),
	(4, 'SELECT d.id, (SELECT e.ts FROM hp e WHERE e.tenant = d.id ORDER BY e.ts DESC, e.id LIMIT 1) FROM tn d'),
	(5, 'SELECT d.id, x.id AS xid, x.ts FROM tn d LEFT JOIN LATERAL (SELECT e.id, e.ts FROM lp e WHERE e.region = d.id ORDER BY e.ts DESC, e.id LIMIT 2) x ON true');
SELECT n, pl_chk(q) FROM lq ORDER BY n;

-- the shapes the per-partition arms served, and ones they refused, keep the rows of the normal plan whatever the scan does: a partition with no index (the normal plan), a hash key of two columns
-- pinned by both, a hash table with partitions of its own, a sort by a column that has no array (the units of the one partition), an OFFSET, a sub-select
CREATE TABLE h2 (id int NOT NULL, a int NOT NULL, b int NOT NULL, ts timestamp NOT NULL, v int) PARTITION BY HASH (a, b);
CREATE TABLE h2_0 PARTITION OF h2 FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE h2_1 PARTITION OF h2 FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE h2_2 PARTITION OF h2 FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO h2 SELECT i, i % 7, i % 5, timestamp '2025-01-01' + (i * 7919 % 7776000) * interval '1 second', i % 40 FROM generate_series(1, 6000) i;
CREATE INDEX h2_c ON h2 USING sift_col (id, a, b, ts, v) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'a');
CREATE TABLE hs (id int NOT NULL, tenant int NOT NULL, region int NOT NULL, ts timestamp NOT NULL) PARTITION BY HASH (tenant);
CREATE TABLE hs_0 PARTITION OF hs FOR VALUES WITH (MODULUS 2, REMAINDER 0) PARTITION BY LIST (region);
CREATE TABLE hs_0a PARTITION OF hs_0 FOR VALUES IN (0, 1);
CREATE TABLE hs_0b PARTITION OF hs_0 FOR VALUES IN (2, 3);
CREATE TABLE hs_1 PARTITION OF hs FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO hs SELECT i, i % 12, i % 4, timestamp '2025-01-01' + (i * 104729 % 5000000) * interval '1 second' FROM generate_series(1, 4000) i;
CREATE INDEX hs_c ON hs USING sift_col (id, tenant, region, ts) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'tenant');
CREATE TABLE hn (id int NOT NULL, tenant int NOT NULL, ts timestamp NOT NULL) PARTITION BY HASH (tenant);
CREATE TABLE hn_0 PARTITION OF hn FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE hn_1 PARTITION OF hn FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO hn SELECT i, i % 10, timestamp '2025-01-01' + i * interval '1 minute' FROM generate_series(1, 3000) i;
CREATE INDEX hn_c0 ON hn_0 USING sift_col (id, tenant, ts) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'tenant');
VACUUM (FREEZE, ANALYZE) h2, hs, hn;

CREATE TABLE xq (n int, q text);
INSERT INTO xq VALUES
	(1, 'SELECT d.id, x.id AS xid, x.ts FROM tn d LEFT JOIN LATERAL (SELECT e.id, e.ts FROM hn e WHERE e.tenant = d.id ORDER BY e.ts DESC LIMIT 2) x ON true'),
	(2, 'SELECT d.id, x.id AS xid FROM tn d LEFT JOIN LATERAL (SELECT e.id FROM h2 e WHERE e.a = d.id AND e.b = 2 ORDER BY e.ts DESC, e.id LIMIT 3) x ON true WHERE d.id < 9'),
	(3, 'SELECT d.id, x.id AS xid FROM tn d LEFT JOIN LATERAL (SELECT e.id FROM h2 e WHERE e.a = 3 AND e.b = d.id ORDER BY e.ts, e.id LIMIT 3) x ON true WHERE d.id < 9'),
	(4, 'SELECT d.id, x.id AS xid, x.ts FROM tn d LEFT JOIN LATERAL (SELECT e.id, e.ts FROM hs e WHERE e.tenant = d.id ORDER BY e.ts DESC, e.id LIMIT 2) x ON true WHERE d.id < 14'),
	(5, 'SELECT d.id, x.id AS xid FROM tn d LEFT JOIN LATERAL (SELECT e.id FROM hp e WHERE e.tenant = d.id ORDER BY e.v DESC NULLS LAST, e.id LIMIT 2) x ON true WHERE d.id < 14'),
	(6, 'SELECT d.id, x.id AS xid FROM tn d LEFT JOIN LATERAL (SELECT e.id FROM hp e WHERE e.tenant = d.id ORDER BY e.ts DESC, e.id LIMIT 2 OFFSET 3) x ON true WHERE d.id < 14'),
	(7, 'SELECT d.id, (SELECT max(e.ts) FROM hp e WHERE e.tenant = d.id) FROM tn d WHERE d.id < 14');
SELECT n, pl_chk(q) FROM xq ORDER BY n;
-- two-column key pinned by both: the constants prune to one partition
SELECT result, count(*) FROM (SELECT pl_chk(format('SELECT id, ts FROM h2 WHERE a = %s AND b = %s ORDER BY ts DESC, id LIMIT 4', a, b)) AS result FROM generate_series(0, 7) a, generate_series(0, 5) b) r GROUP BY 1 ORDER BY 1;
-- by one column only: every partition is read
SELECT pl_chk('SELECT id, ts FROM h2 WHERE a = 2 ORDER BY ts DESC, id LIMIT 4');

-- ORDER BY the group of the second order, then the key, lists the rows group after group (order_array_group): a partitioned table with one partition and no condition reads that order of its
-- partition, the rows the normal plan's; with two partitions the scan reads the units
CREATE TABLE gu (id int NOT NULL, g int NOT NULL, ts timestamp NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE gu_1 PARTITION OF gu FOR VALUES FROM (0) TO (100000);
CREATE TABLE gv (id int NOT NULL, g int NOT NULL, ts timestamp NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE gv_1 PARTITION OF gv FOR VALUES FROM (0) TO (3000);
CREATE TABLE gv_2 PARTITION OF gv FOR VALUES FROM (3000) TO (100000);
INSERT INTO gu SELECT i, 1 + abs(hashint4(i)) % 40, timestamp '2025-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second', i % 30 FROM generate_series(1, 6000) i;
INSERT INTO gv SELECT * FROM gu;
CREATE INDEX gu_c ON gu USING sift_col (id, g, ts, v) WITH (key_cut = 'off', order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'g');
CREATE INDEX gv_c ON gv USING sift_col (id, g, ts, v) WITH (key_cut = 'off', order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'g');
VACUUM (FREEZE, ANALYZE) gu, gv;
INSERT INTO gu SELECT 20000 + i, 1 + i % 40, timestamp '2026-01-01' + i * interval '1 minute', i % 30 FROM generate_series(1, 300) i;
DELETE FROM gu WHERE id % 211 = 0;
CREATE TABLE bq (n int, q text);
INSERT INTO bq SELECT n, q FROM (VALUES
	(1, 'SELECT id, g, ts FROM %1$s ORDER BY g, ts, id LIMIT 20'),
	(2, 'SELECT id, g, ts FROM %1$s ORDER BY g DESC, ts DESC, id LIMIT 20 OFFSET 7'),
	(3, 'SELECT id, g, ts FROM %1$s ORDER BY g, ts, id LIMIT 5 OFFSET 5000'),
	(4, 'SELECT g FROM %1$s ORDER BY g LIMIT 10 OFFSET 3000')
) v(n, q);
SELECT n, t, pl_chk(format(q, t)) FROM bq, unnest(ARRAY['gu', 'gv']) t ORDER BY n, t;

-- a generic plan: the parameter is known when the plan starts, and the partition changes with the value of each execution (the settings come with the function, so the plan cached
-- on its first call is the one of its settings)
SET plan_cache_mode = force_generic_plan;
CREATE FUNCTION gp_sift(t int) RETURNS text LANGUAGE plpgsql SET sift.enable_agg = on SET sift.agg_force = on SET enable_seqscan = off SET enable_sort = off AS
$$ DECLARE r text; BEGIN SELECT string_agg(x::text, ',' ORDER BY ord) INTO r FROM (SELECT row_number() OVER () AS ord, * FROM (SELECT id, ts FROM hp WHERE tenant = t ORDER BY ts DESC, id LIMIT 4) q) x; RETURN r; END $$;
CREATE FUNCTION gp_norm(t int) RETURNS text LANGUAGE plpgsql SET sift.enable_agg = off AS
$$ DECLARE r text; BEGIN SELECT string_agg(x::text, ',' ORDER BY ord) INTO r FROM (SELECT row_number() OVER () AS ord, * FROM (SELECT id, ts FROM hp WHERE tenant = t ORDER BY ts DESC, id LIMIT 4) q) x; RETURN r; END $$;
PREPARE gp(int) AS SELECT id, ts FROM hp WHERE tenant = $1 ORDER BY ts DESC, id LIMIT 4;
-- the partition of the value is one of four the plan has: its array is walked, except in the one whose columns have other attribute numbers (hp_3)
CREATE FUNCTION gp_used(t int) RETURNS text LANGUAGE plpgsql SET sift.enable_agg = on SET sift.agg_force = on SET enable_seqscan = off SET enable_sort = off AS
$$ DECLARE pl text; used bool := false; BEGIN
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) EXECUTE gp(' || t || ')' LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	RETURN CASE WHEN used THEN 'array' ELSE 'units' END; END $$;
SELECT gp_used(t) AS read, count(*) AS tenants, array_agg(t ORDER BY t) FILTER (WHERE t < 12) AS first FROM generate_series(0, 45) t GROUP BY 1 ORDER BY 1;
SELECT count(*) AS executions, count(*) FILTER (WHERE gp_sift(t) IS NOT DISTINCT FROM gp_norm(t)) AS same FROM generate_series(0, 45) t;
RESET plan_cache_mode;

DROP FUNCTION pl_chk(text), gp_used(int), gp_sift(int), gp_norm(int);
DROP TABLE pq, lq, xq, bq, tn, hp, lp, rp, h2, hs, hn, gu, gv;
DROP EXTENSION pg_sift CASCADE;
