# A transaction that locks one row in each of 32 sift_tables holds its intentions as records of its own process, in every row lock mode: no page lock of a relation or a partition,
# as the heap's fast-path relation locks (S119-28: past 16 tables the intentions were page locks, read by sift.locks as locks of the relation). A second session locks and updates
# the other row of every table, and is refused the row that is held where the heap refuses it. A transaction that held a row prepared before the sessions began: the first table a new
# backend locks keeps its intention a record too.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	DO $$ BEGIN FOR j IN 1..32 LOOP
		EXECUTE format('CREATE TABLE iso_mr_%s (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false)', j);
		EXECUTE format('INSERT INTO iso_mr_%s VALUES (1, 0), (2, 0)', j);
	END LOOP; END $$;
	CREATE FUNCTION iso_mr_lock(clause text) RETURNS int LANGUAGE plpgsql AS $$
	DECLARE n int := 0; got int;
	BEGIN
		FOR j IN 1..32 LOOP
			EXECUTE format('SELECT id FROM iso_mr_%s WHERE id = 1 %s', j, clause) INTO got;
			n := n + got;
		END LOOP;
		RETURN n;
	END $$;
	CREATE FUNCTION iso_mr_others() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE refused int := 0; got int;
	BEGIN
		FOR j IN 1..32 LOOP
			EXECUTE format('SELECT id FROM iso_mr_%s WHERE id = 2 FOR UPDATE NOWAIT', j) INTO got;
			EXECUTE format('UPDATE iso_mr_%s SET val = val + 1 WHERE id = 2', j);
			BEGIN
				EXECUTE format('SELECT id FROM iso_mr_%s WHERE id = 1 FOR UPDATE NOWAIT', j) INTO got;
			EXCEPTION WHEN lock_not_available THEN refused := refused + 1;
			END;
		END LOOP;
		RETURN 'others locked and updated: 32, held rows refused: ' || refused;
	END $$;
	CREATE FUNCTION iso_mr_page_locks(who int) RETURNS bigint LANGUAGE sql AS $$
		SELECT count(*) FROM sift.locks WHERE pid = who AND kind IN ('row lock', 'row key') $$;
}

setup
{
	BEGIN; SELECT id FROM iso_mr_32 WHERE id = 2 FOR UPDATE; PREPARE TRANSACTION 'iso_mr';
}

setup
{
	COMMIT PREPARED 'iso_mr';
}

teardown
{
	DO $$ BEGIN FOR j IN 1..32 LOOP EXECUTE format('DROP TABLE iso_mr_%s', j); END LOOP; END $$;
	DROP FUNCTION iso_mr_lock(text), iso_mr_others(), iso_mr_page_locks(int);
}

session s1
step s1ks	{ BEGIN; SELECT iso_mr_lock('FOR KEY SHARE') AS locked; }
step s1sh	{ BEGIN; SELECT iso_mr_lock('FOR SHARE') AS locked; }
step s1nk	{ BEGIN; SELECT iso_mr_lock('FOR NO KEY UPDATE') AS locked; }
step s1up	{ BEGIN; SELECT iso_mr_lock('FOR UPDATE') AS locked; }
step s1pl	{ SELECT iso_mr_page_locks(pg_backend_pid()) AS page_locks; }
step s1c	{ COMMIT; }

session s2
step s2o	{ SELECT iso_mr_others(); }
step s2v	{ SELECT sum(val) AS updated FROM (SELECT val FROM iso_mr_1 WHERE id = 2 UNION ALL SELECT val FROM iso_mr_32 WHERE id = 2) v; }

permutation s1ks s1pl s2o s1c s2v
permutation s1sh s1pl s2o s1c s2v
permutation s1nk s1pl s2o s1c s2v
permutation s1up s1pl s2o s1c s2v
