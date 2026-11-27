# SERIALIZABLE and a read of a scope key of a Format 2.0 sift_col index: the summary takes no predicate lock, so SSI reads the join, whose scans lock the heaps. Two transactions that
# each read the sums of a scope key and then insert, the one a pair of it and the other an event, are a write
# skew, and the second one to commit fails with 40001.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_ss_pr (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false);
	INSERT INTO iso_ss_pr SELECT e, 1 + e % 5 FROM generate_series(1, 40) e;
	CREATE TABLE iso_ss (id int PRIMARY KEY, entity_id int NOT NULL, at date NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
	INSERT INTO iso_ss SELECT g, 1 + g % 40, DATE '2025-01-01' + g / 20, g % 97 FROM generate_series(1, 6000) g;
	CREATE INDEX iso_ss_scope ON iso_ss USING sift_col (id, entity_id, at, amount) WITH (scope = 'parent_id: entity_id -> iso_ss_pr(entity_id, parent_id)', track_changes = 'ctid');
	CREATE FUNCTION iso_ss_read() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE l text; r text;
	BEGIN
		FOR l IN EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT date_trunc('month', e.at::timestamp), sum(e.amount) s FROM iso_ss e JOIN iso_ss_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = 2 GROUP BY 1 LOOP
			r := coalesce(r, substring(l from 'Scope Read: ([a-z]+)'));
		END LOOP;
		RETURN r;
	END $$;
}

setup
{
	VACUUM (ANALYZE) iso_ss, iso_ss_pr;
}

teardown
{
	DROP TABLE iso_ss, iso_ss_pr;
	DROP FUNCTION iso_ss_read();
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1plan	{ SELECT iso_ss_read() AS scope_read; }
step s1r	{ SELECT sum(s) FROM (SELECT date_trunc('month', e.at::timestamp), sum(e.amount) s FROM iso_ss e JOIN iso_ss_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = 2 GROUP BY 1) m; }
step s1i	{ INSERT INTO iso_ss_pr VALUES (41, 2); }
step s1c	{ COMMIT; }

session s2
setup		{ SET max_parallel_workers_per_gather = 0; }
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2r	{ SELECT sum(s) FROM (SELECT date_trunc('month', e.at::timestamp), sum(e.amount) s FROM iso_ss e JOIN iso_ss_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = 2 GROUP BY 1) m; }
step s2i	{ INSERT INTO iso_ss VALUES (6002, 1, DATE '2025-02-01', 20); }
step s2c	{ COMMIT; }

permutation s1b s2b s1plan s1r s2r s1i s1c s2i s2c
