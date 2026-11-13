CREATE INDEX entry_col ON entry USING sift_col (account_id, posted_on, amount, kind);
CREATE INDEX account_version_col ON account_version USING sift_col (account_id, valid_from, valid_to, segment, risk);
CREATE INDEX account_col ON account USING sift_col (id, kind);
