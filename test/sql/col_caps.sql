-- The caps of the options, told at CREATE INDEX: a cube(...) dim has no histogram in a unit with more groups than its cap, a set_columns leaf keeps a
-- bloom filter, not its keys, in a unit with more than set_limit distinct ones and nothing where its keys are one value or fill their range, a key_presence
-- leaf keeps no bitmap over 3% of the unit, a rollup_dims rollup is not written in a unit with too many cells, and calendar_columns = 'all' leaves out the
-- date columns past the 16 dimensions of the cube; a query on them scans those units, so the build says how many, and what to set.
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '60s';
CREATE EXTENSION pg_sift;

-- 100,000 rows in 3 units. few: 7 values; many: 50,000 values (more than the 4,096 groups of a cube dim in every unit); mid: 400 values, none next to another
-- (a bloom filter beyond set_limit, and no gap-free range that a zone would tell); wide: 3,000 values
CREATE TABLE caps (id int, few int, many int, mid int, wide int) WITH (autovacuum_enabled = off);
INSERT INTO caps SELECT g, g % 7, (g * 7919) % 50000, ((g * 7919) % 400) * 3, ((g * 7919) % 3000) * 3 FROM generate_series(1, 100000) g;

-- a dim over its cap, and a set summary over set_limit that a higher one makes exact
CREATE INDEX caps_a ON caps USING sift_col (id, few, many, mid) WITH (preagg = 'cube(few: id; many: id)', set_columns = 'mid', set_limit = 32);
-- a set summary no set_limit can make exact
CREATE INDEX caps_b ON caps USING sift_col (id, few, wide) WITH (set_columns = 'wide');
-- the caps hold: nothing to tell
CREATE INDEX caps_c ON caps USING sift_col (id, few, many, mid) WITH (preagg = 'cube(few: id)', set_columns = 'mid', set_limit = 512);
-- the default spec asks for no dim by name, and no set: silent
CREATE INDEX caps_d ON caps USING sift_col (id, few, many, mid);
-- nothing written: a set summary of keys that fill their range (few), a bitmap over 3% of the unit (many), a rollup of more cells than a tenth of the rows
CREATE INDEX caps_e ON caps USING sift_col (id, few, many, mid) WITH (set_columns = 'few', key_presence = 'many', rollup_dims = 'few, many');
-- what is written is silent: a bitmap that fits (few); the 2,800 combinations of a rollup fit two units and not the third, which holds fewer rows (34%)
CREATE INDEX caps_f ON caps USING sift_col (id, few, many, mid) WITH (key_presence = 'few', rollup_dims = 'few, mid');
-- a calendar list that does not fit the cube, 'all' that leaves the last date columns out
CREATE TABLE caps_cal (c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int, c9 int, c10 int, c11 int, c12 int, c13 int, c14 int, c15 int, c16 int, d1 date, d2 date) WITH (autovacuum_enabled = off);
INSERT INTO caps_cal SELECT g, g % 3, g % 4, g % 5, g % 6, g % 7, g % 8, g % 9, g % 3, g % 4, g % 5, g % 6, g % 7, g % 8, g % 9, g % 2, date '2024-01-01' + g % 30, date '2024-01-01' + g % 20 FROM generate_series(1, 5000) g;
CREATE INDEX caps_cal_all ON caps_cal USING sift_col (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, d1, d2) WITH (calendar_columns = 'all');
-- the default is the same list
CREATE INDEX caps_cal_default ON caps_cal USING sift_col (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, d1, d2);
-- all of them fit
CREATE INDEX caps_cal_two ON caps_cal USING sift_col (c1, c2, d1, d2) WITH (calendar_columns = 'all');
-- what the hint says to do
ALTER INDEX caps_a SET (set_limit = 512);
REINDEX INDEX caps_a;
-- a refresh tells the same, whether or not it rewrote a segment
ALTER INDEX caps_c SET (preagg = 'cube(few: id; many: id)');
SELECT * FROM sift.col_refresh('caps_c');
DROP TABLE caps, caps_cal;
DROP EXTENSION pg_sift;
