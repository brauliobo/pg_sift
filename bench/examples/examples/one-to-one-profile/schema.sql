-- 1:1 between account and profile (profile.account_id is the key; 90% of the accounts have one), activity as the fact
CREATE TABLE account  (id int PRIMARY KEY, plan text NOT NULL, region text NOT NULL);
CREATE TABLE profile  (account_id int PRIMARY KEY, tier int NOT NULL, locale text NOT NULL, bio_len int NOT NULL);
CREATE TABLE activity (id bigint PRIMARY KEY, account_id int NOT NULL, units int NOT NULL, at timestamp NOT NULL);
