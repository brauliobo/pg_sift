-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: late-wave-1
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 20
INSERT INTO entity SELECT g::int, gx.word(g, (SELECT s + 1 FROM gx.knob), :card), 'r' || gx.pick(g, (SELECT s + 2 FROM gx.knob), 6) FROM generate_series(1, :nd) g
 WHERE gx.u(g, (SELECT s + 3 FROM gx.knob)) < 0.15 AND g % 3 = 0 AND NOT EXISTS (SELECT 1 FROM entity x WHERE x.id = g);

-- batch: late-wave-2
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 20
INSERT INTO entity SELECT g::int, gx.word(g, (SELECT s + 1 FROM gx.knob), :card), 'r' || gx.pick(g, (SELECT s + 2 FROM gx.knob), 6) FROM generate_series(1, :nd) g
 WHERE gx.u(g, (SELECT s + 3 FROM gx.knob)) < 0.15 AND NOT EXISTS (SELECT 1 FROM entity x WHERE x.id = g);

-- batch: orphan-burst
-- pattern: orphan_fact
-- rows: nb / 5
INSERT INTO fact SELECT id, :nd + 1000 + id % 500, amount, at FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb / 5);

-- batch: entity-class-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE entity SET class = 'w' || (1 + (id + :round) % :card) WHERE id % 10 = :round % 10;

-- batch: targets-retired
-- pattern: orphan_fact
-- concurrent: no
-- rows: nd / 20
DELETE FROM entity WHERE id % 20 = 5;

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
