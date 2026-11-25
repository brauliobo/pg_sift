# The locks of a subtransaction that held a partition (here the relation: a sift_table has one) go with the subtransaction: ROLLBACK TO SAVEPOINT gives back the lock the savepoint
# took and keeps the row locks of the outer transaction, and a lock that the savepoint made stronger is the weaker one again; RELEASE hands the lock to the parent. A prepared
# transaction keeps the lock of its partition in the lock manager, with no memory in the backend that prepared it, and COMMIT PREPARED and ROLLBACK PREPARED end it for the sessions that wait.
# Needs max_prepared_transactions > 0 (scripts/pgtest.sh sets it).
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_sp (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false);
	CREATE TABLE iso_sp_other (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_sp SELECT i, 0 FROM generate_series(1, 60000) i;
	INSERT INTO iso_sp_other SELECT i, 0 FROM generate_series(1, 100) i;
}

setup
{
	SELECT busy FROM sift.table_maintain('iso_sp');
}

teardown
{
	DROP TABLE iso_sp, iso_sp_other;
}

session s1
setup		{ SET deadlock_timeout = '30s'; }
step s1begin	{ BEGIN; }
step s1lock100	{ SELECT count(*) AS n FROM (SELECT id FROM iso_sp WHERE id <= 100 FOR SHARE) q; }
step s1sp	{ SAVEPOINT a; }
step s1big	{ UPDATE iso_sp SET val = val + 1 WHERE id BETWEEN 101 AND 34000; }
step s1rb	{ ROLLBACK TO a; }
step s1rel	{ RELEASE a; }
step s1lockmany	{ SELECT count(*) AS n FROM (SELECT id FROM iso_sp WHERE id <= 33000 FOR SHARE) q; }
step s1all	{ UPDATE iso_sp SET val = val + 1 WHERE id <= 33000; }
step s1c	{ COMMIT; }
step s1pb	{ BEGIN; UPDATE iso_sp SET val = val + 1 WHERE id <= 34000; }
step s1prep	{ PREPARE TRANSACTION 'sift_esc_2pc'; }
step s1other	{ UPDATE iso_sp_other SET val = val + 1 WHERE id = 1; }

session s2
setup		{ SET deadlock_timeout = '30s'; }
step s2uout	{ UPDATE iso_sp SET val = val + 100 WHERE id = 40000; }
step s2u50	{ UPDATE iso_sp SET val = val + 100 WHERE id = 50; }
step s2shout	{ SELECT id FROM iso_sp WHERE id = 40000 FOR SHARE; }
step s2t	{ BEGIN; SET LOCAL lock_timeout = '300ms'; UPDATE iso_sp SET val = val + 100 WHERE id = 50; }
step s2e	{ ROLLBACK; }
step s2v	{ SELECT count(*) AS n, sum(val) AS s FROM iso_sp; }

session s3
step s3cp	{ COMMIT PREPARED 'sift_esc_2pc'; }
step s3rp	{ ROLLBACK PREPARED 'sift_esc_2pc'; }
step s3pending	{ SELECT count(*) AS prepared FROM pg_prepared_xacts; }

# the savepoint took the relation and is rolled back: the relation is free, the row locks of the outer transaction are not
permutation s1begin s1lock100 s1sp s1big s1rb s2uout s2u50(s1c) s1c s2v
# the savepoint made a lock stronger and is rolled back: the lock is the outer one again, which a FOR SHARE does not conflict with and an UPDATE does
permutation s1begin s1lockmany s1sp s1all s1rb s2shout s2uout(s1c) s1c s2v
# RELEASE: the parent holds the relation
permutation s1begin s1sp s1big s1rel s2uout(s1c) s1c s2v
# a prepared transaction holds the relation, with no backend: the isolation tester cannot see a wait on it, so the session asks with a lock timeout, which is refused while the transaction is
# there; its backend goes on with other work; COMMIT PREPARED, or ROLLBACK PREPARED, ends it and the session's update applies over what it left
permutation s1pb s1prep s1other s2t s2e s3cp s2u50 s3pending s2v
permutation s1pb s1prep s1other s2t s2e s3rp s2u50 s3pending s2v
