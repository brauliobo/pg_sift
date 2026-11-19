-- batch: append-events
-- pattern: append_only
-- rows: nb
INSERT INTO event SELECT * FROM gen_event(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: tag-items
-- pattern: append_only
-- rows: nd / 20
INSERT INTO item_tag SELECT i, 1 + (i + :round) % (SELECT count(*) FROM tag) FROM generate_series(1, :nd) i WHERE i % 20 = :round % 20 ON CONFLICT DO NOTHING;

-- batch: untag-items
-- pattern: delete_heavy
-- rows: nd / 10
DELETE FROM item_tag WHERE item_id <= :nd AND item_id % 10 = :round % 10 AND tag_id % 2 = 0;

-- batch: tag-regroup
-- pattern: dim_attr_churn
-- rows: nd / 100
UPDATE tag SET grp = 'w' || (1 + (id + :round) % 8) WHERE id % 3 = :round % 3;

-- batch: retag-move
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 20
UPDATE item_tag SET tag_id = 1 + (tag_id + :round) % (SELECT count(*) FROM tag) WHERE item_id % 20 = :round % 20 AND NOT EXISTS
  (SELECT 1 FROM item_tag x WHERE x.item_id = item_tag.item_id AND x.tag_id = 1 + (item_tag.tag_id + :round) % (SELECT count(*) FROM tag));

-- batch: item-owner-change
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE item SET kind = 'w' || (1 + (id + :round) % :card), keeper_id = 1 + (keeper_id + :round) % 100 WHERE id % 10 = :round % 10;

-- batch: delete-events
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM event WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-events
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO event SELECT * FROM gen_event(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-link-reload
-- pattern: bulk_reload
-- concurrent: no
-- rows: nd * 3
BEGIN;
DELETE FROM item_tag;
INSERT INTO item_tag SELECT * FROM gen_item_tag(1, :nd);
COMMIT;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) item_tag;
REINDEX TABLE CONCURRENTLY item_tag;
