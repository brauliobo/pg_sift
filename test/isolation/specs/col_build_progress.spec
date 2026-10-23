# CREATE INDEX of a sift_col index reports the heap blocks and the rows it has done in pg_stat_progress_create_index, serial and with
# workers: when the scan is over (the build waits to enter the write phase for the advisory lock sift.test_phase_gate + 3, which the other
# session holds) every block of the table and every row is counted, whichever participant did them.
# requires: build gates

setup
{
	CREATE TABLE iso_cbp (id int, k int, pad text DEFAULT repeat('x', 100)) WITH (autovacuum_enabled = off, parallel_workers = 2);
	INSERT INTO iso_cbp (id, k) SELECT g, g % 100 FROM generate_series(1, 60000) g;
	ANALYZE iso_cbp;
}

teardown
{
	DROP TABLE iso_cbp;
}

session s1
step s1serial	{ SET maintenance_work_mem = '64MB'; SET max_parallel_maintenance_workers = 0; SET sift.test_phase_gate = 1000;
			  CREATE INDEX iso_cbp_idx ON iso_cbp USING sift_col (id, k, pad); }
step s1par	{ SET maintenance_work_mem = '128MB'; SET max_parallel_maintenance_workers = 2; SET min_parallel_table_scan_size = 0; SET sift.test_phase_gate = 1000;
			  CREATE INDEX iso_cbp_idx ON iso_cbp USING sift_col (id, k, pad); }
step s1drop	{ DROP INDEX iso_cbp_idx; }
step s1w	{ SELECT current_setting('sift.test_col_workers')::int > 0 AS workers_launched; }

session s2
step s2h	{ SELECT pg_advisory_lock(1003); }
step s2p	{ SELECT phase, blocks_total > 0 AS has_blocks, blocks_done = blocks_total AS all_blocks, tuples_done = 60000 AS all_rows
			  FROM pg_stat_progress_create_index WHERE relid = 'iso_cbp'::regclass; }
step s2r	{ SELECT pg_advisory_unlock(1003); }

permutation s2h s1serial s2p s2r s1drop s2h s1par s2p s2r s1w
