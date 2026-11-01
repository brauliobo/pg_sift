-- a join whose condition is no key (`k IN (SELECT n + 1 FROM keys)`: the addition can raise) tests every tuple the scan keeps against every row of the joined table, so the scan is priced for those tuples and the planner keeps the hash join
-- label: `SELECT count(*), sum(v) WHERE k IN (SELECT n + 1 FROM keys)` over rows facts of distinct k and 500 keys; PostgreSQL's hash join is the plan and the lane `forced` is the scan the planner refuses
-- ordered: yes
--@ setup
CREATE TABLE jf_facts (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO jf_facts SELECT i, i, i % 10 FROM generate_series(1, {rows}) i;
CREATE INDEX jf_facts_col ON jf_facts USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) jf_facts;
CREATE TABLE jf_keys (n int) WITH (autovacuum_enabled = false);
INSERT INTO jf_keys SELECT i * ({rows} / 500) - 1 FROM generate_series(1, 500) i;
VACUUM (FREEZE, ANALYZE) jf_keys
--@ pg
SELECT count(*), sum(v) FROM jf_facts WHERE k IN (SELECT n + 1 FROM jf_keys)
