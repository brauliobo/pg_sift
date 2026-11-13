-- batch: append-entries
-- pattern: append_only
-- rows: nb
INSERT INTO entry SELECT * FROM gen_entry(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: version-reclassify
-- pattern: dim_attr_churn
-- rows: nd * 4 / 10
UPDATE account_version SET segment = 'w' || (1 + (account_id + :round) % :card), risk = 1 + (risk + :round) % 5 WHERE account_id % 10 = :round % 10;

-- batch: version-boundary-shift
-- pattern: key_churn
-- rows: nd / 10
UPDATE account_version SET valid_to = valid_to + 30 WHERE valid_to < date '9999-12-31' AND account_id % 10 = :round % 10;

-- batch: entry-move-account
-- pattern: key_churn
-- rows: nf / 50
UPDATE entry SET account_id = 1 + (account_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: late-postings
-- pattern: append_only
-- rows: nb / 10
INSERT INTO entry SELECT id, account_id, posted_on - 40, amount, kind FROM gen_entry(:nf + :round * :nf + 1, :nf + :round * :nf + :nb / 10);

-- batch: delete-entries
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM entry WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-entries
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO entry SELECT * FROM gen_entry(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO entry SELECT * FROM gen_entry(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) entry;
REINDEX TABLE CONCURRENTLY entry;
