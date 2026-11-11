-- An IN over a tenant of 718 of 20,000 ids, with a range on the ids (`dim_a_id <= k`), is read as the set of the tenant's ids once the range holds more than about a hundred of them: the
-- plan that probes the pair table for each key the scan meets resolves each fact row against its key's answer (66 instructions a row, 35 of them in the join resolution, against 7 for a row
-- tested in the set's byte table), which the price of a probe did not count, so the probes were kept up to 5,000 keys where the set takes 88.6 M instructions against 167.3 M over 2,000,000
-- facts (the two take the same up to 100 keys, 82.6 M, and the probes 103.1 and 113.6 M against 87.1 and 87.4 M at 500 and 1,000).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SELECT setseed(0.42);
CREATE TABLE facts (id bigserial PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts (dim_a_id, created_at, kind, amount) SELECT 1 + (20000 * random() ^ 1.8)::int % 20000, timestamptz '2026-01-01' + random() * interval '365 days', (random() * 9)::int,
	(random() * 1000)::numeric(8,2) FROM generate_series(1, 1000000) g;
CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id)) WITH (autovacuum_enabled = false);
INSERT INTO scope_pairs SELECT id, 1 FROM generate_series(1, 20000) id WHERE id % 28 = 3;
INSERT INTO scope_pairs SELECT id, 2 FROM generate_series(1, 18000) id;
INSERT INTO scope_pairs SELECT id, 3 + id % 40 FROM generate_series(1, 20000) id;
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
CREATE FUNCTION statement(k int) RETURNS text LANGUAGE sql AS
	$$ SELECT format('SELECT kind, count(*), sum(amount) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM scope_pairs WHERE tenant_id = 1) AND dim_a_id <= %s GROUP BY kind', k) $$;
SELECT k, plan_joins(statement(k)) AS joins, answers(statement(k)) AS answers FROM unnest(ARRAY[300, 1000, 5000, 20000]) k;

DROP FUNCTION plan_joins(text), answers(text), statement(int);
DROP TABLE facts, scope_pairs;
DROP EXTENSION pg_sift;
