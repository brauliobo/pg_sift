-- join on an expression: lower(contact.email) = member.email_lc (mixed case on one side, folded on the other)
CREATE TABLE member  (id int PRIMARY KEY, email_lc text NOT NULL, tier int NOT NULL, country text NOT NULL);
CREATE TABLE contact (id bigint PRIMARY KEY, email text NOT NULL, created_at timestamp NOT NULL, channel text NOT NULL);
