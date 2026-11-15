-- sift.stats() returns the counters of the server, summed over every backend since it started (a pooler hands a session a different backend for each transaction, so the
-- figures of one backend said little); sift.stats(local => true) returns this session's, the rows sift.stats() returned before. Both lists have the same rows. sift.stats_reset() still
-- resets the session's counters alone: the totals of the server only grow, read two of them to measure what happened between.
COMMENT ON FUNCTION sift.stats() IS 'What the aggregate pushdown (Custom Scan (SiftAgg)) scans did, one row per counter and per refusal slug or limit met, summed over every backend of the server since it started. The totals are in shared memory and start again with the server; sift.stats(local => true) returns this session''s.';

CREATE FUNCTION sift.stats(local boolean, OUT name text, OUT value int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_stats_read' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.stats(boolean) IS 'sift.stats() of this session with local => true (the figures of this backend, lost with the session), of the whole server with local => false.';

COMMENT ON FUNCTION sift.stats_reset() IS 'Resets the session counters that sift.stats(local => true) reports; the totals of the server are not reset.';
