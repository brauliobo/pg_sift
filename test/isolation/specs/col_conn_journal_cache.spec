# A backend that wrote the parent table before any child was connected knows the journal as none (the lineage 0 it read is kept with the relcache entry of the parent index); another
# session then connects the first child, which makes the journal and invalidates that entry. The writer's next key changes and deletes must reach the journal: the rows of the
# found-flag plan are those of the normal plan, the journal names the keys they changed since the stamp of the child (the 5 re-keyed and the 2 deleted), and a child refilled after the writes sees them too.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE FUNCTION iso_jc_same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
	DECLARE
		a text;
		b text;
	BEGIN
		SET LOCAL sift.enable_agg = on;
		EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
		SET LOCAL sift.enable_agg = on;
		RETURN a = b;
	END $$;
	CREATE TABLE iso_jc_dim (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_jc_dim SELECT i, 'l' || i FROM generate_series(1, 300) i;
	CREATE INDEX iso_jc_dim_c ON iso_jc_dim USING sift_col (id, label);
	CREATE TABLE iso_jc_ev (id int PRIMARY KEY, dim_id int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_jc_ev SELECT i, 1 + i % 300, i % 10 FROM generate_series(1, 2000) i;
}

teardown
{
	DROP TABLE iso_jc_ev, iso_jc_dim CASCADE;
	DROP FUNCTION iso_jc_same(text);
}

session s1
step s1set	{ SET sift.agg_force = on; SET max_parallel_workers_per_gather = 0; }
step s1pre	{ UPDATE iso_jc_dim SET id = id + 1000 WHERE id = 300; DELETE FROM iso_jc_dim WHERE id = 299; }
step s1write	{ UPDATE iso_jc_dim SET id = id + 2000 WHERE id <= 5; DELETE FROM iso_jc_dim WHERE id IN (50, 51); INSERT INTO iso_jc_dim VALUES (400, 'new'); }

session s2
step s2set	{ SET sift.agg_force = on; SET max_parallel_workers_per_gather = 0; }
step s2connect	{ CREATE INDEX iso_jc_ev_c ON iso_jc_ev USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> iso_jc_dim(id)'); }
step s2fill	{ SELECT sift.col_maintain('iso_jc_ev_c') IS NOT NULL AS maintained; }
step s2join	{ SELECT iso_jc_same('SELECT count(*), sum(e.v) FROM iso_jc_ev e JOIN iso_jc_dim d ON d.id = e.dim_id') AS joined, iso_jc_same('SELECT count(*) FROM iso_jc_ev e WHERE NOT EXISTS (SELECT 1 FROM iso_jc_dim d WHERE d.id = e.dim_id)') AS anti; }
step s2changed	{ SELECT count(c.key) AS changed_keys FROM sift._test_col_conn_stamps('iso_jc_ev_c') s, sift._test_conn_changed('iso_jc_dim_c', s.lineage, s.horizon) c; }

# the writer's first writes see no journal; the connect makes it; the writes after it are in it, before and after the child is filled again
permutation s1set s2set s1pre s2connect s2fill s1write s2changed s2join s2fill s2join
permutation s1set s2set s1pre s2connect s1write s2fill s2join
