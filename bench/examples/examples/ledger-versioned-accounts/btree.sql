CREATE INDEX entry_account_idx ON entry (account_id, posted_on);
CREATE INDEX entry_posted_idx ON entry (posted_on);
CREATE INDEX account_version_range_idx ON account_version (account_id, valid_from, valid_to);
