-- An IN over a table of unique (member, scope key) pairs that a tenant's ids are read from is planned as the one set of the tenant's ids that the scan tests each row against, at every size
-- of the set, not as a btree probe of the pair table for each of the keys the scan meets (109 to 170 M instructions over 1,000,000 facts and 8,000 keys, against 13 to 75 M as the set):
-- the scan of a join query is priced in the units of the plan that reads its table whole, not those of the one index probe a parameterized path of the table costs.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
CREATE TABLE facts (id bigserial PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts (dim_a_id, created_at, kind, amount) SELECT 1 + (8000 * random() ^ 1.8)::int % 8000, timestamptz '2026-01-01' + random() * interval '365 days', (random() * 9)::int,
	(random() * 1000)::numeric(8,2) FROM generate_series(1, 1000000) g;
-- tenants of 8, 80, 800 and 7,200 of the 8,000 ids, and one of all of them: the pair table is above the 10,000 rows from which a probe of it is considered
CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id)) WITH (autovacuum_enabled = false);
INSERT INTO scope_pairs SELECT id, 1 FROM generate_series(1, 8000) id WHERE id % 1000 = 7;
INSERT INTO scope_pairs SELECT id, 2 FROM generate_series(1, 8000) id WHERE id % 100 = 7;
INSERT INTO scope_pairs SELECT id, 3 FROM generate_series(1, 8000) id WHERE id % 10 = 7;
INSERT INTO scope_pairs SELECT id, 4 FROM generate_series(1, 8000) id WHERE id % 10 <> 7;
INSERT INTO scope_pairs SELECT id, 5 FROM generate_series(1, 8000) id;
CREATE INDEX facts_dim_a ON facts (dim_a_id);
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, amount);
VACUUM (FREEZE, ANALYZE) facts, scope_pairs;

CREATE FUNCTION plan_joins(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'none'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins:' THEN r := trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;
-- the rows of the statement with the scan and without it, as one md5 each
CREATE FUNCTION answers(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (%s) t', sql) INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (%s) t', sql) INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;

CREATE TABLE cases (tenant_id int, ids int);
INSERT INTO cases VALUES (1, 8), (2, 80), (3, 800), (4, 7200), (5, 8000);
CREATE FUNCTION statement(t int) RETURNS text LANGUAGE sql AS
	$$ SELECT format('SELECT kind, count(*), sum(amount) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM scope_pairs WHERE tenant_id = %s) GROUP BY kind', t) $$;
-- the plan of each size, the scan's own and the one with the probe rewrite off, and the answers
SELECT c.ids, plan_joins(statement(c.tenant_id)) AS joins, answers(statement(c.tenant_id)) AS answers FROM cases c ORDER BY c.ids;
SET sift.agg_probe_rewrite = off;
SELECT c.ids, plan_joins(statement(c.tenant_id)) AS joins_without_the_rewrite FROM cases c ORDER BY c.ids;
RESET sift.agg_probe_rewrite;
-- an EXISTS and a join over the same table are the same plans
SELECT plan_joins('SELECT kind, count(*) FROM facts f WHERE EXISTS (SELECT 1 FROM scope_pairs p WHERE p.dim_a_id = f.dim_a_id AND p.tenant_id = 4) GROUP BY kind') AS exists_joins;
SELECT plan_joins('SELECT kind, count(*) FROM facts f JOIN scope_pairs p ON p.dim_a_id = f.dim_a_id WHERE p.tenant_id = 3 GROUP BY kind') AS join_joins;

DROP FUNCTION plan_joins(text), answers(text), statement(int);
DROP TABLE cases, facts, scope_pairs;
DROP EXTENSION pg_sift;
