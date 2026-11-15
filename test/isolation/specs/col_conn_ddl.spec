# A connected index (docs/COL-CONNECT.md) read by a REPEATABLE READ session while another session writes the parent, refreshes the child, or runs REINDEX CONCURRENTLY, DROP INDEX
# CONCURRENTLY and CREATE INDEX CONCURRENTLY on the child or the parent: the rows of the found-flag plan are those of the normal plan in the snapshot of the reader in every step (same), the
# DDL that has to wait for the reader waits, and an index that something is connected to cannot be dropped from under it.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE FUNCTION iso_found_run(q text, OUT same boolean, OUT proofs int, OUT keys int, OUT why text)
	LANGUAGE plpgsql AS $$
	DECLARE
		p jsonb;
		a text;
		b text;
	BEGIN
		SET LOCAL sift.enable_agg = on;
		EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
		EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
		SET LOCAL sift.enable_agg = on;
		same := a = b;
		proofs := coalesce(jsonb_path_query_first(p, '$.**."Found Proofs"')::text::int, 0);
		keys := coalesce(jsonb_path_query_first(p, '$.**."Found Keys Outside The Proof"')::text::int, 0);
		why := trim(both '"' from jsonb_path_query_first(p, '$.**."Found Fallback"')::text);
	END $$;
	CREATE TABLE iso_d_dim (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_d_dim SELECT i, 'l' || i FROM generate_series(1, 200) i;
	CREATE INDEX iso_d_dim_c ON iso_d_dim USING sift_col (id, label);
	CREATE TABLE iso_d_ev (id int PRIMARY KEY, dim_id int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_d_ev SELECT i, CASE WHEN i % 50 = 0 THEN NULL WHEN i % 41 = 0 THEN 999 ELSE 1 + i % 200 END, i % 10 FROM generate_series(1, 2000) i;
	CREATE INDEX iso_d_ev_c ON iso_d_ev USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> iso_d_dim(id)');
	CREATE TABLE iso_d_ev2 (id int PRIMARY KEY, dim_id int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_d_ev2 SELECT i, 1 + i % 200, i % 10 FROM generate_series(1, 1000) i;
	CREATE TABLE iso_d_tp (id int PRIMARY KEY, label text) USING sift_table;
	CREATE TABLE iso_d_te (id int PRIMARY KEY, tp_id int) WITH (autovacuum_enabled = off);
}

teardown
{
	DROP TABLE iso_d_ev, iso_d_ev2, iso_d_te, iso_d_tp, iso_d_dim CASCADE;
	DROP FUNCTION iso_found_run(text);
}

session s1
step s1set	{ SET sift.agg_force = on; SET max_parallel_workers_per_gather = 0; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1join	{ SELECT * FROM iso_found_run('SELECT count(*) FROM iso_d_ev e JOIN iso_d_dim d ON d.id = e.dim_id'); }
step s1anti	{ SELECT * FROM iso_found_run('SELECT count(*) FROM iso_d_ev e WHERE NOT EXISTS (SELECT 1 FROM iso_d_dim d WHERE d.id = e.dim_id)'); }
step s1commit	{ COMMIT; }

session s2
step s2set	{ SET sift.agg_force = on; SET max_parallel_workers_per_gather = 0; }
step s2fill	{ SELECT sift.col_maintain('iso_d_ev_c') IS NOT NULL AS maintained; }
step s2write	{ INSERT INTO iso_d_dim VALUES (900, 'new'); DELETE FROM iso_d_dim WHERE id = 7; UPDATE iso_d_dim SET label = 'changed' WHERE id <= 5; DELETE FROM iso_d_ev WHERE id % 13 = 0; }
step s2reind_child	{ REINDEX INDEX CONCURRENTLY iso_d_ev_c; }
step s2reind_parent	{ REINDEX INDEX CONCURRENTLY iso_d_dim_c; }
step s2cic	{ CREATE INDEX CONCURRENTLY iso_d_ev2_c ON iso_d_ev2 USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> iso_d_dim(id)'); }
step s2dropc	{ DROP INDEX CONCURRENTLY iso_d_ev_c; }
step s2dropp	{ DROP INDEX iso_d_dim_c; }
step s2droppc	{ DROP INDEX CONCURRENTLY iso_d_dim_c; }
step s2vac	{ VACUUM iso_d_dim; }
step s2tbl	{ CREATE INDEX iso_d_te_c ON iso_d_te USING sift_col (id, tp_id) WITH (connect = 'tp_id -> iso_d_tp(id)'); }
step s2join	{ SELECT * FROM iso_found_run('SELECT count(*) FROM iso_d_ev e JOIN iso_d_dim d ON d.id = e.dim_id'); }
step s2anti	{ SELECT * FROM iso_found_run('SELECT count(*) FROM iso_d_ev e WHERE NOT EXISTS (SELECT 1 FROM iso_d_dim d WHERE d.id = e.dim_id)'); }
step s2state	{ SELECT child, fill FROM sift.col_connections ORDER BY child::text; }

# the reader's snapshot is older than the parent writes and than the refresh of the child
permutation s1set s2set s2fill s1rr s1join s1anti s2write s2fill s1join s1anti s1commit s2join s2anti s2state
# REINDEX CONCURRENTLY, DROP INDEX CONCURRENTLY and CREATE INDEX CONCURRENTLY of the child wait for the reader, which keeps its plan; after it all is the normal plan's rows
permutation s1set s2set s2fill s1rr s1join s2reind_child s1join s1anti s1commit s2join s2anti s2state
permutation s1set s2set s2fill s1rr s1join s2dropc s1join s1commit s2join s2anti s2state
permutation s1set s2set s2fill s1rr s1join s2cic s1join s1commit s2join s2anti s2state
# REINDEX CONCURRENTLY of the parent index after parent writes, under the reader: nothing is vouched until the VACUUM completes the log of the new copy, then the child is read from its flags again
permutation s1set s2set s2fill s1rr s1join s2write s2reind_parent s1join s1anti s1commit s2join s2anti s2state s2vac s2fill s2join s2anti s2state
# an index that has a connected child cannot be dropped, concurrently or not; a parent that is not a heap table cannot be connected to
permutation s1set s2set s2fill s2dropp s2droppc s2tbl s2join s2anti s2state
