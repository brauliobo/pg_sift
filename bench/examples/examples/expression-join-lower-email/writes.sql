-- batch: append-contacts
-- pattern: append_only
-- rows: nb
INSERT INTO contact SELECT * FROM gen_contact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: member-tier-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE member SET tier = 1 + (tier + :round) % 4, country = 'c' || (1 + (id + :round) % 12) WHERE id % 10 = :round % 10;

-- batch: contact-recase
-- pattern: key_churn
-- rows: nf / 20
UPDATE contact SET email = CASE WHEN email = lower(email) THEN upper(email) ELSE lower(email) END WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: contact-other-member
-- pattern: key_churn
-- rows: nf / 50
UPDATE contact SET email = 'user' || (1 + (id + :round) % :nd) || '@example.com' WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: member-email-change
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 50
UPDATE member SET email_lc = 'moved' || id || '@example.com' WHERE id % 50 = 1;

-- batch: people-restored
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 50
UPDATE member SET email_lc = 'user' || id || '@example.com' WHERE id % 50 = 1;

-- batch: delete-contacts
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM contact WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-contacts
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO contact SELECT * FROM gen_contact(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) contact;
REINDEX TABLE CONCURRENTLY contact;
