#!/usr/bin/env bash
# The seeds 5-6 and the side seed 26 of col_scope_oracle.sh (a job of its own in the full tier; the quick tier runs the seeds of the first file only).
SIFT_ORACLE_SHARD=c . "$(dirname "$0")/col_scope_oracle.sh"
