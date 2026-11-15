-- batch: append-readings
-- pattern: append_only
-- rows: nb
INSERT INTO reading SELECT * FROM gen_reading(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: late-readings
-- pattern: append_only
-- rows: nb / 10
INSERT INTO reading SELECT id, device_id, ts - interval '20 days', value, status FROM gen_reading(:nf + :round * :nf + 1, :nf + :round * :nf + :nb / 10);

-- batch: device-move-site
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE device SET site_id = 1 + (site_id + :round) % (SELECT count(*) FROM site) WHERE id % 10 = :round % 10;

-- batch: device-model-upgrade
-- pattern: dim_attr_churn
-- rows: nd / 5
UPDATE device SET model = 'w' || (1 + (id + :round) % :card) WHERE id % 5 = :round % 5;

-- batch: reading-swap-device
-- pattern: key_churn
-- rows: nf / 100
UPDATE reading SET device_id = 1 + (device_id + :round) % :nd WHERE id <= :nf AND id % 100 = :round % 100;

-- batch: retention-delete
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM reading WHERE id <= :nf / 10;

-- batch: restore-retained
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO reading SELECT * FROM gen_reading(1, :nf / 10);

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO reading SELECT * FROM gen_reading(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) reading;
REINDEX TABLE CONCURRENTLY reading;
