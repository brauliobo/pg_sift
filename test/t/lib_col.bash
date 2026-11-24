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
