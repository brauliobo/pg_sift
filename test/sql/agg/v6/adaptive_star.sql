-- Adaptive execution (src/core/adapt_pipe.c, src/agg/sip.c) and the
-- multi-core paths they stand on (src/agg/par/{morsel,radix}.c): each held to the same work with no limit, to PostgreSQL's own evaluation of the same query, or to
-- the invariant it exists for. The data is made by arithmetic, so that the output does not depend on the version.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers = 8;
SET max_parallel_workers_per_gather = 8;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the order of the conjuncts
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the cheap and selective conjunct leads its group, no conjunct is ever behind one of a higher group of errors, two near ranks keep their places, a drift is followed,
-- a conjunct that was last is run first now and then to be measured again, and a scan whose granules are all empty reads the rows it keeps and not whole chunks
SELECT * FROM sift._test_apipe();

CREATE TABLE ap AS SELECT g AS id, CASE WHEN g % 50 = 0 THEN 1 ELSE 2 + g % 5 END::int8 AS a, CASE WHEN g % 50 <> 0 THEN 0 WHEN g % 100 = 0 THEN 1 + g % 9 ELSE 300 END::int8 AS b FROM generate_series(1, 4000) AS g;
-- b is zero for the rows that a = 1 rejects, and the division passes half of the rows that a = 1 keeps: the measures make it the better rank, and the group of errors keeps it last
SELECT r.matched = (SELECT count(*) FROM ap WHERE a = 1 AND 1000 / b > 5) AS same_as_server, r.reorders >= 0 AS ran, NOT r.division_first AS division_not_first
FROM (SELECT array_agg(a ORDER BY id) AS a, array_agg(b ORDER BY id) AS b FROM ap) AS t, LATERAL sift._test_apipe_filter(a, b, true) AS r;
-- without the group of errors (what the old reorder did), the measures put the division first and a row it cannot divide raises the error
\set VERBOSITY terse
SELECT r.matched FROM (SELECT array_agg(a ORDER BY id) AS a, array_agg(b ORDER BY id) AS b FROM ap) AS t, LATERAL sift._test_apipe_filter(a, b, false) AS r;
\set VERBOSITY default

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the circuit breaker of the join filter
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- 90% of the rows match: the probe stops after the 8 batches of the warmup and looks at one granule in 64 (<= 3% of the rows probed), the zone test is not stopped, no match is lost
SELECT r.rows_probed <= 32768 AS probed_the_warmup, r.rows_probed * 100 / r.rows_total AS probed_pct, r.auto_disables, r.auto_reenables, r.disabled_at_end, r.level2_prunes, r.missed
FROM sift._test_sip_adapt(100, 100, 90, 90) AS r;
-- 5% match: the probe is never stopped
SELECT r.rows_probed = r.rows_total AS all_probed, r.auto_disables, r.disabled_at_end, r.missed FROM sift._test_sip_adapt(100, 100, 5, 5) AS r;
-- the first 16 granules match 95%, the 200 after 2%: stopped, then started again by the granule it looks at, and the rows that miss are rejected from then on
SELECT r.auto_disables, r.auto_reenables >= 1 AS reenabled, r.disabled_at_end, r.reject_after_reenable_pct >= 85 AS rejects_after, r.missed
FROM sift._test_sip_adapt(216, 16, 95, 2) AS r;
-- a stream that stays above the rate keeps the probe stopped after each look
SELECT r.auto_disables, r.auto_reenables, r.disabled_at_end, r.rows_probed * 100 / r.rows_total < 10 AS few_probed FROM sift._test_sip_adapt(300, 300, 80, 80) AS r;

-- the guided queue cut into lanes: the leader and 8 workers (more participants than the 8 lanes) take every granule once, and the ends of the lists are one granule
SELECT w AS workers, p AS parts, r.launched = w AS launched_all, r.total, r.wrong AS granules_not_once, r.bad_claims, r.tail_run
FROM (VALUES (8, 5), (8, 9), (8, 1), (3, 4)) AS v(w, p), LATERAL sift._test_morsel_par(31, w, p, 9000) AS r ORDER BY w, p;
-- 8 workers (the wide queues) drain into the leader in turn: no worker holds the others back, and every group comes back
SELECT w AS workers, m AS mode, r.launched = w AS launched_all, r.bad AS wrong
FROM (VALUES (8, 0), (8, 1), (8, 2), (8, 3)) AS v(w, m), LATERAL sift._test_radix_par(23, w, 24, 30000, m, 64, 0) AS r ORDER BY w, m;

DROP TABLE ap;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
