-- batch: append-readings
-- pattern: append_only
-- rows: nb
INSERT INTO reading SELECT * FROM gen_reading(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: zone-reclassify
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE zone SET kind = 'w' || (1 + (right(id, 7)::int + :round) % :card), capacity = (capacity + :round * 7) % 1000 WHERE right(id, 7)::int % 10 = :round % 10;

-- batch: reading-change-zone
-- pattern: key_churn
-- rows: nf / 50
UPDATE reading SET zone_id = zk(1 + (right(zone_id, 7)::int + :round) % :nd) WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: zones-retired
-- pattern: orphan_fact
-- concurrent: no
-- rows: nd / 20
DELETE FROM zone WHERE right(id, 7)::int % 20 = 3;

-- batch: zones-return
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 20
INSERT INTO zone SELECT * FROM gen_zone(1, :nd) g WHERE right(g.id, 7)::int % 20 = 3;

-- batch: delete-readings
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM reading WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-readings
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO reading SELECT * FROM gen_reading(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) reading;
REINDEX TABLE CONCURRENTLY reading;
