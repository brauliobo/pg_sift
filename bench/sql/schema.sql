CREATE EXTENSION IF NOT EXISTS pg_stat_statements;
CREATE EXTENSION IF NOT EXISTS pg_prewarm;

CREATE UNLOGGED TABLE src (
  src_id bigint, body text, category_id int, created date, likes int, url text
);

CREATE TABLE docs (
  id      bigint PRIMARY KEY,
  body    text NOT NULL,
  category_id int,
  created date,
  likes   int,
  url     text
);
CREATE SEQUENCE docs_id_seq OWNED BY docs.id;

CREATE TABLE categories (id int PRIMARY KEY, name text, region smallint);

-- Query trace: one sampled substring rendered as and/or/phrase.
CREATE TABLE queries (id int PRIMARY KEY, sub int, mode text, terms text[], doc_id bigint);

-- Tier-specific constants used by the workloads (e.g. the selective date cutoff).
CREATE TABLE params (k text PRIMARY KEY, v text);
