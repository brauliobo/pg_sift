-- batch: append-events
-- pattern: append_only
-- rows: nb
INSERT INTO event SELECT * FROM gen_event(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: account-plan-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE account SET plan = 'w' || (1 + (n + :round) % :card), region = 'r' || (1 + (n + :round) % 8) WHERE n % 10 = :round % 10;

-- batch: event-change-account
-- pattern: key_churn
-- rows: nf / 50
UPDATE event SET account_id = acc(1 + (seq + :round) % :nd) WHERE seq <= :nf AND seq % 50 = :round % 50;

-- batch: accounts-rekey
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 100
UPDATE account SET id = md5('moved' || n || ':' || :round)::uuid WHERE n % 100 = 1;

-- batch: accounts-restored
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 100
UPDATE account SET id = acc(n) WHERE n % 100 = 1;

-- batch: delete-events
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM event WHERE seq <= :nf AND seq % 10 = :round % 10;

-- batch: resurrect-events
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO event SELECT * FROM gen_event(1, :nf) WHERE seq % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) event;
REINDEX TABLE CONCURRENTLY event;
