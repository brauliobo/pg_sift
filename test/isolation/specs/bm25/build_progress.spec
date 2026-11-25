# CREATE INDEX reports its phases and the documents it has done in pg_stat_progress_create_index. The build
# waits to enter each phase for an advisory lock (sift.test_phase_gate + the phase: 2 scan, 3 spill, 4 merge,
# 5 write) that the other session holds and releases one by one.
# requires: build gates

setup
{
	CREATE TABLE iso_prog (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_prog SELECT g, 'common w' || (g % 100) || ' u' || g FROM generate_series(1, 20000) g;
	ANALYZE iso_prog;
}

teardown
{
	DROP TABLE iso_prog;
}

session s1
step s1b	{ SET maintenance_work_mem = '1MB'; SET max_parallel_maintenance_workers = 0; SET sift.test_run_docs = 3000;
			  SET sift.test_phase_gate = 1000; CREATE INDEX iso_prog_idx ON iso_prog USING sift (body); }

session s2
step s2h	{ SELECT pg_advisory_lock(1002), pg_advisory_lock(1003), pg_advisory_lock(1004), pg_advisory_lock(1005); }
step s2p	{ SELECT phase, tuples_done > 0 AS some_done, tuples_done <= tuples_total AS within_total, tuples_total FROM pg_stat_progress_create_index
			  WHERE relid = 'iso_prog'::regclass; }
step s2r2	{ SELECT pg_advisory_unlock(1002); }
step s2r3	{ SELECT pg_advisory_unlock(1003); }
step s2r4	{ SELECT pg_advisory_unlock(1004); }
step s2r5	{ SELECT pg_advisory_unlock(1005); }

permutation s2h s1b s2p s2r2 s2p s2r3 s2p s2r4 s2p s2r5
