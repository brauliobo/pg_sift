# sift.table_bulk_groups: what other sessions and the transaction itself see of a COPY that writes column groups. Other sessions see none of its
# rows while it runs (a scan, an index scan, a bitmap scan, SiftAgg and VACUUM included), also while its rows wait in the backend with their
# index entries already in the btree (the COPY is held back at the end by a session that has the maintenance lock, the directory being
# nearly full), all of them once it commits at the snapshots taken after, and none if it rolls back; the transaction sees its rows in the
# next statement, and a statement that reads them writes new rows next to them.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_bulk (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_bulk_id ON iso_bulk (id);
	SET sift.table_bulk_groups = on;
	-- one group per load: the directory is nearly full for a limit of 16 entries
	DO $$
	BEGIN
		FOR i IN 0 .. 13 LOOP
			EXECUTE format('COPY iso_bulk FROM PROGRAM $p$seq %s %s | sed ''s/.*/&\trow &/''$p$', i * 100 + 1, i * 100 + 100);
		END LOOP;
	END $$;
}

teardown
{
	DROP TABLE iso_bulk;
}

session h
step hb		{ BEGIN; SELECT sift._test_tbl_hold_maint('iso_bulk'); }
step hc		{ COMMIT; }

session w
step wb		{ BEGIN; SET sift.table_bulk_groups = on; SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step wcopy	{ COPY iso_bulk FROM PROGRAM 'seq 100001 103000 | sed ''s/.*/&\trow &/'''; }
step wown	{ SELECT count(*), sum(id) FROM iso_bulk; }
step wself	{ INSERT INTO iso_bulk SELECT id + 200000, v FROM iso_bulk WHERE id > 100000 AND id <= 100010; }
step wc		{ COMMIT; }
step wa		{ ROLLBACK; }

session r
step rscan	{ SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*), sum(id) FROM iso_bulk; }
step ridx	{ SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off; SELECT count(*), sum(id) FROM iso_bulk WHERE id > 0; }
step rbm	{ SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on; SELECT count(*) FROM iso_bulk WHERE id > 100; }
step ragg	{ SET sift.enable_agg = on; SET cpu_tuple_cost = 0.5; SELECT id % 3 AS g, count(*) FROM iso_bulk GROUP BY 1 ORDER BY 1; }
step rrr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_bulk; }
step rend	{ COMMIT; }
step rv		{ VACUUM iso_bulk; }

session o
step oseg	{ SELECT count(*) AS entries, count(*) FILTER (WHERE unfrozen) AS carry_their_writer FROM sift.table_segments('iso_bulk') WHERE kind = 'columns'; }

# the COPY waits at its end for the maintenance lock, its rows in the backend and their index entries in the btree: nobody sees them
permutation hb wb wcopy oseg rscan ridx rbm ragg hc wown wc rscan ridx rbm ragg
# a rollback leaves nothing, and VACUUM drops the groups
permutation wb wcopy wown rscan wa rscan ridx rv rscan
# a snapshot taken before the commit never sees the rows; one taken after sees them all
permutation rrr wb wcopy wc rscan rend rscan
# the transaction reads its own rows, and a statement that reads them writes next to them
permutation wb wcopy wown wself wown wc rscan ridx
# VACUUM while the writer runs leaves its groups alone
permutation wb wcopy rv rscan ragg wown wc rscan ragg
