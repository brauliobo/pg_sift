-- The lookups of an aggregate scan. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_lookup_budget
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- The rows of a sub-select joined to the aggregate scan are bounded by memory (half of work_mem: the estimated rows times their width plus 64 bytes a row), not by a
-- row count: a sub-select the planner expects to return 2,000,000 rows (the statistics of the table are set to that) is a lookup table of the scan once work_mem allows it.
-- The rows are the normal plan's in every case.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, n int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 500, i % 100 FROM generate_series(1, 20000) i;
SET client_min_messages = warning;
CREATE INDEX ev_col ON ev USING sift_col (n, amount);
RESET client_min_messages;
CREATE TABLE bigdim (id int, k int) WITH (autovacuum_enabled = false);
INSERT INTO bigdim SELECT i, i % 7 FROM generate_series(0, 999) i;
VACUUM (FREEZE, ANALYZE) ev, bigdim;
-- the planner expects 2,000,000 rows of it: reltuples over the pages it has
UPDATE pg_class SET reltuples = 2000000 WHERE relname = 'bigdim';

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0;
END $$;
SELECT format('SELECT d.k, count(*) AS c, sum(amount) AS s FROM ev JOIN (SELECT id, k FROM bigdim LIMIT %s) d ON d.id = ev.n GROUP BY d.k', 5000000) AS q \gset

SHOW work_mem;
SELECT arm, used, slug FROM sift.explain_why(:'q') WHERE arm = 'single';
SELECT same(:'q') AS default_budget;

-- 2,000,000 rows of 8 bytes and the 64 of a row are 137 MB: half of 128 MB is under them, half of 512 MB over
SET work_mem = '128MB';
SELECT arm, used, slug FROM sift.explain_why(:'q') WHERE arm = 'single';
SELECT same(:'q') AS small_budget;
SET work_mem = '512MB';
SELECT arm, used, slug FROM sift.explain_why(:'q') WHERE arm = 'single';
RESET work_mem;

DROP FUNCTION same(text);
DROP TABLE ev, bigdim;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_lookup_budget

-- >>> agg_lookup_keyed
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- A lookup table that a later inner join keys by one of its columns alone (the ids of a request, a CTE of them, joined to the table of their rows) is read by a
-- btree probe for each key while the probes cost less than its pages, where it was read whole: the rows loaded are the keys', the answers the same, and a request
-- of keys that are most of the table takes the two reads at about the same time.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE ev (id int, dim_id int, v int, d date) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 2000, i % 7, date '2026-01-01' + i % 300 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, v, d);
-- twenty rows to a page: a table of a hundred pages
CREATE TABLE dims (id int PRIMARY KEY, kind int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dims SELECT i, i % 5, repeat('x', 350) FROM generate_series(0, 1999) i;
CREATE TABLE picks (grp int, dim_id int);
INSERT INTO picks SELECT 1, i FROM generate_series(10, 12) i;
INSERT INTO picks SELECT 2, i FROM generate_series(0, 1399) i;
-- a key of twenty-five rows (a thousand pages): the probes read a page for each row they find, so the keys of a request must be fewer for them to pay
CREATE TABLE grps (g int, kind int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO grps SELECT i % 800, i % 5, repeat('x', 350) FROM generate_series(0, 19999) i;
CREATE INDEX grps_g ON grps (g);
CREATE TABLE gpicks (grp int, g int);
INSERT INTO gpicks SELECT 1, i FROM generate_series(10, 12) i;
INSERT INTO gpicks SELECT 2, i FROM generate_series(0, 299) i;
VACUUM (FREEZE, ANALYZE) ev, dims, picks, grps, gpicks;

CREATE FUNCTION request(grp int) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH ids AS MATERIALIZED (SELECT DISTINCT dims.id FROM dims WHERE dims.id IN (SELECT dim_id FROM picks WHERE grp = %s)),
		 d AS (SELECT dims.*, to_jsonb(dims) AS doc FROM ids INNER JOIN dims ON dims.id = ids.id)
		 SELECT d.kind, count(*), sum(ev.v) FROM ev INNER JOIN d ON d.id = ev.dim_id WHERE ev.d >= '2026-02-01' GROUP BY 1 ORDER BY 1$q$, grp)
$$;

CREATE FUNCTION grequest(grp int) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH ids AS MATERIALIZED (SELECT DISTINCT g FROM gpicks WHERE grp = %s),
		 d AS (SELECT grps.*, to_jsonb(grps) AS doc FROM ids INNER JOIN grps ON grps.g = ids.g)
		 SELECT d.kind, count(*), sum(ev.v) FROM ev INNER JOIN d ON d.g = ev.dim_id WHERE ev.d >= '2026-02-01' GROUP BY 1 ORDER BY 1$q$, grp)
$$;

-- the rows loaded and the index scans of the scan, from its EXPLAIN
CREATE FUNCTION loaded(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := ''; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '^ *Lookup: ' THEN r := r || concat_ws('; ', substring(l from '\d+ rows read'), substring(l from '\d+ index scans')) || '; '; END IF;
	END LOOP;
	RETURN r;
END $$;

-- three keys of two thousand: the three rows (and the few rows of the other lookups), one index scan
SELECT loaded(request(1));
-- fourteen hundred keys, 70% of the rows of the table: by the index still (the two reads take the same, 2.6 to 3.9 ms against 2.8 to 4.4 for the table of a hundred pages read whole; the crossover with narrow rows is pinned by agg_lookup_keyed_crossover)
SELECT loaded(request(2));

-- three keys of twenty-five rows: seventy-five rows by probes; three hundred keys (seven thousand five hundred rows, the pages of the table): by the index too, a little faster than read whole (5.3 to 6.3 ms against 5.7 to 7.4)
SELECT loaded(grequest(1));
SELECT loaded(grequest(2));

-- the answers: the same with the table read whole (the setting off), by tuple
CREATE FUNCTION answers(grp int, probe bool, grouped bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || CASE WHEN grouped THEN grequest(grp) ELSE request(grp) END || ') t' INTO r;
	RETURN r;
END $$;
SELECT grouped, answers(g, true, grouped) = answers(g, false, grouped) AS same_answers, answers(g, true, grouped) IS NOT NULL AS not_empty
FROM generate_series(1, 2) g, (VALUES (false), (true)) v(grouped) ORDER BY 1, g;

DROP FUNCTION answers(int, bool, bool), loaded(text), request(int), grequest(int);
DROP TABLE ev, dims, picks, grps, gpicks;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_lookup_keyed

-- >>> agg_lookup_keys_only
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- A semi, anti or inner join by an int4 key (a repeated key of an inner join, a span of keys) is run with the scan (twice, and with workers) and with
-- sift.enable_agg = off: the rows must be the same.
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ev (id int PRIMARY KEY, k int, kind int2, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 101 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 6000 END, (i % 12)::int2, (i % 1000) / 10.0 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (k, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;

-- a membership table (a group, a soft delete, a repeated key), a dimension of one row a key, and one whose keys span more than the byte table holds
CREATE TABLE member (grp int, k int, gone boolean, label text);
INSERT INTO member SELECT g, 1 + (g * 331 + i * 13) % 6000, i % 9 = 0, 'l' || i % 7 FROM generate_series(1, 4) g, generate_series(1, 1500) i;
INSERT INTO member VALUES (2, 77, false, 'dup'), (2, 77, false, 'dup2'), (2, NULL, false, 'none');
CREATE TABLE dim (k int PRIMARY KEY, country int, active boolean);
INSERT INTO dim SELECT i, i % 40, i % 5 <> 0 FROM generate_series(1, 6000) i;
CREATE TABLE wide (k int);
INSERT INTO wide SELECT generate_series(1, 3000) UNION ALL SELECT 2000000000;
ANALYZE member; ANALYZE dim; ANALYZE wide;

CREATE FUNCTION check_rows(q text) RETURNS TABLE (pushed bool, first_run bigint, cached bigint, with_workers bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET LOCAL sift.agg_force = on;
	SET LOCAL sift.backend_cache_kb = 0;
	SET LOCAL sift.shared_cache_kb = 0;
	pushed := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP pushed := pushed OR l LIKE '%Custom Scan (SiftAgg%'; END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_scan ON COMMIT DROP AS %s', q);
	RESET sift.backend_cache_kb;
	RESET sift.shared_cache_kb;
	EXECUTE format('CREATE TEMP TABLE r_kept ON COMMIT DROP AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_kept2 ON COMMIT DROP AS %s', q);
	SET LOCAL parallel_setup_cost = 0;
	SET LOCAL parallel_tuple_cost = 0;
	SET LOCAL min_parallel_table_scan_size = 0;
	SET LOCAL max_parallel_workers_per_gather = 2;
	EXECUTE format('CREATE TEMP TABLE r_par ON COMMIT DROP AS %s', q);
	RESET max_parallel_workers_per_gather;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	first_run := (SELECT count(*) FROM ((TABLE r_scan EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_scan)) d);
	cached := (SELECT count(*) FROM ((TABLE r_kept2 EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_kept2) UNION ALL (TABLE r_kept EXCEPT TABLE r_off)) d);
	with_workers := (SELECT count(*) FROM ((TABLE r_par EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_par)) d);
	DROP TABLE r_scan, r_kept, r_kept2, r_par, r_off;
	RETURN NEXT;
END $$;

-- semi and anti joins with the table's own conditions
SELECT * FROM check_rows($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE k IN (SELECT k FROM member WHERE grp = 2 AND NOT gone) GROUP BY kind$$);
SELECT * FROM check_rows($$SELECT kind, count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM member m WHERE m.k = ev.k AND m.grp = 3) GROUP BY kind$$);
SELECT * FROM check_rows($$SELECT kind, count(*) AS n FROM ev WHERE k IN (SELECT k FROM dim WHERE country < 5 AND active) GROUP BY kind$$);
-- an inner join by a key the table holds once, and by one it repeats (the dictionary and the chains are made and the rows of a key joined)
SELECT * FROM check_rows($$SELECT d.country, count(*) AS n, sum(amount) AS s FROM ev JOIN dim d ON d.k = ev.k WHERE d.active GROUP BY d.country$$);
SELECT * FROM check_rows($$SELECT m.label, count(*) AS n FROM ev JOIN member m ON m.k = ev.k AND m.grp = 2 GROUP BY m.label$$);
SELECT * FROM check_rows($$SELECT kind, count(*) AS n FROM ev JOIN member m ON m.k = ev.k AND m.grp = 2 GROUP BY kind$$);
-- keys past the byte table: the table is looked up
SELECT * FROM check_rows($$SELECT kind, count(*) AS n FROM ev WHERE k IN (SELECT k FROM wide) GROUP BY kind$$);
SELECT * FROM check_rows($$SELECT kind, count(*) AS n FROM ev WHERE k NOT IN (SELECT k FROM wide) GROUP BY kind$$);
-- two tables of keys in one statement
SELECT * FROM check_rows($$SELECT kind, count(*) AS n FROM ev WHERE k IN (SELECT k FROM member WHERE grp = 1) AND k IN (SELECT k FROM dim WHERE active) GROUP BY kind$$);

DROP FUNCTION check_rows(text);
DROP TABLE ev, member, dim, wide;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_lookup_keys_only

-- >>> agg_lookup_lanes
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- A float sum is added in the order of the scan, so a parallel scan has lanes (superblocks l, l + n, ...), and the groups of each lane are sent on before the next
-- lane starts: the output rows of the lookup start again with them, and so must the ids of the lookup's values that an expression over its columns (the FILTER
-- below) was given. The rows are the normal plan's, with a worker and with the leader alone.
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;

CREATE TABLE ev (id int PRIMARY KEY, ent_id int8, val float8) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE abs(hashint4(i)) % 5000 END, (hashint4(i + 5) % 1000) / 7.0 FROM generate_series(1, 120000) i;
CREATE INDEX ev_col ON ev USING sift_col (ent_id, val);
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE ents (id int8, tag text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, CASE WHEN i % 5 = 0 THEN NULL ELSE 't' || i % 3 END FROM generate_series(0, 1999) i;
VACUUM (FREEZE, ANALYZE) ents;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_rows_and_differences(q) $$;

\set q 'SELECT d.tag, count(*) AS n, round((avg(e.val) FILTER (WHERE d.tag IS NOT NULL))::numeric, 6) AS a, count(DISTINCT d.tag) AS t FROM ev e LEFT JOIN ents d ON d.id = e.ent_id GROUP BY 1'
SET max_parallel_workers_per_gather = 1; SET parallel_leader_participation = on;
SELECT * FROM same(:'q');
SET max_parallel_workers_per_gather = 2; SET parallel_leader_participation = off;
SELECT * FROM same(:'q');
SET max_parallel_workers_per_gather = 0;
SELECT * FROM same(:'q');

DROP FUNCTION same(text);
DROP TABLE ev, ents;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_lookup_lanes

-- >>> agg_lookup_cache
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- A joined table no journal vouches for is read by every statement: the Dimension Cache of SiftAggV6 keeps only a journaled parent (docs/ARCHITECTURE.md).
-- Every statement is run with the cache, with sift.backend_cache_kb = 0 and with sift.enable_agg = off: the rows must be the same, whatever happened to the
-- table in between.
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, foreign_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + i % 3000 END, 1 + i % 40, date '2024-01-01' + i % 700, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 120000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, foreign_id, day, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE ents (id int, country int, tier int, label text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, i % 30, i % 4, 'e' || i FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) ents;

-- the dimensions the node of an EXPLAIN ANALYZE took from the Dimension Cache
CREATE FUNCTION cached(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_nodes_cached_tables(q) $$;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_uncached bigint, vs_normal_plan bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_lookup_cache_differences(q) $$;

\set tier_label_lt10 'SELECT e.tier, e.label, count(*) AS n, sum(amount)::bigint AS total FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 10 GROUP BY 1, 2'
\set tier_lt5 'SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 5 GROUP BY 1'

-- a statement and the same statement again; another condition
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_lt5');
SELECT cached(:'tier_lt5');
SELECT cached(:'tier_label_lt10');

-- a write to the table, then vacuumed
INSERT INTO ents VALUES (7, 7, 3, 'new');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');

-- an update and a delete, each vacuumed
UPDATE ents SET tier = 9, label = 'moved' WHERE id = 8;
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
DELETE FROM ents WHERE id < 100;
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');

-- a write that is rolled back moves the pages too, and the rows are what they were
BEGIN;
UPDATE ents SET label = 'rolled back' WHERE id = 500;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
ROLLBACK;
SELECT * FROM same(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');

-- a transaction that sees its own uncommitted rows never takes the rows of another snapshot
BEGIN;
INSERT INTO ents VALUES (11, 5, 2, 'mine');
SELECT * FROM same(:'tier_label_lt10');
ROLLBACK;
SELECT * FROM same(:'tier_label_lt10');

-- a rewritten table, then vacuumed
VACUUM FULL ents;
SELECT cached(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');

-- the cache off
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
RESET sift.backend_cache_kb;
RESET sift.shared_cache_kb;

-- a preserved side is written to by the scan (the rows no tuple matched): it is never kept, and every run sees its own
\set full_join_label 'SELECT coalesce(e.label, ''none'') AS nm, count(*) AS n, sum(ev.amount) AS total FROM ev FULL JOIN ents e ON e.id = ev.dim_a_id GROUP BY 1'
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT cached(:'full_join_label');
SELECT cached(:'full_join_label');
SELECT * FROM same(:'full_join_label');
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_indexonlyscan; RESET enable_hashjoin; RESET enable_mergejoin;
-- a table that does not fit the budget is loaded for its query alone
SET sift.backend_cache_kb = 8;
SET sift.shared_cache_kb = 0;
\set tier_label_lt11 'SELECT e.tier, e.label, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 11 GROUP BY 1, 2'
SELECT cached(:'tier_label_lt11');
SELECT cached(:'tier_label_lt11');
SELECT * FROM same(:'tier_label_lt11');
RESET sift.backend_cache_kb;
RESET sift.shared_cache_kb;
-- ... and is kept once the budget is larger (the refusal is of that budget): the memory of the rows decides, not the pages of the table
SELECT cached(:'tier_label_lt11');
SELECT cached(:'tier_label_lt11');
SELECT * FROM same(:'tier_label_lt11');
-- a condition that is not constant (a stable function) is another query's table every time: not kept
SELECT cached('SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < extract(day from current_date)::int - extract(day from current_date)::int + 10 GROUP BY 1');
SELECT cached('SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < extract(day from current_date)::int - extract(day from current_date)::int + 10 GROUP BY 1');

DROP FUNCTION same(text);
DROP FUNCTION cached(text);
DROP TABLE ev, ents;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_lookup_cache

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
