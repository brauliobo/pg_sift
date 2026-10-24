-- Runs once, when the data directory is created: the first database and every database made from template1 later have the extension.
CREATE EXTENSION IF NOT EXISTS pg_sift;
\connect template1
CREATE EXTENSION IF NOT EXISTS pg_sift;
