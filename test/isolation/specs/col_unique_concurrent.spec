# UNIQUE sift_col (format major 2) against concurrent writers: a duplicate of a row a running transaction wrote waits for it (commit: 23505, rollback: the row goes in), a
# delete that has not committed holds its key for the writer of the same key, a writer of another key never waits, and the rows of an aborted transaction or savepoint leave nothing behind.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_u (id int, v text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_u SELECT g, 'seed' FROM generate_series(1, 10) g;
	CREATE UNIQUE INDEX iso_u_idx ON iso_u USING sift_col (id sift.col_int4_ops);
}

teardown
{
	DROP TABLE iso_u;
}

session s1
step s1b	{ BEGIN; }
step s1i1	{ INSERT INTO iso_u VALUES (100, 's1'); }
step s1d2	{ DELETE FROM iso_u WHERE id = 2; }
step s1sp	{ SAVEPOINT a; INSERT INTO iso_u VALUES (300, 's1'); ROLLBACK TO a; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN; }
step s2i1	{ INSERT INTO iso_u VALUES (100, 's2'); }
step s2i3	{ INSERT INTO iso_u VALUES (300, 's2'); }
step s2d2	{ INSERT INTO iso_u VALUES (2, 's2'); }
step s2nothing	{ INSERT INTO iso_u VALUES (100, 's2') ON CONFLICT DO NOTHING; }
step s2upsert	{ INSERT INTO iso_u VALUES (100, 's2') ON CONFLICT (id) DO UPDATE SET v = 'upserted'; }
step s2c	{ COMMIT; }

session s3
step s3q	{ SELECT id, v FROM iso_u WHERE id IN (2, 100, 200, 300) ORDER BY id, v; }

# the first writer commits: the second finds the duplicate once it is out of doubt
permutation s1b s2b s1i1 s2i1 s1c s2c s3q
# the first writer rolls back: the second takes the key
permutation s1b s2b s1i1 s2i1 s1r s2c s3q
# other keys never wait
permutation s1b s2b s1i1 s2i3 s1c s2c s3q
# the rows of a savepoint that was rolled back are no conflict
permutation s1b s2b s1sp s2i3 s1c s2c s3q
# a delete that has not committed: the writer of its key waits, and takes it when it commits
permutation s1b s2b s1d2 s2d2 s1c s2c s3q
# ... and the delete that rolls back keeps the key: the writer is refused
permutation s1b s2b s1d2 s2d2 s1r s2c s3q
# ON CONFLICT waits for the transaction it conflicts with: DO NOTHING does nothing once it commits, DO UPDATE takes the row
permutation s1b s2b s1i1 s2nothing s1c s2c s3q
permutation s1b s2b s1i1 s2upsert s1c s2c s3q
permutation s1b s2b s1i1 s2nothing s1r s2c s3q
