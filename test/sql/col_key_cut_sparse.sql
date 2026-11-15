-- key_cut by default: a leaf that follows the order of the table loses its zones to the cut, and that is a cost of the cut weighed by the share of the rows that have a
-- value in the leaf, not a veto: a filled one still ends the cut, one filled in 10% of the rows or fewer does not, one that does not follow the table never did; a leaf whose zones
-- skip some of the units (its values near the order of the table by 20,000 of 60,000) costs the cut what it loses of them, one that skips none (60,000 of 60,000) nothing
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- kz: 101 keys scattered over the heap, and leaves that follow it (a counter) with a value in all, 50%, 10%, 1%, 0.2% or none of the rows, or that do not (random values), and leaves near the order of the table by 2,000, 20,000 and 60,000 values
CREATE TABLE kz (id int PRIMARY KEY, tenant_id int, a1 int, z_all int, z_50 int, z_10 int, z_1 int, z_02 int, z_0 int, z_rand int, z_near int, z_part int, z_far int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kz SELECT i, i % 101, i % 1000, i, CASE WHEN random() < 0.5 THEN i END, CASE WHEN random() < 0.1 THEN i END, CASE WHEN random() < 0.01 THEN i END, CASE WHEN random() < 0.002 THEN i END, NULL,
	(random() * 1000000)::int, i + (random() * 2000)::int, i + (random() * 20000)::int, i + (random() * 60000)::int, repeat('x', 100)
FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) kz;

CREATE FUNCTION kdecide(z text) RETURNS TABLE (decision text, reason text) LANGUAGE plpgsql AS
$$ BEGIN
	EXECUTE format('CREATE INDEX kz_i ON kz USING sift_col (id, tenant_id, a1, %I)', z);
	RETURN QUERY SELECT d.decision, d.reason FROM sift.key_cut_decision('kz_i') d;
	DROP INDEX kz_i;
END $$;

SELECT z AS leaf, k.* FROM unnest(ARRAY['z_all', 'z_50', 'z_10', 'z_1', 'z_02', 'z_0', 'z_rand', 'z_near', 'z_part', 'z_far']) z, LATERAL kdecide(z) k;

-- what the cost stands for: the window of the sparse leaf reads more units under the cut than before it
CREATE FUNCTION kunits(q text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		IF l ~ 'Units:' THEN RETURN regexp_replace(trim(l), ', [0-9]+ from pre-aggregates| \([^)]*\)', '', 'g'); END IF;
	END LOOP;
END $$;
CREATE INDEX kz_off ON kz USING sift_col (id, tenant_id, a1, z_02) WITH (key_cut = 'off');
SELECT 'off' AS layout, kunits('SELECT count(*) FROM kz WHERE z_02 BETWEEN 20000 AND 40000');
DROP INDEX kz_off;
CREATE INDEX kz_cut ON kz USING sift_col (id, tenant_id, a1, z_02) WITH (key_cut = 'tenant_id');
SELECT 'cut' AS layout, kunits('SELECT count(*) FROM kz WHERE z_02 BETWEEN 20000 AND 40000');
DROP INDEX kz_cut;

-- what a build does with the sparse leaf and with the filled one
CREATE INDEX kz_sparse ON kz USING sift_col (id, tenant_id, a1, z_02);
CREATE INDEX kz_filled ON kz USING sift_col (id, tenant_id, a1, z_all);
SELECT 'sparse' AS leaf, sift_helpers.index_key_cut_reloption('kz_sparse') UNION ALL SELECT 'filled', sift_helpers.index_key_cut_reloption('kz_filled');

DROP TABLE kz;
DROP FUNCTION kdecide(text), kunits(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
