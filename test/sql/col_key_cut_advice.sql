-- sift.cluster_advice names key_cut (and key_columns) for the key a workload looks rows up by, by the zone rule: it says so only when the key reads many units
-- and no other leaf is ordered now, so the cut widens nothing. Three states of one table: arrival order (id and day follow the heap), clustered by the key,
-- and clustered then refilled holes (UPDATE, VACUUM, INSERT), which widens the min/max zones of the key although pg_stats still says it is nearly sorted.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

-- the heap order of each state: arrival is id order; clustered is tenant order; refilled is tenant order with one row in fifty somewhere else, as the holes
-- of an UPDATE and VACUUM leave them for the INSERTs that follow (the same layout, made by one INSERT .. ORDER BY so that it does not depend on the free space map)
CREATE FUNCTION kt_fill(t text, mode text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id int, tenant_id int, day date, pad text) WITH (autovacuum_enabled = false)', t);
	EXECUTE format($q$INSERT INTO %I SELECT i, tn, date '2020-01-01' + i / 300, repeat('x', 1000)
		FROM (SELECT i, abs(hashint4(i)) %% 300 AS tn, row_number() OVER (ORDER BY abs(hashint4(i)) %% 300, i) AS rn FROM generate_series(0, 59999) i) x
		ORDER BY CASE %L WHEN 'arrival' THEN i WHEN 'clustered' THEN rn WHEN 'refilled' THEN CASE WHEN i %% 50 = 0 THEN abs(hashint4(i + 5)) %% 60000 ELSE rn END END, i$q$, t, mode);
END $$;

SELECT kt_fill('kt_arrival', 'arrival');
SELECT kt_fill('kt_clustered', 'clustered');
SELECT kt_fill('kt_refilled', 'refilled');
-- the default decision is off (format minor 14): the states are those of an index built before it, which the advice is for
SET sift.col_format_minor = 14;
CREATE INDEX kt_arrival_col ON kt_arrival USING sift_col (id, tenant_id, day) WITH (point_columns = 'tenant_id', key_columns = 'off');
CREATE INDEX kt_clustered_col ON kt_clustered USING sift_col (id, tenant_id, day) WITH (point_columns = 'tenant_id', key_columns = 'off');
CREATE INDEX kt_refilled_col ON kt_refilled USING sift_col (id, tenant_id, day) WITH (point_columns = 'tenant_id', key_columns = 'off');
RESET sift.col_format_minor;
SELECT pg_stat_force_next_flush() \gset
VACUUM (FREEZE, ANALYZE) kt_arrival, kt_clustered, kt_refilled;

-- what the zones say of the key
SELECT 'arrival' AS state, path, verdict FROM sift.col_clustering('kt_arrival_col') WHERE path IN ('id', 'tenant_id', 'day')
UNION ALL SELECT 'clustered', path, verdict FROM sift.col_clustering('kt_clustered_col') WHERE path IN ('id', 'tenant_id', 'day')
UNION ALL SELECT 'refilled', path, verdict FROM sift.col_clustering('kt_refilled_col') WHERE path IN ('id', 'tenant_id', 'day') ORDER BY 1, 2;

-- arrival: id and day are ordered now, key_cut would widen them: the competing leaves are the answer, no key_cut hint
SELECT order_by, verdict, split_part(reason, ':', 1) AS why, reason ~ 'key_cut = ' AS names_key_cut FROM sift.cluster_advice('kt_arrival', 10000, 'tenant_id');
-- clustered: already ordered
SELECT order_by, verdict, split_part(reason, ':', 1) AS why, reason ~ 'key_cut = ' AS names_key_cut FROM sift.cluster_advice('kt_clustered', 10000, 'tenant_id');
-- refilled: a rewrite is recommended, and key_cut is named as the way to keep it ordered; key_columns too (300 values, not listed)
SELECT order_by, verdict, round(units_now) > 5 * round(units_after) AS widened,
       reason ~ 'key_cut = ''tenant_id'' cuts the units by tenant_id' AS names_key_cut, reason ~ 'key_columns = ''tenant_id''' AS names_key_columns,
       reason ~ 'no leaf that is ordered now is widened' AS widens_nothing
FROM sift.cluster_advice('kt_refilled', 10000);
-- the key given by hand
SELECT reason ~ 'key_cut = ''tenant_id''' AS names_key_cut FROM sift.cluster_advice('kt_refilled', 10000, 'tenant_id');
-- a key that would widen a leaf that is ordered now: the arrival table, asked for its day
SELECT reason ~ 'key_cut = ' AS names_key_cut FROM sift.cluster_advice('kt_arrival', 10000, 'tenant_id, day');

-- an index that names no key: the hint says how to choose one
SET sift.col_format_minor = 14;
CREATE INDEX kt_plain_col ON kt_refilled USING sift_col (id, tenant_id, day);
RESET sift.col_format_minor;
SELECT index::text, reason ~ 'set key_cut to the column the queries look rows up by' AS says_how, reason ~ 'key_cut = ''' AS names_key FROM sift.cluster_advice('kt_refilled', 10000) ORDER BY index::text;
DROP INDEX kt_plain_col;

-- an index that has the cut already, and the switches
ALTER INDEX kt_refilled_col SET (key_cut = 'tenant_id');
REINDEX INDEX kt_refilled_col;
SELECT reason ~ 'key_cut = ' AS names_key_cut FROM sift.cluster_advice('kt_refilled', 10000);
SET sift.cluster_advice = off;
SELECT verdict, reason FROM sift.cluster_advice('kt_arrival', 10000);
RESET sift.cluster_advice;

DROP TABLE kt_arrival, kt_clustered, kt_refilled;
DROP FUNCTION kt_fill(text, text);
DROP EXTENSION pg_sift;
