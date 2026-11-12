-- rollups by member (not by tenant: a pair edit costs nothing), one for each family of classes, kept by a statement trigger with a transition table;
-- a class reads them through the same semi join. Updates and deletes of events are not maintained here.
CREATE TABLE r_month (dim_a_id int NOT NULL, month date NOT NULL, n bigint NOT NULL, amount_sum bigint NOT NULL, PRIMARY KEY (dim_a_id, month));
CREATE TABLE r_score (dim_a_id int NOT NULL, score int NOT NULL, n bigint NOT NULL, PRIMARY KEY (dim_a_id, score));
CREATE TABLE r_chan (dim_a_id int NOT NULL, channel_id int NOT NULL, region text NOT NULL, n bigint NOT NULL, PRIMARY KEY (dim_a_id, channel_id, region));
CREATE TABLE r_kind (dim_a_id int NOT NULL, month date NOT NULL, kind text NOT NULL, n bigint NOT NULL, PRIMARY KEY (dim_a_id, month, kind));
@UPSERTS_EVENTS@
CREATE FUNCTION rollup_events_insert() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
@UPSERTS_NEWROWS@
  RETURN NULL;
END $$;
CREATE TRIGGER rollup_events_insert AFTER INSERT ON events REFERENCING NEW TABLE AS newrows FOR EACH STATEMENT EXECUTE FUNCTION rollup_events_insert();
VACUUM (FREEZE, ANALYZE) r_month, r_score, r_chan, r_kind;
