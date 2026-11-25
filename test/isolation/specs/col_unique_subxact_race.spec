# UNIQUE sift_col (format major 2): a subtransaction that rolls back is out of the running transactions before it lets go of the keys it holds in the registry, so a writer of the same key
# finds the entry of a transaction that ended and reuses its slot. The rollback then lets go of its own entries only: the key the second writer holds stays held (the third writer waits for
# it and is refused when it commits), instead of the rollback freeing the second writer's entry and the key going in twice.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_r (id int, v text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_r SELECT g, 'seed' FROM generate_series(1, 10) g;
	CREATE UNIQUE INDEX iso_r_idx ON iso_r USING sift_col (id);
}

teardown
{
	DROP TABLE iso_r;
}

session s0
step s0lock		{ SELECT pg_advisory_lock(777020); }
step s0unlock	{ SELECT pg_advisory_unlock(777020); }

session s1
step s1gate		{ SET sift.test_phase_gate = 777000; }
step s1b		{ BEGIN; }
step s1sp		{ SAVEPOINT a; INSERT INTO iso_r VALUES (77, 's1'); ROLLBACK TO a; }
step s1c		{ COMMIT; }

session s2
step s2i		{ BEGIN; INSERT INTO iso_r VALUES (77, 's2'); }
step s2c		{ COMMIT; }

session s3
step s3i		{ INSERT INTO iso_r VALUES (77, 's3'); }
step s3q		{ SELECT id, v FROM iso_r WHERE id = 77 ORDER BY v; }

permutation s0lock s1gate s1b s1sp s2i s0unlock s3i s2c s3q s1c
