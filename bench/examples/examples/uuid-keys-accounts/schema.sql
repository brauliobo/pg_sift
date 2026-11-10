-- uuid keys on both sides: account.id and event.account_id are uuids (hash-spread, no order); n is the ordinal the generator and the writes use
CREATE TABLE account (id uuid PRIMARY KEY, n int NOT NULL, plan text NOT NULL, region text NOT NULL);
CREATE TABLE event   (id uuid PRIMARY KEY, seq bigint NOT NULL, account_id uuid NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL);
