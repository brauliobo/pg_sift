-- batch: append-sessions
-- pattern: append_only
-- rows: nb
INSERT INTO session SELECT * FROM gen_session(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: user-plan-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE users SET plan = 'w' || (1 + (id + :round) % 4) WHERE id % 10 = :round % 10;

-- batch: anonymise-sessions
-- pattern: key_churn
-- rows: nf / 25
UPDATE session SET user_id = NULL WHERE id <= :nf AND id % 25 = :round % 25;

-- batch: attach-anonymous-sessions
-- pattern: key_churn
-- rows: nf / 70
UPDATE session SET user_id = 1 + id % :nd WHERE user_id IS NULL AND id % 7 = :round % 7;

-- batch: delete-users
-- pattern: orphan_fact
-- concurrent: no
-- rows: nd / 20
DELETE FROM users WHERE id <= :nd AND id % 20 = :round % 20;

-- batch: users-return
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 20
INSERT INTO users SELECT * FROM gen_users(1, :nd) WHERE id % 20 = (:round - 1) % 20;

-- batch: delete-sessions
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM session WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-sessions
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO session SELECT * FROM gen_session(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) session;
REINDEX TABLE CONCURRENTLY session;
