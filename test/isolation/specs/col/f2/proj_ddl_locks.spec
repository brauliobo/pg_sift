# The projections of a sift_col index against the maintenance and the commit. sift.create_projection locks the table as VACUUM does, so a VACUUM of the table waits for the create to commit
# instead of dropping the projection whose definition it cannot see yet; the projection ends ready. sift.drop_projection follows its transaction to the end: a drop whose commit fails on
# a serialization failure leaves the projection whole and ready, and a create of the name the transaction dropped is refused.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_pl (id bigint, tenant int, amount bigint);
	CREATE INDEX iso_pl_idx ON iso_pl USING sift_col (id, tenant, amount) WITH (key_order = 'id');
	INSERT INTO iso_pl SELECT g, g % 4, g % 100 FROM generate_series(1, 4000) g;
	CREATE TABLE iso_pl_k (k int PRIMARY KEY, v int);
	INSERT INTO iso_pl_k VALUES (1, 0), (2, 0);
}
setup		{ VACUUM iso_pl; }

teardown
{
	DROP TABLE iso_pl, iso_pl_k;
}

session c
step cb		{ BEGIN; }
step ccreate	{ SELECT sift.create_projection('iso_pl_idx', 'p1', 'ORDER BY tenant, amount, id'); }
step cdrop	{ SELECT sift.drop_projection('iso_pl_idx', 'p1'); }
step crecreate	{ SELECT sift.create_projection('iso_pl_idx', 'p1', 'ORDER BY tenant, amount, id'); }
step cend	{ COMMIT; }
step cstate	{ SELECT name, state FROM sift.projection_list() WHERE relation = 'iso_pl_idx'::regclass ORDER BY name; }

session v
step vac	{ VACUUM iso_pl; }

session d
step db		{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT v FROM iso_pl_k WHERE k = 1; }
step dw		{ UPDATE iso_pl_k SET v = v + 1 WHERE k = 2; }
step ddrop	{ SELECT sift.drop_projection('iso_pl_idx', 'p1'); }
step dend	{ COMMIT; }

session s
step sb		{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT v FROM iso_pl_k WHERE k = 2; }
step sw		{ UPDATE iso_pl_k SET v = v + 1 WHERE k = 1; }
step send	{ COMMIT; }

permutation cb ccreate vac cend cstate
permutation ccreate db sb dw sw ddrop send dend cstate
permutation ccreate cb cdrop crecreate cend cstate
