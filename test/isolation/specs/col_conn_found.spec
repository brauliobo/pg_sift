# The join answered from the found flag (docs/COL-CONNECT.md, "Found") while another session writes the parent: its writes that are still open are in doubt, so their keys are outside
# the proof and the parent probe does not see them; after the commit it sees them, after the abort it sees none. The rows are those of the normal plan in every step.
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
	CREATE TABLE iso_f_dim (id int PRIMARY KEY, label text, note int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_f_dim SELECT i, 'l' || i, 0 FROM generate_series(1, 200) i;
	CREATE INDEX iso_f_dim_c ON iso_f_dim USING sift_col (id, label);
	CREATE TABLE iso_f_ev (id int PRIMARY KEY, dim_id int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_f_ev SELECT i, CASE WHEN i % 50 = 0 THEN NULL ELSE 1 + i % 200 END, i % 10 FROM generate_series(1, 2000) i;
	CREATE INDEX iso_f_ev_c ON iso_f_ev USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> iso_f_dim(id)');
}

teardown
{
	DROP TABLE iso_f_ev, iso_f_dim;
	DROP FUNCTION iso_found_run(text);
}

session s1
step s1fill	{ SELECT sift.col_maintain('iso_f_ev_c'); }
step s1b	{ BEGIN; }
step s1upd	{ UPDATE iso_f_dim SET label = 'changed' WHERE id <= 5; }
step s1del	{ DELETE FROM iso_f_dim WHERE id = 7; }
step s1ins	{ INSERT INTO iso_f_dim VALUES (900, 'new', 0); }
step s1commit	{ COMMIT; }
step s1abort	{ ROLLBACK; }

session s2
step s2set	{ SET sift.agg_force = on; SET max_parallel_workers_per_gather = 0; }
step s2join	{ SELECT * FROM iso_found_run('SELECT count(*) FROM iso_f_ev e JOIN iso_f_dim d ON d.id = e.dim_id'); }
step s2anti	{ SELECT * FROM iso_found_run('SELECT count(*) FROM iso_f_ev e WHERE NOT EXISTS (SELECT 1 FROM iso_f_dim d WHERE d.id = e.dim_id)'); }

permutation s1fill s2set s2join s1b s1upd s1del s1ins s2join s2anti s1commit s2join s2anti
permutation s1fill s2set s1b s1upd s1del s1ins s2join s1abort s2join s2anti
