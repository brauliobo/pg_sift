# An UPDATE of a column that a CREATE UNIQUE INDEX CONCURRENTLY makes a key waits for a FOR KEY SHARE of the row, whether the session opened the table before the build or while the
# index was not valid yet: the key columns of the store are those of every index the catalog lists, valid or not
# requires: build col
setup
{
	DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
	CREATE TABLE iso_ks (id int PRIMARY KEY, k int) USING sift_table WITH (autovacuum_enabled = off);
	INSERT INTO iso_ks SELECT i, i FROM generate_series(1, 10) i;
}
teardown { DROP TABLE iso_ks; }

session u
setup { SET max_parallel_workers_per_gather = 0; }
step u_open { SELECT count(*) FROM iso_ks; }
step u_upd { UPDATE iso_ks SET k = k + 100 WHERE id = 1; }

session old
step old_begin { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class WHERE relname = 'iso_ks'; }
step old_end { COMMIT; }

session c
step cic { CREATE UNIQUE INDEX CONCURRENTLY iso_ks_k ON iso_ks (k); }

session ks
step ks_lock { BEGIN; SELECT id FROM iso_ks WHERE id = 1 FOR KEY SHARE; }
step ks_end { COMMIT; }

session chk
step chk_valid { SELECT indisvalid FROM pg_index WHERE indexrelid = 'iso_ks_k'::regclass; }

permutation old_begin cic u_open chk_valid old_end chk_valid ks_lock u_upd ks_end
permutation u_open cic ks_lock u_upd ks_end
