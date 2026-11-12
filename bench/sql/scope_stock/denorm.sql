-- @COPY@: a copy of the events for each (tenant, event) pair, kept by triggers: a new event goes to the copy of every tenant of its member (statement trigger,
-- transition table), a pair that appears copies the events of its member, a pair that goes deletes them. Updates and deletes of events are not maintained here.
CREATE INDEX events_dim ON events (dim_a_id);
@CREATE@
INSERT INTO @COPY@ SELECT p.tenant_id, e.* FROM events e JOIN pairs p ON p.dim_a_id = e.dim_a_id ORDER BY p.tenant_id, e.created_at, e.id;
@INDEXES@
CREATE FUNCTION copy_events_insert() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO @COPY@ SELECT p.tenant_id, n.* FROM newrows n JOIN pairs p ON p.dim_a_id = n.dim_a_id;
  RETURN NULL;
END $$;
CREATE TRIGGER copy_events_insert AFTER INSERT ON events REFERENCING NEW TABLE AS newrows FOR EACH STATEMENT EXECUTE FUNCTION copy_events_insert();
CREATE FUNCTION copy_pair_added() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO @COPY@ SELECT NEW.tenant_id, e.* FROM events e WHERE e.dim_a_id = NEW.dim_a_id;
  RETURN NULL;
END $$;
CREATE TRIGGER copy_pair_added AFTER INSERT ON pairs FOR EACH ROW EXECUTE FUNCTION copy_pair_added();
CREATE FUNCTION copy_pair_removed() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  DELETE FROM @COPY@ WHERE tenant_id = OLD.tenant_id AND dim_a_id = OLD.dim_a_id;
  RETURN NULL;
END $$;
CREATE TRIGGER copy_pair_removed AFTER DELETE ON pairs FOR EACH ROW EXECUTE FUNCTION copy_pair_removed();
VACUUM (FREEZE, ANALYZE) @COPY@;
