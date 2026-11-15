-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: status-relabel
-- pattern: dim_attr_churn
-- rows: 5
UPDATE status SET is_final = (rank + :round) % 2 = 0, rank = 1 + (rank + :round) % 5;

-- batch: facts-change-status
-- pattern: key_churn
-- rows: nf / 20
UPDATE fact SET status_code = (ARRAY['open', 'pending', 'paid', 'shipped', 'closed'])[1 + (id + :round) % 5] WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: status-retired
-- pattern: orphan_fact
-- concurrent: no
-- rows: nf / 5
DELETE FROM status WHERE code = 'pending';

-- batch: status-restored
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nf / 5
INSERT INTO status VALUES ('pending', 'Pending', false, 2);

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
