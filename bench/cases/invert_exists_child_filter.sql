-- Parents that have a child of a kind, counted as the distinct keys of the children of that kind that have a parent: the parent is not scanned
-- label: `SELECT count(*) FROM iv_dims WHERE EXISTS (SELECT 1 FROM iv_events WHERE iv_events.dim_id = iv_dims.id AND iv_events.kind = 7)` over the rows of the dataset and a tenth as many parents joined by a foreign key, the index of the children connected to the parent and filled; `join` is the same statement with sift.agg_child_first off (the parents are the rows scanned and the children a table loaded for the join)
-- ordered: no
-- set: sift.agg_force = on; sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
CREATE TABLE iv_dims (id int PRIMARY KEY, grp int);
INSERT INTO iv_dims SELECT g, g % 50 FROM generate_series(1, {rows} / 10) g;
CREATE INDEX iv_dims_col ON iv_dims USING sift_col (id, grp);
CREATE TABLE iv_events (id int PRIMARY KEY, dim_id int REFERENCES iv_dims (id), kind int);
INSERT INTO iv_events SELECT g, CASE WHEN g % 100 = 0 THEN NULL ELSE 1 + (g::bigint * 7919) % ({rows} / 10) END, g % 1000 FROM generate_series(1, {rows}) g;
CREATE INDEX iv_events_col ON iv_events USING sift_col (id, dim_id, kind);
VACUUM (FREEZE, ANALYZE) iv_dims, iv_events;
SELECT sift.col_maintain('iv_events_col');
VACUUM ANALYZE iv_dims
--@ pg
SELECT count(*) FROM iv_dims WHERE EXISTS (SELECT 1 FROM iv_events WHERE iv_events.dim_id = iv_dims.id AND iv_events.kind = 7)
