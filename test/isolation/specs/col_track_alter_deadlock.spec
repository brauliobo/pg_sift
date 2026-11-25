# ALTER INDEX ... SET (track_changes) of a sift_col index of format major 2 holds the index and needs the table (to drop or make the row triggers). A statement that holds the table and then
# asks for the index (a second ALTER INDEX) is waiting for the first, so the first must not wait for the table: it is refused at once (55P03, "run the statement again"), not left to a
# deadlock that the server breaks after deadlock_timeout.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_d (id int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_d SELECT g, g FROM generate_series(1, 10) g;
	CREATE INDEX iso_d_idx ON iso_d USING sift_col (id);
}

teardown
{
	DROP TABLE iso_d;
}

session s1
setup		{ SET deadlock_timeout = '100ms'; }
step s1b	{ BEGIN; }
step s1off	{ ALTER INDEX iso_d_idx SET (track_changes = 'off'); }
step s1r	{ ROLLBACK; }

session s2
setup		{ SET deadlock_timeout = '100ms'; }
step s2b	{ BEGIN; SELECT count(*) AS n FROM iso_d; }
step s2on	{ ALTER INDEX iso_d_idx RESET (track_changes); }
step s2c	{ COMMIT; }

permutation s2b s1b s1off s2on s1r s2c
