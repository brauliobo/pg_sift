-- The joined table of a lookup join of Custom Scan (SiftAgg) is read through a btree for its own `column < value`, `<=`, `>` and `>=` conditions (both bounds of a range) as for
-- `column = value` (sift.agg_lookup_pushdown): the bounds follow the equal columns of the index, a bound that keeps a small share of the table is an index scan, a bound that keeps a
-- large share, a table without an index and a condition that is not a bound are read by the heap scan, with the same rows. Every statement is run with the pushdown, without it and with
-- sift.enable_agg = off.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SET sift.agg_probe_rewrite = off;
SET sift.enable_col_scan = off;

CREATE TABLE ev (id int PRIMARY KEY, dim_id int, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 30000 END, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0 FROM generate_series(1, 300000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_id, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;

-- the id is the key of the join; country and tier are the equal columns of an index that a bound follows, the name is text under a collation, the stamp a date
CREATE TABLE dims (id int PRIMARY KEY, country int, tier int2, name text COLLATE "C", day date);
INSERT INTO dims SELECT i, i % 100, (i % 5)::int2, 'n' || lpad(i::text, 5, '0'), date '2024-01-01' + i % 900 FROM generate_series(1, 30000) i;
INSERT INTO dims VALUES (30001, NULL, NULL, NULL, NULL);
CREATE INDEX dims_country_tier ON dims (country, tier);
CREATE INDEX dims_name ON dims (name);
CREATE INDEX dims_day ON dims (day);
CREATE TABLE dims_bare (id int, country int, tier int2);
INSERT INTO dims_bare SELECT id, country, tier FROM dims;
ANALYZE dims; ANALYZE dims_bare;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_with_lookup_counts(q) $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_no_pushdown bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.agg_lookup_pushdown = off;
	EXECUTE format('CREATE TEMP TABLE r_no ON COMMIT DROP AS %s', q);
	RESET sift.agg_lookup_pushdown;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_no_pushdown := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_no) UNION ALL (TABLE r_no EXCEPT TABLE r_on)) d);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_no, r_off;
	RETURN NEXT;
END $$;

-- each bound of the key, both bounds, the bound of a semi join, an EXISTS, an inner join that returns a column
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 300) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id < 300) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id > 29700) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 29700) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 100 AND id < 400) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.id <= 300) GROUP BY kind');
SELECT * FROM same('SELECT d.country, count(*) AS n, sum(amount) AS s FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.id <= 300 GROUP BY d.country');
-- a bound that keeps no row, a bound that keeps one, the bounds that cross
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 0) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 30000 AND id <= 30000) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id >= 300 AND id < 100) GROUP BY kind');
-- the index (country, tier): a value of the first column and a bound of the second, a bound of the first column, a list and a bound
SELECT * FROM same('SELECT d.country, d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.country = 3 AND d.tier <= 3 GROUP BY 1, 2');
SELECT * FROM same('SELECT d.country, d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.country IN (3, 17) AND d.tier >= 3 GROUP BY 1, 2');
SELECT * FROM same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.country < 2 GROUP BY 1');
SELECT * FROM same('SELECT d.tier, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_id WHERE d.tier < 4 AND d.country = 3 GROUP BY 1');
-- text under a collation of the column, a date, an operand of another type (bigint, a cast)
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE name < ''n00300'')');
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE name >= ''n00200'' AND name <= ''n00400'')');
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE name < ''n00300'' COLLATE "en-x-icu")');
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE day < date ''2024-01-05'')');
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 300::bigint)');
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 300.5::numeric)');
-- a NULL operand matches no row; a NULL of the column never passes a bound
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= NULL::int)');
SELECT * FROM same('SELECT count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE country < 3)');
-- the bound of a LEFT join is a condition of its ON clause: the events keep their rows
SELECT * FROM same('SELECT d.country, count(*) AS n FROM ev LEFT JOIN dims d ON d.id = ev.dim_id AND d.id <= 300 GROUP BY d.country');
-- a bound that keeps most of the table is read by the heap scan, as a table with no index, a condition that is not a bound (<>, a function) and a bound of an expression are
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= 20000) GROUP BY kind');
SELECT * FROM same('SELECT b.country, count(*) AS n FROM ev JOIN dims_bare b ON b.id = ev.dim_id WHERE b.id <= 300 GROUP BY b.country');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <> 300) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id + 0 <= 300) GROUP BY kind');
-- a bound that is a parameter: the generic plan reads it when the scan starts
SET plan_cache_mode = force_generic_plan;
PREPARE up_to(int) AS SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= $1) GROUP BY kind ORDER BY 1;
EXECUTE up_to(300);
EXECUTE up_to(0);
EXECUTE up_to(NULL);
SET sift.enable_agg = off;
DEALLOCATE up_to;
PREPARE up_to(int) AS SELECT kind, count(*) AS n FROM ev WHERE dim_id IN (SELECT id FROM dims WHERE id <= $1) GROUP BY kind ORDER BY 1;
EXECUTE up_to(300);
RESET sift.enable_agg;
RESET plan_cache_mode;
DEALLOCATE ALL;

DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP TABLE ev, dims, dims_bare;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
