-- The registry of the manifests of the indexes in shared memory (core/shm_manifest.c) and the locks of the partitions (core/part_lock.c). A slot is read without a lock between two reads of its
-- seqlock; the snapshot of a manifest is in a shared area, counted by its readers and freed by the last; its bytes are charged to the manifest pool of the ledger. An index is named by a relation number.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TEMP TABLE base AS SELECT sift._test_shm_manifest_stat('snaps_live') AS snaps, sift._test_shm_manifest_stat('snap_bytes') AS bytes,
  sift._test_shm_manifest_stat('evictions') AS evictions, sift._test_shm_pool_used(0) AS pool;
CREATE FUNCTION held() RETURNS TABLE (snaps int8, bytes int8, pool int8) LANGUAGE sql AS
  $$ SELECT sift._test_shm_manifest_stat('snaps_live') - snaps, sift._test_shm_manifest_stat('snap_bytes') - bytes, sift._test_shm_pool_used(0) - pool FROM base $$;

-- an index the registry does not know
SELECT sift._test_shm_manifest_peek(900001) IS NULL AS unknown, sift._test_shm_manifest_read(900001, -1, 0) IS NULL AS no_snapshot;

-- the meta alone: the copy is whole, and there is no snapshot to read
SELECT sift._test_shm_manifest_publish(900001, 7, 0, false) AS published, sift._test_shm_manifest_peek(900001) AS meta, sift._test_shm_manifest_read(900001, -1, 0) IS NULL AS no_snapshot;

-- with a snapshot (4 partitions, 2 scopes): the flags say so, a reader pins it and finds what was published
SELECT sift._test_shm_manifest_publish(900001, 8, 4, true) AS published, sift._test_shm_manifest_peek(900001) AS meta;
SELECT sift._test_shm_manifest_read(900001, -1, 0) AS read, snaps AS snapshots, bytes > 0 AS bytes_charged FROM held();
SELECT sift._test_shm_manifest_stat('area_users') AS area_users;

-- a commit publishes the next version while a reader holds the old: both exist until the reader lets go
SELECT sift._test_shm_manifest_overlap(900001, 9, 4) AS overlap, sift._test_shm_manifest_peek(900001) AS meta;
SELECT snaps AS snapshots FROM held();

-- the list of a partition is made once, charged to the pool and counted in the snapshot, and freed with it
CREATE TEMP TABLE before_list AS SELECT bytes FROM held();
SELECT sift._test_shm_manifest_read(900001, 2, 100) AS first_read, (SELECT bytes FROM held()) - (SELECT bytes FROM before_list) AS bytes_charged;
SELECT sift._test_shm_manifest_read(900001, 2, 100) AS second_read, (SELECT bytes FROM held()) - (SELECT bytes FROM before_list) AS bytes_charged;

-- DROP or REINDEX: readers find nothing and every byte comes back
SELECT sift._test_shm_manifest_invalidate(900001, false);
SELECT sift._test_shm_manifest_peek(900001) IS NULL AS gone, sift._test_shm_manifest_read(900001, -1, 0) IS NULL AS no_snapshot, snaps AS snapshots, bytes AS bytes_left FROM held();

-- a quarantined index is not served and a commit does not bring it back, until it is invalidated
SELECT sift._test_shm_manifest_publish(900002, 3, 0, false) AS published;
SELECT sift._test_shm_manifest_invalidate(900002, true);
SELECT sift._test_shm_manifest_peek(900002) IS NULL AS not_served, sift._test_shm_manifest_publish(900002, 4, 0, false) AS published_over_quarantine;
SELECT sift._test_shm_manifest_invalidate(900002, false);
SELECT sift._test_shm_manifest_publish(900002, 5, 0, false) AS published, sift._test_shm_manifest_peek(900002) AS meta;
SELECT sift._test_shm_manifest_invalidate(900002, false);

-- the ledger holds what the registry counts, to the byte, and gives all of it back
SELECT sift._test_shm_manifest_publish(900003, 1, 4096, true) AS published;
SELECT snaps AS snapshots, bytes > 131072 AS counted, pool = bytes AS ledger_agrees FROM held();
SELECT sift._test_shm_manifest_invalidate(900003, false);
SELECT snaps AS snapshots, bytes AS bytes_left, pool AS pool_left FROM held();

-- a reader that looked up the slot of one index and pins it for another (the slot was given away in between) gets nothing, never the snapshot of the index that holds it now
SELECT sift._test_shm_manifest_publish(900101, 11, 2, true) AS published, sift._test_shm_manifest_publish(900102, 12, 2, true) AS published;
SELECT sift._test_shm_manifest_read_stale(900101, 900101) AS own_snapshot, sift._test_shm_manifest_read_stale(900101, 900102) AS wrong_index, sift._test_shm_manifest_read_stale(900102, 900101) AS other_way;
SELECT sift._test_shm_manifest_invalidate(900101, false), sift._test_shm_manifest_invalidate(900102, false);

-- more indexes than slots: the least used are given up, their snapshots with them, a quarantined one last; nothing is left when every index has been invalidated
SELECT sift._test_shm_manifest_publish(900201, 1, 0, false) AS published;
SELECT sift._test_shm_manifest_invalidate(900201, true);
SELECT count(*) FILTER (WHERE sift._test_shm_manifest_publish(rel::oid, 1, 2, true)) AS published FROM generate_series(910000, 910799) AS rel;
SELECT sift._test_shm_manifest_stat('slots_valid') <= 512 AS within_slots, sift._test_shm_manifest_stat('snaps_live') <= 512 AS snapshots_within_slots,
  sift._test_shm_manifest_stat('evictions') - evictions > 0 AS evicted FROM base;
SELECT count(sift._test_shm_manifest_invalidate(rel::oid, false)) AS invalidated FROM generate_series(910000, 910799) AS rel;
SELECT sift._test_shm_manifest_publish(900201, 2, 0, false) AS published_over_quarantine;
SELECT sift._test_shm_manifest_invalidate(900201, false);
SELECT snaps AS snapshots, bytes AS bytes_left, pool AS pool_left FROM held();

-- a writer and a reader on one meta: no copy is torn
SELECT sift._test_shm_manifest_hammer(900004, 20000, true) AS writer_torn, sift._test_shm_manifest_hammer(900004, 20000, false) AS reader_torn;
SELECT sift._test_shm_manifest_invalidate(900004, false);

DROP FUNCTION held();
DROP TABLE before_list;
SELECT pool AS base_pool FROM base \gset
DROP TABLE base;

-- a session is over for the next one when its backend left the list of backends: its exit callbacks are done by then
CREATE FUNCTION gone(pid int) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..100 LOOP
		IF NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pg_stat_activity.pid = gone.pid) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN false;
END $$;

-- the last backend of the area frees what it holds (the snapshots, their bytes in the ledger): a new session starts clean (the pool as it was when the test began: the write buffers of other tests hold their own pages in it)
SELECT sift._test_shm_manifest_publish(900005, 1, 4096, true) AS published, sift._test_shm_manifest_publish(900006, 1, 4096, true) AS published;
SELECT pg_backend_pid() AS old_pid \gset
\c -
SELECT gone(:old_pid) AS previous_session_gone;
SELECT sift._test_shm_manifest_stat('snaps_live') AS snapshots, sift._test_shm_manifest_stat('snap_bytes') AS bytes_left, sift._test_shm_manifest_stat('area_users') AS area_users,
  sift._test_shm_pool_used(0) - :base_pool AS pool_left;
SELECT sift._test_shm_manifest_read(900005, -1, 0) IS NULL AS no_snapshot;
SELECT sift._test_shm_manifest_invalidate(900005, false), sift._test_shm_manifest_invalidate(900006, false);

-- a backend that ends inside a pin (FATAL runs no cleanup of the pin itself) takes its epoch back: no snapshot of the cluster stays unfreed behind it
SELECT sift._test_shm_manifest_stat('epochs_active') AS before_pin;
SELECT sift._test_shm_manifest_announce_epoch();
SELECT sift._test_shm_manifest_stat('epochs_active') AS inside_pin;
SELECT pg_backend_pid() AS old_pid \gset
\c -
SELECT gone(:old_pid) AS previous_session_gone;
SELECT sift._test_shm_manifest_stat('epochs_active') AS after_exit;

-- the stripe of a partition is the same every time and one of 256; the page lock of a partition is at the top of the block numbers
SELECT sift._test_part_lock_stripe(7, 5) = sift._test_part_lock_stripe(7, 5) AS stable, sift._test_part_lock_stripe(7, 5) BETWEEN 0 AND 255 AS in_range,
  count(DISTINCT sift._test_part_lock_stripe(7, p)) > 100 AS spread FROM generate_series(0, 1023) AS p;
SELECT sift._test_part_lock_blkno(0) AS first, sift._test_part_lock_blkno(1023) AS last, sift._test_part_lock_blkno(1024) AS wraps;

-- a stripe held exclusive keeps out a second holder, shared or exclusive; held shared it lets other sharers in
SELECT sift._test_part_lock_try(7, 5, true) AS exclusive, sift._test_part_lock_try(7, 5, true) AS second_exclusive, sift._test_part_lock_try(7, 5, false) AS shared_over_exclusive;
SELECT sift._test_part_unlock(7, 5);
SELECT sift._test_part_lock_try(7, 5, false) AS shared, sift._test_part_lock_try(7, 5, false) AS second_shared, sift._test_part_lock_try(7, 5, true) AS exclusive_over_shared;
SELECT sift._test_part_unlock(7, 5), sift._test_part_unlock(7, 5);
SELECT sift._test_part_lock_try(7, 5, true) AS free_again;
SELECT sift._test_part_unlock(7, 5);

-- two partitions, the lower stripe first: both held, whatever the order asked; on one stripe it is taken once
SELECT sift._test_part_lock_pair(7, 5, 6);
SELECT sift._test_part_lock_try(7, 5, true) AS first_free_while_held, sift._test_part_lock_try(7, 6, true) AS second_free_while_held;
SELECT sift._test_part_unlock_pair(7, 6, 5);
SELECT sift._test_part_lock_try(7, 5, true) AS first_free, sift._test_part_lock_try(7, 6, true) AS second_free;
SELECT sift._test_part_unlock(7, 5), sift._test_part_unlock(7, 6);
SELECT min(p) AS same_stripe FROM generate_series(1, 4000) AS p WHERE sift._test_part_lock_stripe(7, p) = sift._test_part_lock_stripe(7, 0) \gset
SELECT sift._test_part_lock_pair(7, 0, :same_stripe);
SELECT sift._test_part_lock_try(7, 0, true) AS free_while_held;
SELECT sift._test_part_unlock_pair(7, 0, :same_stripe);
SELECT sift._test_part_lock_try(7, 0, true) AS free_after_one_release;
SELECT sift._test_part_unlock(7, 0);

-- an index of format major 2 in use: its commits publish the version of the manifest, the first backend that plans it leaves the snapshot, and a backend whose cache is empty takes the
-- partition directory and the part lists out of the snapshot (no page of them is read) and plans with at most a few buffers
SET sift.test_create_format1 = off;
CREATE TABLE live (id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO live SELECT i, i % 10 FROM generate_series(1, 20000) i;
SET client_min_messages = warning;
CREATE INDEX live_i ON live USING sift_col (id, v);
CREATE FUNCTION plan_buffers(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off;
	EXECUTE 'EXPLAIN (BUFFERS, FORMAT JSON) ' || q INTO j;
	RETURN coalesce((j->0->'Planning'->>'Shared Hit Blocks')::int, 0) + coalesce((j->0->'Planning'->>'Shared Read Blocks')::int, 0);
END $$;
SELECT plan_buffers('SELECT count(*) FROM live WHERE id BETWEEN 100 AND 200') >= 0 AS planner_warm;
SELECT sift._test_f2_plan_cache_drop();
SELECT partitions, parts, total_rows FROM sift._test_f2_plan_snapshot('live_i');
SELECT sift._test_shm_manifest_stat('snaps_live') >= 1 AS snapshot_held;
SELECT sift._test_f2_plan_cache_drop(), sift._test_f2_plan_cache(true) IS NOT NULL AS reset, sift._test_f2_plan_cache_shm(true) IS NOT NULL AS reset_shm;
SELECT plan_buffers('SELECT count(*) FROM live WHERE id BETWEEN 100 AND 200') <= 4 AS plan_at_most_4_buffers;
SELECT sift._test_f2_plan_cache_shm(true) AS "loaded_from_shm,published", (sift._test_f2_plan_cache(true))[5] AS part_lists_read_from_pages;
DROP FUNCTION plan_buffers(text);
DROP TABLE live;
DROP FUNCTION gone(int);

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
