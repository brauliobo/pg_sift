# The rows a Custom Scan (SiftAgg) kept of a joined table for the next query (sift.agg_lookup_cache_kb) serve it only while the table is what it was: a write
# that is in flight, committed or rolled back, an update, a delete, and a snapshot older than a committed write all give the rows of the normal plan.
# requires: build col

setup
{
	CREATE TABLE iso_lc (id int PRIMARY KEY, entity_id int, kind text, code text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_lc SELECT i, i % 40, (array['a', 'b', 'c'])[1 + i % 3], 'c' || (i % 40) FROM generate_series(1, 4000) i;
	CREATE INDEX iso_lc_col ON iso_lc USING sift_col (id, entity_id, kind, code);
	CREATE TABLE iso_lc_dim (id int, country int, label text, code text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_lc_dim SELECT i, i % 4, 'e' || i, 'c' || i FROM generate_series(0, 39) i;
}

setup		{ VACUUM (FREEZE, ANALYZE) iso_lc; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_lc_dim; }

setup
{
	-- the tables the node took from an earlier query, and whether its rows are those of the normal plan, in the snapshot of the statement
	CREATE FUNCTION iso_lc_check(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		nodes int := 0;
		kept int := 0;
		differences bigint;
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
			IF l LIKE '%Cached Lookup Tables:%' THEN kept := kept + trim(split_part(l, ':', 2))::int; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE iso_lc_on AS ' || q;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'CREATE TEMP TABLE iso_lc_off AS ' || q;
		PERFORM set_config('sift.enable_agg', 'on', true);
		differences := (SELECT count(*) FROM ((TABLE iso_lc_on EXCEPT TABLE iso_lc_off) UNION ALL (TABLE iso_lc_off EXCEPT TABLE iso_lc_on)) d);
		DROP TABLE iso_lc_on, iso_lc_off;
		RETURN format('%s SiftAgg nodes, %s cached tables, %s differences from the normal plan', nodes, kept, differences);
	END $$;
	-- an integer key, and a text key whose rows keep their label and code as distinct values
	CREATE FUNCTION iso_lc_run() RETURNS text LANGUAGE sql AS $$
		SELECT iso_lc_check('SELECT d.label, f.kind, count(*) AS n FROM iso_lc f JOIN iso_lc_dim d ON d.id = f.entity_id WHERE d.country = 1 GROUP BY d.label, f.kind') $$;
	CREATE FUNCTION iso_lc_runt() RETURNS text LANGUAGE sql AS $$
		SELECT iso_lc_check('SELECT d.label, f.kind, count(*) AS n FROM iso_lc f LEFT JOIN iso_lc_dim d ON d.code = f.code GROUP BY d.label, f.kind') $$;
}

teardown
{
	DROP FUNCTION iso_lc_run(), iso_lc_runt(), iso_lc_check(text);
	DROP TABLE iso_lc, iso_lc_dim;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1run	{ SELECT iso_lc_run(); }
step s1runt	{ SELECT iso_lc_runt(); }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_lc_dim; }
step s1end	{ COMMIT; }

session s2
step s2begin	{ BEGIN; }
step s2upd	{ UPDATE iso_lc_dim SET country = 1 WHERE id = 3; }
step s2updt	{ UPDATE iso_lc_dim SET label = 'renamed', code = 'c4' WHERE id = 7; }
step s2ins	{ INSERT INTO iso_lc_dim VALUES (2, 1, 'late'); }
step s2del	{ DELETE FROM iso_lc_dim WHERE id = 5; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }
step s2vacuum	{ VACUUM iso_lc_dim; }

# the first query loads the table, the second uses it
permutation s1run s1run
# a write in flight, then committed: the rows are loaded again; vacuumed, they are kept again
permutation s1run s1run s2begin s2upd s2ins s1run s2commit s1run s2vacuum s1run s1run
# rolled back
permutation s1run s1run s2begin s2ins s2del s2abort s1run s2vacuum s1run s1run
# a delete, committed
permutation s1run s1run s2del s1run s2vacuum s1run s1run
# a snapshot older than a committed write and its vacuum reads the rows of its own snapshot
permutation s1run s1run s1rr s2upd s2del s2vacuum s1run s1end s1run s1run
# a text key: a changed label and key give the rows of the normal plan, and the rows are kept again once vacuumed
permutation s1runt s1runt s2begin s2updt s1runt s2commit s1runt s2vacuum s1runt s1runt
