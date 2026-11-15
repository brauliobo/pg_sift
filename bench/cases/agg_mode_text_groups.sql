-- the most frequent value of a text column in each of a few groups is found by counting the values, where each value went through two sorts
-- label: `SELECT grp, mode() WITHIN GROUP (ORDER BY status) FROM md_fact GROUP BY grp`, 12 groups of 9 statuses
-- ordered: yes
-- set: sift.agg_force = on
--@ setup
CREATE TABLE md_fact AS SELECT g AS id, g % 12 AS grp, ('status_' || (g::bigint * 7919 % 9 + (g % 5) / 4))::text AS status, (g::bigint * 104729 % 1000)::int AS qty FROM generate_series(1, {rows}) g;
CREATE INDEX md_fact_col ON md_fact USING sift_col (id, grp, status, qty);
VACUUM (FREEZE, ANALYZE) md_fact
--@ pg
SELECT grp, mode() WITHIN GROUP (ORDER BY status) AS common_status FROM md_fact GROUP BY grp ORDER BY grp
