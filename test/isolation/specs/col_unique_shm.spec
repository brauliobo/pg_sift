# UNIQUE sift_col (format major 2) against writers whose rows are still in their stage: the key of a row a running transaction has not flushed is held in shared memory, so the writer of a duplicate waits for
# it (commit: 23505, rollback: the row goes in); the keys of a savepoint that rolled back are free and those of one that was released are held; a transaction that writes more keys than the registry holds sends its stage to the partitions and keeps them. The index has no class written: the default class of the type serves ON CONFLICT.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_s (id int, v text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_s SELECT g, 'seed' FROM generate_series(1, 10) g;
	CREATE UNIQUE INDEX iso_s_idx ON iso_s USING sift_col (id);
}

teardown
{
	DROP TABLE iso_s;
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_s VALUES (100, 's1'); }
step s1rel	{ SAVEPOINT a; INSERT INTO iso_s VALUES (400, 's1'); RELEASE a; }
step s1sp	{ SAVEPOINT a; INSERT INTO iso_s VALUES (300, 's1'); ROLLBACK TO a; }
step s1again	{ DELETE FROM iso_s WHERE id = 100; INSERT INTO iso_s VALUES (100, 's1 again'); }
step s1big	{ INSERT INTO iso_s SELECT g, 'big' FROM generate_series(1000000, 1140000) g; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2i	{ INSERT INTO iso_s VALUES (100, 's2'); }
step s2i3	{ INSERT INTO iso_s VALUES (300, 's2'); }
step s2i4	{ INSERT INTO iso_s VALUES (400, 's2'); }
step s2first	{ INSERT INTO iso_s VALUES (1000000, 's2'); }
step s2last	{ INSERT INTO iso_s VALUES (1140000, 's2'); }
step s2nothing	{ INSERT INTO iso_s VALUES (100, 's2') ON CONFLICT (id) DO NOTHING; }
step s2upsert	{ INSERT INTO iso_s VALUES (100, 's2') ON CONFLICT (id) DO UPDATE SET v = 'upserted'; }

session s3
step s3q	{ SELECT id, v FROM iso_s WHERE id IN (100, 300, 400, 1000000, 1140000) ORDER BY id, v; }
step s3n	{ SELECT count(*) AS rows, count(DISTINCT id) AS ids FROM iso_s; }

# the first writer has not flushed its row: the second waits for it, and is refused when it commits
permutation s1b s1i s2i s1c s3q
# ... and takes the key when it rolls back
permutation s1b s1i s2i s1r s3q
# ON CONFLICT waits too: DO NOTHING does nothing and DO UPDATE takes the row once it commits
permutation s1b s1i s2nothing s1c s3q
permutation s1b s1i s2upsert s1c s3q
permutation s1b s1i s2upsert s1r s3q
# a savepoint that rolled back holds no key, one that was released holds it until the transaction ends
permutation s1b s1sp s2i3 s1c s3q
permutation s1b s1rel s2i4 s1c s3q
permutation s1b s1rel s2i4 s1r s3q
# a row deleted and written again by the same transaction holds its key
permutation s1b s1i s1again s2i s1c s3q
# a transaction that writes more keys than the registry holds: its first and its last key are held
permutation s1b s1big s2first s1c s3n
permutation s1b s1big s2first s1r s3n
permutation s1b s1big s2last s1c s3n
permutation s1b s1big s2last s1r s3n
