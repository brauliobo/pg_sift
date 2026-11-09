-- The same join with 10% of the parent rows updated in place after the stamp (a tenth of the keys outside the proof, probed in the parent btree)
-- label: `SELECT kind, count(*) FROM fp_events WHERE EXISTS (SELECT 1 FROM fp_dims WHERE fp_dims.id = fp_events.dim_id) GROUP BY 1` over the rows of the dataset and a tenth as many parents joined by a foreign key, the index of the events connected to the parent and filled and 10% of the parent rows updated in place afterwards; `join` is the same index with sift.agg_connect_found off (the join loads the parent)
-- ordered: no
-- set: sift.agg_force = on; sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
CREATE TABLE fp_dims (id int PRIMARY KEY, grp int, note int);
INSERT INTO fp_dims SELECT g, g % 50, 0 FROM generate_series(1, {rows} / 10) g;
CREATE INDEX fp_dims_col ON fp_dims USING sift_col (id, grp);
CREATE TABLE fp_events (id int PRIMARY KEY, dim_id int REFERENCES fp_dims (id), kind int);
INSERT INTO fp_events SELECT g, CASE WHEN g % 100 = 0 THEN NULL ELSE 1 + (g::bigint * 7919) % ({rows} / 10) END, g % 5 FROM generate_series(1, {rows}) g;
CREATE INDEX fp_events_col ON fp_events USING sift_col (id, dim_id, kind);
VACUUM (FREEZE, ANALYZE) fp_dims, fp_events;
SELECT sift.col_maintain('fp_events_col');
UPDATE fp_dims SET grp = grp + 1 WHERE id % 10 = 0;
VACUUM ANALYZE fp_dims
--@ pg
SELECT kind, count(*) FROM fp_events WHERE EXISTS (SELECT 1 FROM fp_dims WHERE fp_dims.id = fp_events.dim_id) GROUP BY 1
