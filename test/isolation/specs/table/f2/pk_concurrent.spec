# Two sessions that insert one PRIMARY KEY value of a sift_table (a plain INSERT, or an UPDATE of the key): the second finds the row of the first through the index before the row is in any
# part, so it waits for the first and fails with a duplicate key when the first commits, or goes on when the first rolls back, as on a heap. A savepoint rolled back lets go of its rows.
# requires: build col

setup
{
	DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
	CREATE TABLE iso_pk (id int PRIMARY KEY, v int) USING sift_table WITH (autovacuum_enabled = off);
	INSERT INTO iso_pk VALUES (1, 1);
}

teardown
{
	DROP TABLE iso_pk;
}

session s1
setup { SET max_parallel_workers_per_gather = 0; }
step s1_begin { BEGIN; }
step s1_insert { INSERT INTO iso_pk VALUES (5000, 1); }
step s1_key_update { UPDATE iso_pk SET id = 5000 WHERE id = 1; }
step s1_savepoint { SAVEPOINT s; }
step s1_back { ROLLBACK TO s; }
step s1_commit { COMMIT; }
step s1_abort { ROLLBACK; }

session s2
setup { SET max_parallel_workers_per_gather = 0; }
step s2_begin { BEGIN; }
step s2_insert { INSERT INTO iso_pk VALUES (5000, 2); }
step s2_commit { COMMIT; }

session s3
step s3_read { SELECT id, v FROM iso_pk ORDER BY id, v; }

# the second insert waits for the first, then fails: the first committed
permutation s1_begin s2_begin s1_insert s2_insert(s1_commit) s1_commit s2_commit s3_read

# the first rolled back: the second goes on
permutation s1_begin s2_begin s1_insert s2_insert(s1_abort) s1_abort s2_commit s3_read

# a savepoint rolled back lets go of its row: the second does not wait
permutation s1_begin s1_savepoint s1_insert s1_back s2_begin s2_insert s2_commit s1_commit s3_read

# a row inserted inside a savepoint is the transaction's: the second waits for it
permutation s1_begin s1_savepoint s1_insert s2_begin s2_insert(s1_commit) s1_commit s2_commit s3_read

# an UPDATE of the key is an insert of the new key
permutation s1_begin s2_begin s1_key_update s2_insert(s1_commit) s1_commit s2_commit s3_read
