-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

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


-- batch: reload-dimension
-- pattern: bulk_reload
-- concurrent: no
-- rows: nd
BEGIN;
DELETE FROM dim;
INSERT INTO dim SELECT * FROM gen_dim(1, :nd);
COMMIT;

-- batch: reload-fact-tenth
-- pattern: bulk_reload
-- concurrent: no
-- rows: nf / 10
BEGIN;
DELETE FROM fact WHERE id <= :nf / 10;
INSERT INTO fact SELECT * FROM gen_fact(1, :nf / 10);
COMMIT;

-- batch: swap-dimension-attributes
-- pattern: bulk_reload
-- concurrent: no
-- rows: nd
BEGIN;
DELETE FROM dim;
INSERT INTO dim SELECT id, grp, 'w' || (1 + (id + :round) % 5), NOT flag, weight FROM gen_dim(1, :nd);
COMMIT;
