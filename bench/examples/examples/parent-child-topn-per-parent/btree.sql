CREATE INDEX child_parent_amount_idx ON child (parent_id, amount DESC, id);
CREATE INDEX child_parent_at_idx ON child (parent_id, at);
CREATE INDEX child_kind_idx ON child (kind);
CREATE INDEX parent_grp_idx ON parent (grp);
CREATE INDEX parent_state_idx ON parent (state);
