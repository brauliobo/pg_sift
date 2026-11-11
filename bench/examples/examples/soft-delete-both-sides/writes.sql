-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: soft-delete-facts
-- pattern: soft_delete
-- rows: nf / 20
UPDATE fact SET deleted_at = timestamp '2024-08-01' WHERE id <= :nf AND id % 20 = :round % 20 AND deleted_at IS NULL;

-- batch: undelete-facts
-- pattern: soft_delete
-- rows: nf / 20
UPDATE fact SET deleted_at = NULL WHERE id <= :nf AND id % 20 = (:round + 7) % 20;

-- batch: soft-delete-dims
-- pattern: soft_delete
-- rows: nd / 10
UPDATE dim SET deleted_at = timestamp '2024-08-01' WHERE id % 10 = :round % 10 AND deleted_at IS NULL;

-- batch: undelete-dims
-- pattern: soft_delete
-- rows: nd / 10
UPDATE dim SET deleted_at = NULL WHERE id % 10 = (:round + 3) % 10;

-- batch: dim-state-churn
-- pattern: dim_attr_churn
-- rows: nd / 5
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5) WHERE id % 5 = :round % 5;

-- batch: purge-deleted-facts
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM fact WHERE deleted_at IS NOT NULL AND id <= :nf;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) fact;
REINDEX TABLE CONCURRENTLY fact;
