-- batch: append-events
-- pattern: append_only
-- rows: nb
INSERT INTO event SELECT * FROM gen_event(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: source-reclassify
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE source SET kind = 'w' || (1 + (id + :round) % :card), region = 'r' || (1 + (id + :round) % 6) WHERE id % 10 = :round % 10;

-- batch: event-change-source
-- pattern: key_churn
-- rows: nf / 50
UPDATE event SET feed_id = 1 + (feed_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: reload-one-month
-- pattern: bulk_reload
-- concurrent: no
-- rows: nf / 12
BEGIN;
DELETE FROM event WHERE ts >= '2024-03-01' AND ts < '2024-04-01' AND id <= :nf;
INSERT INTO event SELECT * FROM gen_event(1, :nf) WHERE ts >= '2024-03-01' AND ts < '2024-04-01';
COMMIT;

-- batch: delete-old-quarter
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 4
DELETE FROM event WHERE ts < '2024-04-01' AND id <= :nf;

-- batch: restore-old-quarter
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 4
INSERT INTO event SELECT * FROM gen_event(1, :nf) WHERE ts < '2024-04-01';

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rounds: 3
-- rows: 0
-- a sift_table VACUUM asks every 50 ms for the lock of its cut and gives up after 5 s for each file, so the 12 partitions of event under readers
-- and writers would take a minute; each round vacuums one of them
VACUUM (ANALYZE) event_01;

-- batch: drop-recreate-sift-index
-- pattern: maintenance
-- maintenance: yes
-- rounds: 3
-- sift_only: yes
-- rows: 0
DROP INDEX CONCURRENTLY source_col;
CREATE INDEX CONCURRENTLY source_col ON source USING sift_col (id, kind, region);
