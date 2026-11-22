# Two sessions ON CONFLICT on one key of a sift_table: the second finds the first's row through the index before the row is in any part, waits for it, and then takes the DO UPDATE or DO NOTHING
# path as it does on a heap. Rows are staged in the inserter's memory until it commits or a probe asks for them, so the check of the unique index must see a staged insert.
# requires: build col

setup
{
	DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
	CREATE TABLE iso_oc (id int PRIMARY KEY, v int, hits int NOT NULL DEFAULT 0) USING sift_table WITH (autovacuum_enabled = off);
}

teardown
{
	DROP TABLE iso_oc;
}

session s1
setup { SET max_parallel_workers_per_gather = 0; }
step s1_begin { BEGIN; }
step s1_upsert { INSERT INTO iso_oc VALUES (1, 10) ON CONFLICT (id) DO UPDATE SET hits = iso_oc.hits + 1, v = EXCLUDED.v; }
step s1_nothing { INSERT INTO iso_oc VALUES (2, 20) ON CONFLICT (id) DO NOTHING; }
step s1_commit { COMMIT; }
step s1_abort { ROLLBACK; }

session s2
setup { SET max_parallel_workers_per_gather = 0; }
step s2_begin { BEGIN; }
step s2_upsert { INSERT INTO iso_oc VALUES (1, 11) ON CONFLICT (id) DO UPDATE SET hits = iso_oc.hits + 1, v = EXCLUDED.v; }
step s2_nothing { INSERT INTO iso_oc VALUES (2, 21) ON CONFLICT (id) DO NOTHING; }
step s2_commit { COMMIT; }

session s3
step s3_read { SELECT id, v, hits FROM iso_oc ORDER BY id; }

# the second upsert waits for the first, then updates the row the first wrote
permutation s1_begin s2_begin s1_upsert s2_upsert(s1_commit) s1_commit s2_commit s3_read

# the first aborts: the second inserts
permutation s1_begin s2_begin s1_upsert s2_upsert(s1_abort) s1_abort s2_commit s3_read

# DO NOTHING keeps the first row
permutation s1_begin s2_begin s1_nothing s2_nothing(s1_commit) s1_commit s2_commit s3_read

# a reader never sees the staged row before the commit
permutation s1_begin s1_upsert s3_read s1_commit s3_read
