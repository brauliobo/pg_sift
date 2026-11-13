-- batch: append-events
-- pattern: append_only
-- rows: nb
INSERT INTO event SELECT * FROM gen_event(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: catalog-recategorize
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE catalog SET category = 'w' || (1 + (sku + :round) % :card), price = price + 1 WHERE sku % 10 = :round % 10;

-- batch: event-resku
-- pattern: key_churn
-- rows: nf / 50
UPDATE event SET attrs = jsonb_set(attrs, '{sku}', to_jsonb(1 + ((attrs ->> 'sku')::int + :round) % :nd)) WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: event-channel-change
-- pattern: dim_attr_churn
-- rows: nf / 20
UPDATE event SET attrs = jsonb_set(attrs, '{channel}', to_jsonb('ch' || (1 + (id + :round) % 4))) WHERE id <= :nf AND id % 20 = :round % 20;

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

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO event SELECT * FROM gen_event(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) event;
REINDEX TABLE CONCURRENTLY event;
