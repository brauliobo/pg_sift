-- A scan of one table that groups starts at about 0.3 ms (the source, its first units), which was priced at 0.7: a tenant of 6,700 rows of 100,000 by month took the
-- normal plan, 1.8 ms (an index-only scan and a hash aggregate), where the scan takes 0.85. The scan is now priced from 0.14, so the tenants whose rows the scan reads
-- faster than the index does (6,700 rows: 1.8 against 0.85 ms; 15,800: 4.0 against 0.83) are the scan's, and a tenant of 1,300 rows (0.43 against 0.59 ms) stays the normal plan's.
-- The prices (scripts/plan-margin.sh): tenant 1 scan 248.8 against normal 545.4, 2 196.0 against 236.2 (17%), 10 169.3 against 94.3, 40 156.6 against 55.2; the tenants between
-- 3 and 4 are a tie of the prices (1% and 14%) and are not asserted.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, event_date date NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + floor(power((hashint4(g) & 16777215) / 16777216.0, 2) * 40)::int, date '2024-01-01' + (hashint4(g + 1) & 16777215) % 730, (ARRAY['a', 'b', 'c', 'd'])[1 + (hashint4(g + 2) & 3)]
FROM generate_series(1, 100000) g;
CREATE INDEX events_tenant ON events (tenant_id, event_date);
CREATE INDEX events_col ON events USING sift_col (tenant_id, event_date, kind);
VACUUM (FREEZE, ANALYZE) events;

CREATE FUNCTION month_plan(tenant int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text;
	q text = format($q$SELECT date_trunc('month', event_date) AS m, count(*) FROM events WHERE tenant_id = %s GROUP BY 1$q$, tenant);
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'SiftAgg' THEN RETURN 'scan'; END IF;
	END LOOP;
	RETURN 'normal';
END $$;

SELECT t AS tenant, (SELECT count(*) FROM events WHERE tenant_id = t) AS rows, month_plan(t) AS plan FROM unnest(ARRAY[1, 2, 10, 40]) t ORDER BY t;

DROP FUNCTION month_plan(int);
DROP TABLE events;
DROP EXTENSION pg_sift;
