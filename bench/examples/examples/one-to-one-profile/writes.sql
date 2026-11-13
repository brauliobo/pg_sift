-- batch: append-activity
-- pattern: append_only
-- rows: nb
INSERT INTO activity SELECT * FROM gen_activity(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: profile-tier-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE profile SET tier = 1 + (tier + :round) % 4, bio_len = (bio_len + :round * 37) % 2000 WHERE account_id % 10 = :round % 10;

-- batch: account-plan-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE account SET plan = 'w' || (1 + (id + :round) % :card) WHERE id % 10 = :round % 10;

-- batch: profiles-created-late
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 10
INSERT INTO profile SELECT g::int, gx.pick(g, (SELECT s + 3 FROM gx.knob), 4), 'l' || gx.pick(g, (SELECT s + 4 FROM gx.knob), 10), gx.pick(g, (SELECT s + 5 FROM gx.knob), 2000)
 FROM generate_series(1, :nd) g WHERE NOT EXISTS (SELECT 1 FROM profile x WHERE x.account_id = g) AND g % 3 = 0;

-- batch: profiles-deleted
-- pattern: delete_heavy
-- concurrent: no
-- rows: nd / 20
DELETE FROM profile WHERE account_id % 20 = :round % 20;

-- batch: activity-change-account
-- pattern: key_churn
-- rows: nf / 50
UPDATE activity SET account_id = 1 + (account_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) activity;
REINDEX TABLE CONCURRENTLY activity;
