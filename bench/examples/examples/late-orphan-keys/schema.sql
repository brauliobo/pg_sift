-- facts whose dimension row arrives late, in waves, and rows that never get one: gaps in the dimension key space
CREATE TABLE entity (id int PRIMARY KEY, class text NOT NULL, region text NOT NULL);
CREATE TABLE fact   (id bigint PRIMARY KEY, target_id int NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL);
