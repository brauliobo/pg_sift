\set ECHO none
-- Shared by the agg_preagg* tests that read the plain counters (\ir test/common/sql/agg/preagg_units.sql): pa_units(q) is the granules of the SiftAggV6 node (scanned,
-- pruned or answered), those it answered from the pre-aggregates and the workers planned (NULL: the plan has no SiftAggV6, or the query fails).
CREATE FUNCTION pa_units(q text, OUT units int, OUT pre int, OUT workers int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'SiftAggV6' AND units IS NULL THEN
			units := 0;
			pre := 0;
		ELSIF l ~ '^\s*Granules: ' THEN
			units := units + (SELECT sum(m[1]::int) FROM regexp_matches(l, '(?:scanned|pruned_\w+|preagg)=(\d+)', 'g') m);
			pre := pre + coalesce(substring(l from ' preagg=(\d+)')::int, 0);
		END IF;
		workers := coalesce(substring(l from 'Workers Planned: (\d+)')::int, workers);
	END LOOP;
EXCEPTION WHEN OTHERS THEN
	units := NULL; pre := NULL; workers := NULL;
END $$;
\set ECHO all
