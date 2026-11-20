-- uuid and text keys, wide text: log -> service (uuid) -> team (text key)
CREATE TABLE team    (id text PRIMARY KEY, owner text NOT NULL, tier int NOT NULL);
CREATE TABLE service (id uuid PRIMARY KEY, name text NOT NULL, team_id text NOT NULL, env text NOT NULL);
CREATE TABLE log     (id bigint PRIMARY KEY, service_id uuid NOT NULL, level text NOT NULL, message text NOT NULL, ts timestamp NOT NULL);
