CREATE INDEX event_col ON event USING sift_col (account_id, seq, amount, at);
CREATE INDEX account_col ON account USING sift_col (id, n, plan, region);
