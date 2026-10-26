-- The keys a CTE of a literal list hands the scan are no more than the constants of its list, for the column the list is of. A CTE that
-- returns a second column (the partner of the listed row) joined to the parents of the facts on that column has keys of its own: the list says
-- nothing of them, so the scan of an index cut by the key is priced for the keys PostgreSQL expects there, not for the one of the list.
-- The cost of the scan is read from the plan: both statements take it. Every statement is also run with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE lk_facts (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, amount float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO lk_facts SELECT i, CASE WHEN i % 4 = 0 THEN 1 ELSE 2 + (i::bigint * 7919 % 3000)::int END,
	date '2024-01-01' + (i % 700), i % 50 FROM generate_series(1, 400000) i;
CREATE INDEX lk_facts_col ON lk_facts USING sift_col (id, parent_id, ts, amount) WITH (key_cut = parent_id);
CREATE TABLE lk_dims (id int PRIMARY KEY, partner int NOT NULL, grp int NOT NULL);
INSERT INTO lk_dims SELECT p, 1 + (p * 31) % 3000, p % 10 FROM generate_series(1, 3000) p;
VACUUM (FREEZE, ANALYZE) lk_facts, lk_dims;

-- the CTE lists the parent (pid) and returns its partner (qid) beside it; the parents of the facts are joined to one of the two
CREATE FUNCTION lk_q(key_col text) RETURNS text LANGUAGE sql AS $$ SELECT format($f$
WITH ids AS MATERIALIZED (SELECT d.id AS pid, d.partner AS qid FROM lk_dims d WHERE d.id IN (1) AND d.id IN (SELECT id FROM lk_dims WHERE grp >= 0))
SELECT f.ts, count(*) AS n, sum(f.amount) AS total FROM lk_facts f JOIN lk_dims p ON p.id = f.parent_id JOIN ids ON ids.%s = p.id
WHERE f.ts >= date '2024-03-01' GROUP BY 1 ORDER BY 1$f$, key_col) $$;

CREATE FUNCTION lk_cost(key_col text) RETURNS TABLE (plan text, scan_cost float8) LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || lk_q(key_col) INTO j;
	plan := CASE WHEN j::text LIKE '%"SiftAgg"%' THEN 'SiftAgg' ELSE 'normal' END;
	scan_cost := (jsonb_path_query_first(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")') ->> 'Total Cost')::float8;
	RETURN NEXT;
END $$;

CREATE FUNCTION lk_same(key_col text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', lk_q(key_col));
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', lk_q(key_col));
	RESET sift.enable_agg;
	n := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN n;
END $$;

-- the scan of the join on the listed column is priced for the one key of the list, the join on the other column for the keys PostgreSQL expects (at least a third more)
CREATE TEMP TABLE lk_costs AS SELECT k AS key_col, c.* FROM unnest(ARRAY['pid', 'qid']) k, LATERAL lk_cost(k) c;
SELECT key_col, plan, scan_cost < 0.75 * (SELECT scan_cost FROM lk_costs WHERE key_col = 'qid') AS cheaper_than_other, lk_same(key_col) AS rows_unlike_normal
FROM lk_costs ORDER BY key_col;

DROP FUNCTION lk_q(text), lk_cost(text), lk_same(text);
DROP TABLE lk_facts, lk_dims;
DROP EXTENSION pg_sift;
