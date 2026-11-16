-- batch: new-follows
-- pattern: append_only
-- rows: nb
INSERT INTO follows SELECT * FROM gen_follows(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: unfollow
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 20
DELETE FROM follows WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: refollow
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 20
INSERT INTO follows SELECT * FROM gen_follows(1, :nf) WHERE id % 20 = (:round - 1) % 20;

-- batch: person-moves-country
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE person SET country = 'c' || (1 + (id + :round) % 12), tier = 1 + (tier + :round) % 4 WHERE id % 10 = :round % 10;

-- batch: follow-redirect
-- pattern: key_churn
-- rows: nf / 50
UPDATE follows SET followee = 1 + (followee + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: bulk-follows
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO follows SELECT * FROM gen_follows(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) follows;
REINDEX TABLE CONCURRENTLY follows;
