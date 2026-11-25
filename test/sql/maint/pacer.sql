-- The pacer of the maintenance worker (core/maint/pacer.h): how long a step sleeps for the pages it writes, whether a step of space or structure starts, and which worker leaves for a request of a
-- tier before its own, all from the load of the foreground. The law and the priority run on the loads and the times the test gives: no clock, no sleep.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT sift._test_pacer_reset();

-- the regimes, from 8 slots of CPU (max_parallel_workers, at least 2) and the average of the scans against their usual: the sleeps of P0 / P1 / P2 / P3 in ms per 16 pages, -1 for a step that does not start
SELECT v.load, sift._test_pacer_decide(v.active, v.slots, v.ewma_us, v.base_us, v.yields, v.base_delay) AS decision
FROM (VALUES
 ('idle',                      0,  8, 1000, 1000, 0,   0),
 ('one backend, an operator floor of 5 ms', 1, 8, 1000, 1000, 0, 5),
 ('a quarter of the CPU',      2,  8, 1000, 1000, 0,   0),
 ('half of the CPU',           4,  8, 1000, 1000, 0,   0),
 ('scans twice as slow as usual', 0, 8, 2000, 1000, 0, 0),
 ('scans 2.4 times slower than a base under 1 ms', 0, 8, 2400, 100, 0, 0),
 ('three quarters of the CPU', 6,  8, 1000, 1000, 0,   0),
 ('all of the CPU',            8,  8, 1000, 1000, 0,   0),
 ('twice the CPU',            16,  8, 1000, 1000, 0,   0),
 ('scans three times slower',  0,  8, 3000, 1000, 0,   0),
 ('a step gave way for a lock', 0, 8, 1000, 1000, 1,   0),
 ('an operator floor of 100 ms', 0, 8, 1000, 1000, 0, 100),
 ('an operator floor of 100 ms at half the CPU', 4, 8, 1000, 1000, 0, 100),
 ('one slot of CPU counts as two', 1, 1, 1000, 1000, 0, 0)
) AS v(load, active, slots, ewma_us, base_us, yields, base_delay);

-- the priority: a request of a tier that found no free worker makes the workers of the tiers after it leave, for as long as sift.maintenance_naptime (2 s), and no worker leaves for a lower tier
SHOW sift.maintenance_naptime;
SELECT sift._test_pacer_reset();
SELECT tier, sift._test_pacer_yield(tier, 800000000000000) AS leaves_when_nobody_waits FROM generate_series(0, 3) tier;
SELECT sift._test_pacer_starve(1, 800000010000000);
SELECT tier, sift._test_pacer_yield(tier, 800000010500000) AS leaves_half_a_second_after_a_p1_request FROM generate_series(0, 3) tier;
SELECT tier, sift._test_pacer_yield(tier, 800000012500000) AS leaves_two_and_a_half_seconds_after FROM generate_series(0, 3) tier;
SELECT sift._test_pacer_starve(0, 800000020000000);
SELECT tier, sift._test_pacer_yield(tier, 800000020100000) AS leaves_after_a_p0_request FROM generate_series(0, 3) tier;

-- the foreground: one scan in 64 feeds the fast and the slow average (1/16 and 1/256, the slow one never under 1 ms)
SELECT sift._test_pacer_reset();
SELECT sift._test_pacer_foreground(2000, 64) AS first_sample;
SELECT sift._test_pacer_foreground(8000, 64) AS a_slow_scan;
SELECT sift._test_pacer_foreground(8000, 63) AS not_a_full_round_of_64;
SELECT sift._test_pacer_reset();
SELECT sift._test_pacer_foreground(100, 64) AS faster_than_the_floor;
SELECT sift._test_pacer_reset();

-- what a worker last sampled (sift_pacer_snapshot, which sift_internal.maint_pacer reports): the mode, the running backends and the sleep of a P1 step; FULL, 0 and 0 before the first look
SELECT sift._test_pacer_snapshot() AS before_the_first_look;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
