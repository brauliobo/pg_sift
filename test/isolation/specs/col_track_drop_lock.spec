# DROP INDEX CONCURRENTLY of a tracked sift_col index must not stop the table: it waits for the sessions that hold the table before it marks the index dead, never after.
# A reader that begins after that mark (the drop is held on the test gate, just before it removes the index's two triggers) is not waited for, and the queries that begin
# behind it must not queue behind the removal of the triggers, which needs a lock that conflicts with the reader. The triggers go once the reader is done.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_dl (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
	INSERT INTO iso_dl SELECT i, i % 5, i FROM generate_series(1, 500) i;
	CREATE INDEX iso_dl_x ON iso_dl USING sift_col (id, g, v);

	CREATE FUNCTION iso_dl_triggers(OUT triggers int) LANGUAGE sql AS
		$$ SELECT count(*)::int FROM pg_trigger WHERE tgrelid = 'iso_dl'::regclass AND tgname LIKE 'sift_track_%' $$;
	-- the triggers are removed in the transaction of the drop, or by a worker once the readers are gone: wait for the second
	CREATE FUNCTION iso_dl_gone(OUT triggers text) LANGUAGE plpgsql AS $$
	BEGIN
		FOR i IN 1..300 LOOP
			EXIT WHEN iso_dl_triggers() = 0;
			PERFORM pg_sleep(0.1);
		END LOOP;
		triggers := iso_dl_triggers()::text;
	END $$;
}

teardown
{
	DROP TABLE iso_dl;
	DROP FUNCTION iso_dl_triggers(), iso_dl_gone();
}

session gate
step glock	{ SELECT pg_advisory_lock(20983, 9); }
step gunlock	{ SELECT pg_advisory_unlock(20983, 9); }

session drop
setup		{ SET sift.test_drop_gate = on; }
step ddrop	{ DROP INDEX CONCURRENTLY iso_dl_x; }

session reader
step rbegin	{ BEGIN; SELECT count(*) AS rows FROM iso_dl; }
step rend	{ COMMIT; }

session other
step oread	{ SELECT count(*) AS rows FROM iso_dl; }
step owrite	{ UPDATE iso_dl SET v = v + 1 WHERE id = 1; DELETE FROM iso_dl WHERE id = 500; }

session check
step cgone	{ SELECT * FROM iso_dl_gone(); }

permutation glock ddrop rbegin gunlock oread owrite rend cgone
