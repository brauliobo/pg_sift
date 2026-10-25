# Fixtures of the sift_col crash and replica tests (col_index_*, col_run_heads_*): statements for `sql NODE ...` that compare the aggregate
# scan with the normal plan on the same data.

# jsonb "weight" leaf with mixed JSON types; the checks over table orders (tenant_id, status, qty, gone, doc)
COL_WEIGHT_FUNCS=(
	"CREATE FUNCTION weight_of(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS \$\$ SELECT CASE i % 17 WHEN 0 THEN '\"heavy\"' WHEN 1 THEN 'true' WHEN 2 THEN '{\"kg\": 1}'
		WHEN 3 THEN '[1]' WHEN 4 THEN 'null' ELSE to_jsonb((i % 40) / 4.0::float8) END::jsonb \$\$"
	"CREATE FUNCTION weight_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS \$\$ SELECT CASE WHEN i % 23 = 0 THEN '{}'::jsonb ELSE jsonb_build_object('weight', weight_of(i)) END \$\$"
	"CREATE FUNCTION typeof_same() RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE q text; a text; b text;
		BEGIN
			SET LOCAL seq_page_cost = 10000;
			FOREACH q IN ARRAY ARRAY[
				'SELECT (count(*), sum((doc->>''weight'')::float8))::text FROM orders WHERE jsonb_typeof(doc->''weight'') = ''number''',
				'SELECT count(*)::text FROM orders WHERE jsonb_typeof(doc->''weight'') <> ''number''',
				'SELECT count(*)::text FROM orders WHERE jsonb_typeof(doc->''weight'') = ''null''',
				'SELECT count(*)::text FROM orders WHERE jsonb_typeof(doc->''weight'') IS NULL',
				'SELECT string_agg(coalesce(t, ''-'') || '':'' || n, '','' ORDER BY t NULLS FIRST) FROM (SELECT jsonb_typeof(doc->''weight'') AS t, count(*) AS n FROM orders GROUP BY 1) x',
				'SELECT (count(*) FILTER (WHERE jsonb_typeof(doc->''weight'') = ''string'' OR tenant_id < 5), count(*) FILTER (WHERE jsonb_typeof(doc->''weight'') IS NULL))::text FROM orders']
			LOOP
				SET LOCAL sift.enable_agg = on;
				EXECUTE q INTO a;
				SET LOCAL sift.enable_agg = off;
				EXECUTE q INTO b;
				IF a IS DISTINCT FROM b THEN RETURN false; END IF;
			END LOOP;
			RETURN true;
		END \$\$"
	"CREATE FUNCTION gone_same() RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE q text := \$q\$SELECT coalesce(string_agg(status || ':' || n || ':' || s, ',' ORDER BY status), '') FROM (SELECT status, count(*) AS n, sum(qty) AS s FROM orders WHERE gone IS NULL GROUP BY 1) x\$q\$; a text; b text;
		BEGIN
			SET LOCAL seq_page_cost = 10000;
			SET LOCAL sift.enable_agg = on;
			EXECUTE q INTO a;
			SET LOCAL sift.enable_agg = off;
			EXECUTE q INTO b;
			RETURN a IS NOT DISTINCT FROM b;
		END \$\$"
)

# table t (k, v) with its sift_col index, and the top-N comparison
COL_TOPN_FIXTURE=(
	"CREATE FUNCTION topn_same() RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE col text; dir text; lim int; g text; q text; a text; b text;
		BEGIN
			SET LOCAL max_parallel_workers_per_gather = 0;
			FOREACH col IN ARRAY ARRAY['k', 'v'] LOOP
			FOREACH dir IN ARRAY ARRAY['ASC', 'DESC'] LOOP
			FOREACH lim IN ARRAY ARRAY[1, 32, 100] LOOP
			FOREACH g IN ARRAY ARRAY['on', 'off'] LOOP
				q := format('SELECT coalesce(string_agg(%1\$s::text, '','' ORDER BY pos), '''') FROM (SELECT row_number() OVER () AS pos, %1\$s FROM (SELECT %1\$s FROM t ORDER BY %1\$s %2\$s LIMIT %3\$s) x) y', col, dir, lim);
				PERFORM set_config('sift.test_topn_gorder', g, true);
				SET LOCAL sift.enable_agg = on;
				EXECUTE q INTO a;
				SET LOCAL sift.enable_agg = off;
				EXECUTE q INTO b;
				IF a IS DISTINCT FROM b THEN RETURN false; END IF;
			END LOOP; END LOOP; END LOOP; END LOOP;
			RETURN true;
		END \$\$"
	"CREATE TABLE t (id bigserial PRIMARY KEY, k int NOT NULL, v int NOT NULL, pad text)"
	"ALTER TABLE t ALTER COLUMN pad SET STORAGE PLAIN"
	"INSERT INTO t (k, v, pad) SELECT (hashint4(i) & 2147483647) % 100000, i, repeat('x', 400) FROM generate_series(1, 60000) i"
	"CREATE INDEX t_col ON t USING sift_col (k, v) WITH (write_buffer_limit = 64, order_columns = 'k, v', order_array_columns = 'k, v')"
	"VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) t"
)
