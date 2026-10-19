-- median of n server-side runs of a query (ms), settings applied per run
CREATE OR REPLACE FUNCTION bench_ms(q text, n int, agg text, pre text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz; xs float8[] := '{}'; i int; c bigint := 0; rr record;
BEGIN
	PERFORM set_config('sift.enable_agg', agg, false);
	PERFORM set_config('sift.agg_preagg', pre, false);
	PERFORM set_config('max_parallel_workers_per_gather', '0', false);
	PERFORM set_config('seq_page_cost', '10000', false);
	BEGIN
		PERFORM set_config('sift.col_rollup_worker', 'off', false);
		PERFORM set_config('sift.col_rollup_on_demand', 'off', false);
	EXCEPTION WHEN OTHERS THEN NULL;
	END;
	FOR i IN 0..n LOOP
		t0 := clock_timestamp();
		FOR rr IN EXECUTE q LOOP c := c + 1; END LOOP;
		IF i > 0 THEN xs := xs || (extract(epoch FROM clock_timestamp() - t0) * 1000)::float8; END IF;
	END LOOP;
	RETURN (SELECT round((percentile_cont(0.5) WITHIN GROUP (ORDER BY v))::numeric, 2) FROM unnest(xs) v);
END $$;

-- one CREATE INDEX: ms
CREATE OR REPLACE FUNCTION build_ms(ddl text, workers int) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz;
BEGIN
	PERFORM set_config('max_parallel_maintenance_workers', workers::text, false);
	PERFORM set_config('maintenance_work_mem', '1GB', false);
	DROP INDEX IF EXISTS docs_col;
	t0 := clock_timestamp();
	EXECUTE ddl;
	RETURN round((extract(epoch FROM clock_timestamp() - t0) * 1000)::numeric, 0);
END $$;

DROP FUNCTION IF EXISTS median_of(numeric[]);
CREATE FUNCTION median_of(xs numeric[]) RETURNS text LANGUAGE sql AS
$$ SELECT 'min ' || min(v) || ' median ' || (percentile_cont(0.5) WITHIN GROUP (ORDER BY v))::numeric FROM unnest(xs) v $$;

-- on-CPU time (ms) of the backend for one serial CREATE INDEX: robust against a busy host
CREATE OR REPLACE FUNCTION cpu_ns() RETURNS bigint LANGUAGE sql AS
$$ SELECT split_part(pg_read_file('/proc/self/schedstat'), ' ', 1)::bigint $$;
CREATE OR REPLACE FUNCTION build_cpu_ms(ddl text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE c0 bigint;
BEGIN
	PERFORM set_config('max_parallel_maintenance_workers', '0', false);
	PERFORM set_config('maintenance_work_mem', '1GB', false);
	DROP INDEX IF EXISTS docs_col;
	c0 := cpu_ns();
	EXECUTE ddl;
	RETURN round(((cpu_ns() - c0) / 1e6)::numeric, 0);
END $$;
