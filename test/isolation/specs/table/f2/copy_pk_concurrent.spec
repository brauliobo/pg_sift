# A COPY into a sift_table with a PRIMARY KEY hands its rows to the bulk route 1000 at a time, and they wait in the stage of the backend with their index entries already in the btree
# until the COPY ends: an INSERT of one of its keys by another session in between waits for the COPY and fails with a duplicate key when it commits, or goes on when it rolls back, as
# on a heap. The check of row 1001 holds the COPY there, once the first 1000 rows are in the btree.
# requires: build col

setup
{
	DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
	CREATE FUNCTION iso_copy_pk_hold(id int) RETURNS bool LANGUAGE plpgsql AS $$
	BEGIN
		IF id = 1001 THEN PERFORM pg_advisory_xact_lock_shared(114, 3); END IF;
		RETURN true;
	END $$;
	CREATE TABLE iso_copy_pk (id int PRIMARY KEY CHECK (iso_copy_pk_hold(id)), v text) USING sift_table WITH (autovacuum_enabled = off);
}

teardown
{
	DROP TABLE iso_copy_pk;
	DROP FUNCTION iso_copy_pk_hold(int);
}

session h
step h_hold { SELECT pg_advisory_lock(114, 3); }
step h_release { SELECT pg_advisory_unlock(114, 3); }

session s1
setup { SET max_parallel_workers_per_gather = 0; }
step s1_begin { BEGIN; }
step s1_copy { COPY iso_copy_pk FROM PROGRAM 'seq 1 1001 | sed ''s/$/\tcopy/'''; }
step s1_commit { COMMIT; }
step s1_abort { ROLLBACK; }

session s2
setup { SET max_parallel_workers_per_gather = 0; }
step s2_insert { INSERT INTO iso_copy_pk VALUES (1, 'insert'); }

session s3
step s3_read { SELECT count(*) AS rows, count(*) FILTER (WHERE id = 1) AS key_1, string_agg(v, ',') FILTER (WHERE id = 1) AS v_1 FROM iso_copy_pk; }

# the insert waits for the COPY, then fails: the COPY committed
permutation h_hold s1_begin s1_copy s2_insert(s1_commit) h_release s1_commit s3_read

# the COPY rolled back: the insert goes on
permutation h_hold s1_begin s1_copy s2_insert(s1_abort) h_release s1_abort s3_read
