\set ECHO none
-- Shared by the agg_preagg* tests that read the plain counters (\ir test/agg_preagg_units.sql): pa_units(q) is the units of the SiftAgg node, those it
-- answered from the pre-aggregates and the workers planned (NULL: the plan has no SiftAgg, or the query fails).
CREATE FUNCTION pa_units(q text, OUT units int, OUT pre int, OUT workers int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		workers := coalesce(substring(l from 'Workers Planned: (\d+)')::int, workers);
	END LOOP;
EXCEPTION WHEN OTHERS THEN
	units := NULL; pre := NULL; workers := NULL;
END $$;
\set ECHO all
