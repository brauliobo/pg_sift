-- The sweep of the shared value stores and scope journals takes the lock of the stores and opens every table of the database that has one: a superuser, or the owner of the extension through
-- the event trigger that runs it (SECURITY DEFINER), may start it, no other role. The registries of the server (sift._quarantine, sift._progress, sift_internal.maint_queue) stay open: the
-- views and sift.activity() call them as the caller, and they return a role the rows of the relations it may inspect only (sift._may_inspect).
REVOKE EXECUTE ON FUNCTION sift._col_value_store_drop_unused() FROM PUBLIC;
