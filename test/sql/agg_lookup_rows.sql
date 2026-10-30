-- The rows of a joined table are kept by the ids of their key values and of their column values (one copy of each distinct value). Whatever the
-- types, the repeated keys, the NULLs and the number of distinct values (the tables that find them grow), the rows of a join must be the same
-- loaded for the query, taken from the cache and read by the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, k text, kv varchar(20), n int, n8 bigint, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i,
	CASE WHEN i % 41 = 0 THEN NULL WHEN i % 43 = 0 THEN '' ELSE 'k' || (i % 700) END,
	CASE WHEN i % 37 = 0 THEN NULL ELSE 'v' || (i % 1500) || CASE WHEN i % 5 = 0 THEN repeat('é', 3) ELSE '' END END,
	CASE WHEN i % 31 = 0 THEN NULL ELSE i % 90 END,
	CASE WHEN i % 29 = 0 THEN NULL ELSE 4000000000 + i % 120 END,
	(i % 1000) / 10.0
FROM generate_series(1, 30000) i;
CREATE INDEX ev_col ON ev USING sift_col (k, kv, n, n8, amount);
VACUUM (FREEZE, ANALYZE) ev;

-- every column type a join reads: by value (int, bigint, float8, bool, date), by reference (text, numeric, bytea, a fixed length uuid), NULLs among them
CREATE TABLE dims (k text, kv varchar(20), n int, n8 bigint, label text, num numeric, f float8, b bool, dd date, bytes bytea, u uuid) WITH (autovacuum_enabled = false);
INSERT INTO dims
SELECT CASE WHEN i % 53 = 0 THEN NULL WHEN i % 59 = 0 THEN '' ELSE 'k' || (i % 700) END,
	CASE WHEN i % 47 = 0 THEN NULL ELSE 'v' || (i % 1500) || CASE WHEN i % 5 = 0 THEN repeat('é', 3) ELSE '' END END,
	CASE WHEN i % 61 = 0 THEN NULL ELSE i % 90 END,
	CASE WHEN i % 67 = 0 THEN NULL ELSE 4000000000 + i % 120 END,
	CASE WHEN i % 7 = 0 THEN NULL ELSE 'label ' || (i % 3000) END,
	CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 500) / 8.0 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (i % 211) / 4.0 END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE i % 3 = 0 END,
	CASE WHEN i % 19 = 0 THEN NULL ELSE date '2024-01-01' + i % 400 END,
	CASE WHEN i % 23 = 0 THEN NULL ELSE decode(lpad(to_hex(i % 900), 4, '0'), 'hex') END,
	CASE WHEN i % 25 = 0 THEN NULL ELSE md5((i % 400)::text)::uuid END
FROM generate_series(1, 9000) i;
VACUUM (FREEZE, ANALYZE) dims;

CREATE FUNCTION nodes(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_nodes_cached_tables(q) $$;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_uncached bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.agg_lookup_cache_kb = 0;
	EXECUTE format('CREATE TEMP TABLE r_gen ON COMMIT DROP AS %s', q);
	RESET sift.agg_lookup_cache_kb;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_uncached := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_gen) UNION ALL (TABLE r_gen EXCEPT TABLE r_on)) d);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_gen, r_off;
	RETURN NEXT;
END $$;

-- one text key, repeated keys, NULL and empty keys on both sides, every column type
\set left_join_wide_leaves 'SELECT coalesce(d.label, ''-'') AS label, coalesce(d.num::text, ''-'') AS num, d.b, d.dd, count(*) AS n, sum(ev.amount) AS total, sum(d.f) AS f FROM ev LEFT JOIN dims d ON d.k = ev.k GROUP BY 1, 2, 3, 4'
\set left_join_bytes_uuid 'SELECT coalesce(encode(d.bytes, ''hex''), ''-'') AS bytes, coalesce(d.u::text, ''-'') AS u, count(*) AS n, sum(d.f) AS f FROM ev LEFT JOIN dims d ON d.k = ev.k GROUP BY 1, 2'
SELECT nodes(:'left_join_wide_leaves');
SELECT nodes(:'left_join_wide_leaves');
SELECT * FROM same(:'left_join_wide_leaves');
SELECT nodes(:'left_join_bytes_uuid');
SELECT * FROM same(:'left_join_bytes_uuid');

-- a varchar key with multi-byte values, an inner join
\set inner_join_kv 'SELECT coalesce(d.label, ''-'') AS label, d.b, count(*) AS n, sum(d.f) AS f FROM ev JOIN dims d ON d.kv = ev.kv GROUP BY 1, 2'
SELECT nodes(:'inner_join_kv');
SELECT nodes(:'inner_join_kv');
SELECT * FROM same(:'inner_join_kv');

-- an integer and a bigint key together with a text one
\set left_join_three_keys 'SELECT coalesce(d.label, ''-'') AS label, d.dd, count(*) AS n, sum(ev.amount) AS total FROM ev LEFT JOIN dims d ON d.n = ev.n AND d.n8 = ev.n8 AND d.k = ev.k GROUP BY 1, 2'
SELECT nodes(:'left_join_three_keys');
SELECT nodes(:'left_join_three_keys');
SELECT * FROM same(:'left_join_three_keys');

-- one integer key: a semi join and an anti join
\set in_subselect 'SELECT ev.n % 7 AS g, count(*) AS n, sum(ev.amount) AS total FROM ev WHERE ev.n IN (SELECT d.n FROM dims d WHERE d.n8 % 3 = 0) GROUP BY 1'
\set not_exists 'SELECT ev.n % 7 AS g, count(*) AS n FROM ev WHERE NOT EXISTS (SELECT 1 FROM dims d WHERE d.n = ev.n AND d.b) GROUP BY 1'
SELECT nodes(:'in_subselect');
SELECT * FROM same(:'in_subselect');
SELECT nodes(:'not_exists');
SELECT * FROM same(:'not_exists');

-- arms that read other columns of a table share its kept rows: one entry holds the columns of them all, and a query that reads one of them takes it
\set group_label 'SELECT d.label AS g, count(*) AS n FROM ev JOIN dims d ON d.n8 = ev.n8 GROUP BY 1'
\set group_num_text 'SELECT d.num::text AS g, count(*) AS n FROM ev JOIN dims d ON d.n8 = ev.n8 GROUP BY 1'
\set group_label_concat 'SELECT d.label || coalesce(d.num::text, ''-'') AS g, count(*) AS n FROM ev JOIN dims d ON d.n8 = ev.n8 GROUP BY 1'
SELECT count(*) AS entries_before FROM pg_backend_memory_contexts WHERE name = 'sift cache lookup table' \gset
SELECT nodes(:'group_label');
SELECT nodes(:'group_num_text');
SELECT nodes(:'group_label_concat');
SELECT nodes(:'group_label');
SELECT nodes(:'group_num_text');
SELECT count(*) - :entries_before AS new_entries FROM pg_backend_memory_contexts WHERE name = 'sift cache lookup table';
SELECT * FROM same(:'group_label');
SELECT * FROM same(:'group_num_text');
SELECT * FROM same(:'group_label_concat');

-- a preserved side with NULL keys: its rows no tuple matched are output rows
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SET enable_hashjoin = off; SET enable_mergejoin = off;
\set full_join_label_key 'SELECT coalesce(d.label, ''-'') AS label, coalesce(ev.k, ''-'') AS k, count(*) AS n FROM ev FULL JOIN dims d ON d.k = ev.k AND d.n = ev.n GROUP BY 1, 2'
SELECT nodes(:'full_join_label_key');
SELECT * FROM same(:'full_join_label_key');
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_indexonlyscan; RESET enable_hashjoin; RESET enable_mergejoin;

DROP FUNCTION nodes(text), same(text);
DROP TABLE ev, dims;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
