# A chain of column deltas longer than a quarter of sift.backend_cache_kb is not kept whole in the cache of the chains: the update of a row reads it page by page for the records of the other
# transactions. A transaction that took its snapshot before another one patched a row of that chain is refused when it patches the same row, and one that patched another row is not.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_ll (id int PRIMARY KEY, a int NOT NULL, b text, c text, d text, e text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_ll SELECT i, i, repeat('b', 100), repeat('c', 100), repeat('d', 100), repeat('e', 100) FROM generate_series(1, 6000) i;
}

setup
{
	UPDATE iso_ll SET b = b || 'x';
}

teardown
{
	DROP TABLE iso_ll;
}

session s1
step s1set	{ SET sift.backend_cache_kb = '1MB'; }
step s1upd	{ UPDATE iso_ll SET c = 'one' WHERE id = 5; }

session s2
step s2set	{ SET sift.backend_cache_kb = '1MB'; }
step s2begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS rows FROM iso_ll; }
step s2same	{ UPDATE iso_ll SET d = 'two' WHERE id = 5; }
step s2other	{ UPDATE iso_ll SET d = 'two' WHERE id = 6; }
step s2c	{ COMMIT; }
step s2v	{ SELECT id, c = 'one' AS c_one, d = 'two' AS d_two FROM iso_ll WHERE id IN (5, 6) ORDER BY id; }

permutation s1set s2set s2begin s1upd s2other s2same s2c s2v
