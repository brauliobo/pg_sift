# The registry of the keys of UNIQUE sift_col indexes (col/f2/uniq/shm.c): a stripe that is full borrows a spare entry while fewer transactions than its entries ended since its last sweep,
# instead of asking the process array for each of its transactions on every insert once any transaction of the server ends. The stripe holds the entries of the running transaction of s1 and
# one of a transaction that ended: a sweep would give that one back, so an entry taken without one adds to the entries of the registry.
# requires: col

setup
{
	CREATE FUNCTION iso_uqs_active() RETURNS int8 AS 'pg_sift', 'sift_test_uniq_shm_active' LANGUAGE C STRICT;
	CREATE FUNCTION iso_uqs_fill(stripe int, n int, xid int8) RETURNS int AS 'pg_sift', 'sift_test_uniq_shm_fill' LANGUAGE C STRICT;
	CREATE FUNCTION iso_uqs_reserve_one(stripe int) RETURNS bool AS 'pg_sift', 'sift_test_uniq_shm_reserve_one' LANGUAGE C STRICT;
	CREATE FUNCTION iso_uqs_sweep(stripe int) RETURNS void AS 'pg_sift', 'sift_test_uniq_shm_sweep' LANGUAGE C STRICT;
	CREATE FUNCTION iso_uqs_take(stripe int) RETURNS int8 LANGUAGE plpgsql AS $$
	DECLARE
		before int8 := iso_uqs_active();
	BEGIN
		PERFORM iso_uqs_reserve_one(stripe);
		RETURN iso_uqs_active() - before;
	END $$;
	CREATE PROCEDURE iso_uqs_end(n int) LANGUAGE plpgsql AS $$
	BEGIN
		FOR i IN 1 .. n LOOP
			PERFORM txid_current();
			COMMIT;
		END LOOP;
	END $$;
}

teardown
{
	DROP FUNCTION iso_uqs_take(int);
	DROP PROCEDURE iso_uqs_end(int);
	DROP FUNCTION iso_uqs_active();
	DROP FUNCTION iso_uqs_fill(int, int, int8);
	DROP FUNCTION iso_uqs_reserve_one(int);
	DROP FUNCTION iso_uqs_sweep(int);
}

session s1
step s1fill	{ BEGIN; SELECT iso_uqs_fill(11, 511, txid_current()) AS running; SELECT iso_uqs_sweep(11); SELECT iso_uqs_fill(11, 1, 3) AS ended; }
step s1take	{ SELECT iso_uqs_take(11) AS added; }
step s1c	{ COMMIT; }

session s2
step s2end	{ CALL iso_uqs_end(1); }
step s2many	{ CALL iso_uqs_end(512); }

# one transaction ended since the sweep: the stripe borrows a spare entry and keeps the ended one
permutation s1fill s2end s1take s1c
# as many transactions as the stripe has entries ended since: the stripe is swept, and the entry of the ended transaction is the one taken
permutation s1fill s2many s1take s1c
