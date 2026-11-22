# A reader's snapshot is answered from the projections of a sift_col index while other sessions delete, update, insert and vacuum: the answer is the heap's at the snapshot, whichever part
# the projection stands for when the statement starts, because the projection parts, the base parts and the rest of the relation are read from one pinned version of the manifest.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_p (id bigint, tenant int, amount bigint, note text);
	CREATE INDEX iso_p_idx ON iso_p USING sift_col (id, tenant, amount) WITH (key_order = 'id');
	INSERT INTO iso_p SELECT g, g % 4, g % 100, 'n' FROM generate_series(1, 20000) g;
}
setup		{ VACUUM iso_p; }
setup
{
	SELECT sift.create_projection('iso_p_idx', 'iso_p_agg', 'GROUP BY tenant MEASURES count(*), sum(amount)');
	SELECT sift.create_projection('iso_p_idx', 'iso_p_sort', 'ORDER BY tenant, amount, id');
}
setup		{ VACUUM iso_p; }

teardown
{
	DROP TABLE iso_p;
}

session r
step rb		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT tenant, count(*) AS n, sum(amount) AS s FROM iso_p GROUP BY tenant ORDER BY tenant; }
step ragg	{ SELECT tenant, count(*) AS n, sum(amount) AS s FROM iso_p GROUP BY tenant ORDER BY tenant; }
step rsort	{ SELECT id FROM iso_p WHERE tenant = 2 ORDER BY amount, id LIMIT 5; }
step rend	{ COMMIT; }
step rnow	{ SELECT tenant, count(*) AS n, sum(amount) AS s FROM iso_p GROUP BY tenant ORDER BY tenant; }

session w
step wdel	{ DELETE FROM iso_p WHERE id % 10 = 0; }
step wupd	{ UPDATE iso_p SET amount = amount + 1000 WHERE id % 7 = 0; }
step wins	{ INSERT INTO iso_p SELECT 30000 + g, g % 4, 5, 'new' FROM generate_series(1, 500) g; }
step wbegin	{ BEGIN; DELETE FROM iso_p WHERE tenant = 2 AND id % 3 = 0; }
step wabort	{ ROLLBACK; }

session v
step vac	{ VACUUM iso_p; }

permutation rb wdel ragg rsort rend rnow
permutation rb wupd wins ragg rend rnow
permutation rb wdel vac ragg rsort rend rnow
permutation wbegin rb ragg wabort ragg rend
permutation rb wins vac wupd ragg rsort rend rnow
