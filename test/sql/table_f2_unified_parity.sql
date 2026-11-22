-- A sift_table answers aggregates through SiftAggV6, the projections and the hybrid scan from its own store, row for row as its heap twin does: numeric(p <= 18, s) as scaled integers (NaN kept),
-- FILTER, date_bin, IN lists, null tests, rows updated by column deltas and rows deleted. A NaN is read back as it was written; a statement whose quals or columns meet a NaN is refused by the aggregate engine (sift.enable_agg = off lets the heap decide), so the row is deleted before the other checks. Every check prints t. Run: psql -X -v ON_ERROR_STOP=1 -f this file, on a build with sift_table.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
-- a testing build creates Format 1 relations unless told otherwise; a release build has no such setting
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE ut (id int PRIMARY KEY, grp int, tag text, at timestamptz, day date, amount numeric(12,2), qty int, flag boolean)
	USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE uh (LIKE ut INCLUDING ALL) WITH (autovacuum_enabled = false);

INSERT INTO ut
SELECT g, g % 17, 'tag' || g % 5, timestamptz '2030-01-01' + g * interval '7 minutes', date '2030-01-01' + g % 60,
	CASE WHEN g % 211 = 0 THEN NULL ELSE (g % 1000) / 8.0 END, g % 23, g % 3 = 0
FROM generate_series(1, 20000) g;
INSERT INTO ut VALUES (20001, 1, 'tag0', timestamptz '2030-02-01', date '2030-02-01', 'NaN', 1, false);
INSERT INTO uh SELECT * FROM ut;
SELECT sift.table_maintain('ut') IS NOT NULL AS maintained;

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_rows(replace(q, '@T', 'ut'), replace(q, '@T', 'uh')) $$;

CREATE FUNCTION plan_has(q text, node text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'ut') LOOP
		IF line LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SELECT parity('SELECT grp, count(*), sum(qty), min(qty), max(qty) FROM @T GROUP BY grp') AS plain_groups;
SELECT parity('SELECT tag, count(*), sum(amount), avg(amount) FROM @T WHERE amount IS NOT NULL GROUP BY tag') AS numeric_sums;
SELECT parity('SELECT id, amount FROM @T WHERE id = 20001') AS nan_round_trip;
CREATE FUNCTION raises(q text) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN EXECUTE q; RETURN false; EXCEPTION WHEN OTHERS THEN RETURN true; END $$;
SELECT parity('SELECT count(amount), sum(amount), min(amount), max(amount) FROM @T') AS aggregate_over_a_nan;
SELECT parity('SELECT count(*) FROM @T WHERE amount > 1') AS comparison_with_a_nan_row;
SELECT raises('SELECT count(*) FROM ut WHERE amount IS NULL') AS null_test_over_a_nan_is_refused;
SELECT parity('SELECT count(*), count(DISTINCT grp) FROM @T') AS totals_with_a_nan_row;
DELETE FROM ut WHERE id = 20001;
DELETE FROM uh WHERE id = 20001;
SELECT sift.table_maintain('ut') IS NOT NULL AS maintained_without_nan;
SELECT parity('SELECT count(*), count(amount), count(DISTINCT grp) FROM @T') AS totals_with_nulls;
SELECT parity('SELECT day, count(*) FILTER (WHERE flag), sum(qty) FILTER (WHERE qty > 10) FROM @T GROUP BY day') AS filtered;
SELECT parity('SELECT date_bin(interval ''1 day'', at, timestamptz ''2030-01-01''), count(*) FROM @T GROUP BY 1') AS date_bin;
SELECT parity('SELECT grp, count(*) FROM @T WHERE grp IN (1, 4, 9) AND qty >= 5 AND amount IS NOT NULL GROUP BY grp') AS in_list_and_null_test;
SELECT plan_has('SELECT grp, count(*) FROM @T GROUP BY grp', 'SiftAggV6') AS served_by_agg_v6;

UPDATE ut SET qty = qty + 1, amount = amount + 0.25 WHERE id % 7 = 0 AND amount IS NOT NULL;
UPDATE uh SET qty = qty + 1, amount = amount + 0.25 WHERE id % 7 = 0 AND amount IS NOT NULL;
DELETE FROM ut WHERE id % 11 = 0;
DELETE FROM uh WHERE id % 11 = 0;
SELECT parity('SELECT grp, count(*), sum(qty), sum(amount) FROM @T WHERE amount IS NOT NULL GROUP BY grp') AS after_updates_and_deletes;
SELECT parity('SELECT tag, count(*) FROM @T GROUP BY tag') AS after_updates_and_deletes_by_tag;

SELECT sift.table_maintain('ut') IS NOT NULL AS maintained_again;
SELECT parity('SELECT grp, count(*), sum(qty) FROM @T GROUP BY grp') AS after_maintenance;

DROP TABLE ut, uh;
DROP FUNCTION parity(text), same_rows(text, text), plan_has(text, text), raises(text);
