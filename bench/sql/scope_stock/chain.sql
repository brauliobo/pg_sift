-- The maintained pair table of the denormalised and incremental-view arms: grants and memberships -> pair_paths (paths of a pair) -> pairs (distinct pairs).
-- A pair edit is a row trigger on the two relations; a pair that appears or disappears fires the row triggers of `pairs` (the arms hang their work on those).
CREATE TABLE pair_paths (tenant_id int NOT NULL, dim_a_id int NOT NULL, n int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id));
CREATE TABLE pairs (tenant_id int NOT NULL, dim_a_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id));
CREATE INDEX pairs_by_member ON pairs (dim_a_id);
INSERT INTO pair_paths
  SELECT tenant_id, dim_a_id, count(*)
    FROM (SELECT tenant_id, dim_a_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
          UNION ALL
          SELECT g.tenant_id, m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL) x
   GROUP BY 1, 2;
INSERT INTO pairs SELECT tenant_id, dim_a_id FROM pair_paths;

CREATE FUNCTION pair_paths_bump(t int, ids int[], d int) RETURNS void LANGUAGE sql AS $$
  INSERT INTO pair_paths AS pp (tenant_id, dim_a_id, n) SELECT t, m, d FROM unnest(ids) m
  ON CONFLICT (tenant_id, dim_a_id) DO UPDATE SET n = pp.n + d
$$;
CREATE FUNCTION pair_paths_changed() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_OP = 'INSERT' AND NEW.n > 0 THEN INSERT INTO pairs VALUES (NEW.tenant_id, NEW.dim_a_id);
  ELSIF TG_OP = 'UPDATE' AND OLD.n = 0 AND NEW.n > 0 THEN INSERT INTO pairs VALUES (NEW.tenant_id, NEW.dim_a_id);
  ELSIF TG_OP = 'UPDATE' AND OLD.n > 0 AND NEW.n = 0 THEN DELETE FROM pairs WHERE tenant_id = NEW.tenant_id AND dim_a_id = NEW.dim_a_id;
  END IF;
  RETURN NULL;
END $$;
CREATE TRIGGER pair_paths_changed AFTER INSERT OR UPDATE OF n ON pair_paths FOR EACH ROW EXECUTE FUNCTION pair_paths_changed();

CREATE FUNCTION grant_members(g grants) RETURNS int[] LANGUAGE sql STABLE AS $$
  SELECT CASE WHEN g.group_id IS NULL THEN ARRAY[g.dim_a_id]
              ELSE ARRAY(SELECT dim_a_id FROM memberships WHERE group_id = g.group_id AND deleted_at IS NULL) END
$$;
CREATE FUNCTION grants_pairs() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_OP IN ('UPDATE', 'DELETE') AND OLD.deleted_at IS NULL THEN PERFORM pair_paths_bump(OLD.tenant_id, grant_members(OLD), -1); END IF;
  IF TG_OP IN ('INSERT', 'UPDATE') AND NEW.deleted_at IS NULL THEN PERFORM pair_paths_bump(NEW.tenant_id, grant_members(NEW), 1); END IF;
  RETURN NULL;
END $$;
CREATE TRIGGER grants_pairs AFTER INSERT OR UPDATE OR DELETE ON grants FOR EACH ROW EXECUTE FUNCTION grants_pairs();

CREATE FUNCTION memberships_pairs() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE t int;
BEGIN
  IF TG_OP IN ('UPDATE', 'DELETE') AND OLD.deleted_at IS NULL THEN
    FOR t IN SELECT tenant_id FROM grants WHERE group_id = OLD.group_id AND deleted_at IS NULL LOOP PERFORM pair_paths_bump(t, ARRAY[OLD.dim_a_id], -1); END LOOP;
  END IF;
  IF TG_OP IN ('INSERT', 'UPDATE') AND NEW.deleted_at IS NULL THEN
    FOR t IN SELECT tenant_id FROM grants WHERE group_id = NEW.group_id AND deleted_at IS NULL LOOP PERFORM pair_paths_bump(t, ARRAY[NEW.dim_a_id], 1); END LOOP;
  END IF;
  RETURN NULL;
END $$;
CREATE TRIGGER memberships_pairs AFTER INSERT OR UPDATE OR DELETE ON memberships FOR EACH ROW EXECUTE FUNCTION memberships_pairs();
