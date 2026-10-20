-- Timing helpers of steps/extras.sh (leaf types, facets and fields, BM25 sizes).

-- median (ms) of n server-side runs of a statement after one untimed run; SETTINGS is 'name=value;name=value', applied for the runs and reset after
CREATE OR REPLACE FUNCTION timed_ms(q text, n int DEFAULT 21, settings text DEFAULT '') RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz; xs float8[] := '{}'; i int; kv text;
BEGIN
	FOREACH kv IN ARRAY string_to_array(settings, ';') LOOP
		IF kv <> '' THEN PERFORM set_config(split_part(kv, '=', 1), split_part(kv, '=', 2), false); END IF;
	END LOOP;
	FOR i IN 0..n LOOP
		t0 := clock_timestamp();
		EXECUTE q;
		IF i > 0 THEN xs := xs || (extract(epoch FROM clock_timestamp() - t0) * 1000)::float8; END IF;
	END LOOP;
	FOREACH kv IN ARRAY string_to_array(settings, ';') LOOP
		IF kv <> '' THEN EXECUTE 'RESET ' || split_part(kv, '=', 1); END IF;
	END LOOP;
	RETURN (SELECT round((percentile_cont(0.5) WITHIN GROUP (ORDER BY v))::numeric, 3) FROM unnest(xs) v);
END $$;

-- one CREATE INDEX: wall ms and the on-CPU ms of this backend (serial, robust against a busy host)
CREATE OR REPLACE FUNCTION build_index(ddl text, OUT wall_ms numeric, OUT cpu_ms numeric) LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz; c0 bigint;
BEGIN
	PERFORM set_config('max_parallel_maintenance_workers', '0', false);
	PERFORM set_config('maintenance_work_mem', '1GB', false);
	t0 := clock_timestamp();
	c0 := split_part(pg_read_file('/proc/self/schedstat'), ' ', 1)::bigint;
	EXECUTE ddl;
	cpu_ms := round(((split_part(pg_read_file('/proc/self/schedstat'), ' ', 1)::bigint - c0) / 1e6)::numeric, 0);
	wall_ms := round((extract(epoch FROM clock_timestamp() - t0) * 1000)::numeric, 0);
END $$;
