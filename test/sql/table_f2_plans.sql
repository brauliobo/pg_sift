-- The plans a Format 2.0 sift_table gets: the aggregate engine of Format 2.0 (SiftAggV6) reads the table itself, with the quals it decides on the stored values, the answer of a projection where a
-- ready one stands for parts, and the same rows as the heap twin. The plans are those of EXPLAIN (COSTS off); a node that is not offered shows as the standard plan.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE pt (id int PRIMARY KEY, grp int, kind text, at timestamptz, qty int, amount numeric(12,2)) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO pt SELECT g, g % 17, 'k' || g % 4, timestamptz '2030-01-01' + g * interval '1 minute', g % 23, (g % 500) / 4.0 FROM generate_series(1, 50000) g;
SELECT sift.table_maintain('pt') IS NOT NULL AS maintained;
ANALYZE pt;

CREATE FUNCTION plan(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		CONTINUE WHEN line ~ '^\s*(Exec|I/O & Mem):';
		RETURN NEXT regexp_replace(line, '^\s+', '');
	END LOOP;
END $$;

SELECT plan('SELECT grp, count(*), sum(qty) FROM pt GROUP BY grp') AS group_by_key;
SELECT plan('SELECT grp, sum(amount) FROM pt WHERE qty >= 5 AND grp IN (1, 2, 3) GROUP BY grp') AS filtered_numeric_sum;
SELECT plan('SELECT date_bin(interval ''1 hour'', at, timestamptz ''2030-01-01''), count(*) FROM pt GROUP BY 1') AS date_bin;
SET sift.agg_force = on;
SELECT plan('SELECT count(*) FROM pt WHERE id < 100') AS key_range_count;
SELECT plan('SELECT kind, count(*) FROM pt GROUP BY kind') AS text_key_is_served_by_agg_v6;
DROP FUNCTION plan(text);
DROP TABLE pt;
DROP EXTENSION pg_sift;
