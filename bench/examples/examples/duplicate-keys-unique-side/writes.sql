-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: dim-new-versions
-- pattern: dim_attr_churn
-- rows: nd / 50
INSERT INTO dim SELECT id, grp, 'w' || (1 + (id + :round) % 5), 4 + :round FROM dim WHERE id % 50 = :round % 50 AND version = 1;

-- batch: dim-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5) WHERE id % 10 = :round % 10;

-- batch: dim-dedupe
-- pattern: delete_heavy
-- concurrent: no
-- rows: nd / 50
DELETE FROM dim WHERE version > 1;

-- batch: fact-change-dim
-- pattern: key_churn
-- rows: nf / 50
UPDATE fact SET dim_id = 1 + (dim_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-facts
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM fact WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-facts
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO fact SELECT * FROM gen_fact(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) fact;
REINDEX TABLE CONCURRENTLY fact;
