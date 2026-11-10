-- ledger entries against a versioned dimension: the account's segment and risk are those valid on the posting date (range join)
CREATE TABLE account         (id int PRIMARY KEY, kind text NOT NULL);
CREATE TABLE account_version (account_id int NOT NULL, valid_from date NOT NULL, valid_to date NOT NULL, segment text NOT NULL, risk int NOT NULL, PRIMARY KEY (account_id, valid_from));
CREATE TABLE entry           (id bigint PRIMARY KEY, account_id int NOT NULL, posted_on date NOT NULL, amount numeric(14,2) NOT NULL, kind text NOT NULL);
