-- A top-1 probe of the grouped order array for each row of a LATERAL (the latest row of an entity) restarts a scan whose source stays open: 19,500 instructions and 2 buffer pins,
-- 2.4 to 2.7 us a probe over 1,000, 3,000 and 10,000 entities at 100,000 to 1,000,000 rows. Every probe was priced as the first open of the array (28,000 operators and 7,300 for
-- each segment: 88 to 106 units), forty times what it takes and more than the aggregate that reads the same entities. The cost of a probe is checked at 20,000 rows.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE ev AS SELECT 1 + (i::bigint * 7919 % 200)::int AS ent, now() - (hashint4(i) & 8388607) * interval '1 second' AS ts, i % 1000 AS v FROM generate_series(1, 20000) i;
CREATE TABLE ents AS SELECT g AS id, g % 30 AS grp FROM generate_series(1, 200) g;
CREATE INDEX ev_c ON ev USING sift_col (ent, ts, v) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'ent');
VACUUM (FREEZE, ANALYZE) ev, ents;

-- the cost of one probe (the Limit's startup) and of the aggregate of the same entities, in the plan the planner chooses
CREATE FUNCTION probe_cost() RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE l text; c float8;
BEGIN
	FOR l IN EXPLAIN SELECT d.id, x.ts FROM ents d LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = d.id ORDER BY ts DESC LIMIT 1) x ON true WHERE d.grp = 3 LOOP
		IF l ~ 'Limit' AND c IS NULL THEN c := substring(l FROM 'cost=([0-9.]+)\.\.')::float8; END IF;
	END LOOP;
	RETURN c;
END $$;
SELECT probe_cost() BETWEEN 1.5 AND 5 AS probe_costs_microseconds;

-- the same rows with the probes and with the normal plan
CREATE FUNCTION probe_rows(push boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT md5(string_agg(concat_ws(%L, d.id, x.ts), %L ORDER BY d.id)) FROM ents d LEFT JOIN LATERAL (SELECT ts FROM ev WHERE ent = d.id ORDER BY ts DESC LIMIT 1) x ON true WHERE d.grp = 3', ',', ';') INTO r;
	RETURN r;
END $$;

SELECT probe_rows(true) = probe_rows(false) AS same_rows;

DROP FUNCTION probe_cost(), probe_rows(boolean);
DROP TABLE ev, ents;
DROP EXTENSION pg_sift;
