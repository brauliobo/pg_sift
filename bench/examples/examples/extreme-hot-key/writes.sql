-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: dim-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 3 + 1
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5), flag = NOT flag WHERE id % 3 = :round % 3;

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

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) fact;
REINDEX TABLE CONCURRENTLY fact;

-- batch: hot-dim-flip
-- pattern: dim_attr_churn
-- rows: nf * 3 / 10
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5), flag = NOT flag WHERE id = 1;
