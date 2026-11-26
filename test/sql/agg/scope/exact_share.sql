-- A read of a scope key (count and sum of the rows of a member's tenant) must not turn to the join for the hours between two VACUUMs: rows inserted since the last VACUUM make
-- the pages of their slice of time not all-visible, and the scope of a time-cut index and of one without a time cut is read as before, with the rows of the normal plan.
-- An index whose format keeps a scope summary (sift.scope_stats) is read through it, fresh rows or not.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT d, 1 + d % 4 FROM generate_series(1, 200) d;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE timed (id int PRIMARY KEY, dim_a_id int, created_at date, amount int) WITH (autovacuum_enabled = false);
CREATE TABLE untimed (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO timed SELECT i, 1 + (i * 7) % 200, date '2025-01-01' + i / 2000, i % 10 FROM generate_series(1, 600000) i;
INSERT INTO untimed SELECT i, 1 + (i * 7) % 200, i % 10 FROM generate_series(1, 600000) i;
CREATE INDEX timed_col ON timed USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
CREATE INDEX untimed_col ON untimed USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
VACUUM (FREEZE, ANALYZE) timed, untimed, tenant_scope;

-- the read of tenant 2: read through the scope where the index keeps a summary of it, and the rows of the normal plan
CREATE FUNCTION scope_read(tbl text, OUT by_summary bool, OUT same_rows bool) LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT count(*), sum(amount) FROM %I WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 2)', tbl);
DECLARE l text; scoped bool := false; a text; b text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP scoped := scoped OR l LIKE '%Scope Read%'; END LOOP;
	by_summary := scoped = EXISTS (SELECT 1 FROM sift.scope_stats(format('%I_col', tbl)::regclass));
	EXECUTE format('SELECT row(c.*)::text FROM (%s) c', q) INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT row(c.*)::text FROM (%s) c', q) INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	same_rows := a = b;
END $$;

-- every page all-visible
SELECT tbl, (scope_read(tbl)).* FROM unnest(ARRAY['timed', 'untimed']) tbl ORDER BY tbl;

-- 1% of the rows arrive and nothing has vacuumed them: the pages of the slice of time they are in are not all-visible
INSERT INTO timed SELECT 1000000 + i, 1 + (i * 7) % 200, date '2026-06-01', 1 FROM generate_series(1, 6000) i;
INSERT INTO untimed SELECT 1000000 + i, 1 + (i * 7) % 200, 1 FROM generate_series(1, 6000) i;
SELECT tbl, (scope_read(tbl)).* FROM unnest(ARRAY['timed', 'untimed']) tbl ORDER BY tbl;

DROP FUNCTION scope_read(text);
DROP TABLE timed, untimed, tenant_scope;
DROP EXTENSION pg_sift;
