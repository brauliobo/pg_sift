-- an IN list of 60 keys of a column the heap holds in the order of the key, with `point_tid_columns`: the index and bitmap scans of the sift_col index stay beside the aggregate scan and the costs choose
-- label: `count(*), sum(v) WHERE id = ANY of 60 ids` over a heap in the order of id, `point_tid_columns = 'id'`; the variant `btree` is a b-tree on id
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS pi;
CREATE TABLE pi AS SELECT g::int AS id, (g % 1000)::int AS v, (g % 97)::int AS w FROM generate_series(1, {rows}) g;
CREATE INDEX pi_col ON pi USING sift_col (id, v, w) WITH (point_tid_columns = 'id');
CREATE SCHEMA c_point_in_ordered_60__btree;
CREATE TABLE c_point_in_ordered_60__btree.pi AS TABLE pi;
CREATE INDEX pi_bt ON c_point_in_ordered_60__btree.pi (id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) pi;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_point_in_ordered_60__btree.pi
--@ pg
SELECT count(*), sum(v) FROM pi WHERE id = ANY (ARRAY[(7919 % {rows} + 1), (15838 % {rows} + 1), (23757 % {rows} + 1), (31676 % {rows} + 1), (39595 % {rows} + 1), (47514 % {rows} + 1), (55433 % {rows} + 1), (63352 % {rows} + 1), (71271 % {rows} + 1), (79190 % {rows} + 1), (87109 % {rows} + 1), (95028 % {rows} + 1), (102947 % {rows} + 1), (110866 % {rows} + 1), (118785 % {rows} + 1), (126704 % {rows} + 1), (134623 % {rows} + 1), (142542 % {rows} + 1), (150461 % {rows} + 1), (158380 % {rows} + 1), (166299 % {rows} + 1), (174218 % {rows} + 1), (182137 % {rows} + 1), (190056 % {rows} + 1), (197975 % {rows} + 1), (205894 % {rows} + 1), (213813 % {rows} + 1), (221732 % {rows} + 1), (229651 % {rows} + 1), (237570 % {rows} + 1), (245489 % {rows} + 1), (253408 % {rows} + 1), (261327 % {rows} + 1), (269246 % {rows} + 1), (277165 % {rows} + 1), (285084 % {rows} + 1), (293003 % {rows} + 1), (300922 % {rows} + 1), (308841 % {rows} + 1), (316760 % {rows} + 1), (324679 % {rows} + 1), (332598 % {rows} + 1), (340517 % {rows} + 1), (348436 % {rows} + 1), (356355 % {rows} + 1), (364274 % {rows} + 1), (372193 % {rows} + 1), (380112 % {rows} + 1), (388031 % {rows} + 1), (395950 % {rows} + 1), (403869 % {rows} + 1), (411788 % {rows} + 1), (419707 % {rows} + 1), (427626 % {rows} + 1), (435545 % {rows} + 1), (443464 % {rows} + 1), (451383 % {rows} + 1), (459302 % {rows} + 1), (467221 % {rows} + 1), (475140 % {rows} + 1)])
