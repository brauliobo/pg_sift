-- TEMP sift_table and sift_col never reach the registries in shared memory: no other backend can read a temporary relation, so churning through them evicts no manifest of a permanent
-- table and takes no slot of it.
CREATE EXTENSION pg_sift_test CASCADE;

CREATE TABLE perm_t (id int, v int) USING sift_table;
INSERT INTO perm_t SELECT g, g FROM generate_series(1, 200) g;
SELECT count(*) FROM perm_t;
CREATE TEMP TABLE base AS SELECT sift._test_shm_manifest_stat('slots_valid') AS slots, sift._test_shm_manifest_stat('area_bytes') AS area_bytes,
  sift._test_shm_manifest_stat('evictions') AS evictions;

-- every flavor of temporary relation, read, written, indexed, vacuumed and dropped
CREATE FUNCTION churn(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		EXECUTE format('CREATE TEMP TABLE tt_%s (id int, v int) USING sift_table ON COMMIT PRESERVE ROWS', i);
		EXECUTE format('INSERT INTO tt_%s SELECT g, g FROM generate_series(1, 100) g', i);
		EXECUTE format('UPDATE tt_%s SET v = v + 1 WHERE id %% 7 = 0', i);
		EXECUTE format('SELECT count(*) FROM tt_%s', i);
		EXECUTE format('CREATE TEMP TABLE th_%s (id int, v int)', i);
		EXECUTE format('INSERT INTO th_%s SELECT g, g FROM generate_series(1, 100) g', i);
		EXECUTE format('CREATE UNIQUE INDEX th_%s_i ON th_%s USING sift_col (id, v)', i, i);
		EXECUTE format('SELECT count(*) FROM th_%s', i);
		EXECUTE format('DROP TABLE tt_%s, th_%s', i, i);
	END LOOP;
END $$;
SELECT churn(40);
SELECT sift._test_shm_manifest_stat('slots_valid') - slots AS slots_taken, sift._test_shm_manifest_stat('area_bytes') - area_bytes AS area_grown,
  sift._test_shm_manifest_stat('evictions') - evictions AS evicted FROM base;

-- the same inside one transaction with ON COMMIT DROP and DELETE ROWS
BEGIN;
CREATE TEMP TABLE oc_drop (id int, v int) USING sift_table ON COMMIT DROP;
CREATE TEMP TABLE oc_del (id int, v int) USING sift_table ON COMMIT DELETE ROWS;
INSERT INTO oc_drop SELECT g, g FROM generate_series(1, 100) g;
INSERT INTO oc_del SELECT g, g FROM generate_series(1, 100) g;
UPDATE oc_del SET v = 0 WHERE id <= 10;
SELECT count(*) FROM oc_drop, oc_del WHERE oc_drop.id = oc_del.id;
COMMIT;
SELECT sift._test_shm_manifest_stat('slots_valid') - slots AS slots_taken FROM base;

-- the permanent table is still served from the registry, as it was
SELECT count(*), sum(v) FROM perm_t;
SELECT shm_manifest_cached FROM sift.status('perm_t');

DROP FUNCTION churn(int);
DROP TABLE perm_t, base, oc_del;
DROP EXTENSION pg_sift_test, pg_sift;
