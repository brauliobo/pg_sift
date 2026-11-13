-- bucket lookups by an exact text key: readings carry a zone key, the zone table says what the zone is
CREATE TABLE zone    (id text PRIMARY KEY, region text NOT NULL, kind text NOT NULL, capacity int NOT NULL);
CREATE TABLE reading (id bigint PRIMARY KEY, zone_id text NOT NULL, level int NOT NULL, at timestamp NOT NULL);
