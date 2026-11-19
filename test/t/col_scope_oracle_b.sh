#!/usr/bin/env bash
# The seeds 3-4 and the side seeds 22 and 24 of col_scope_oracle.sh (a job of its own in the full tier; the quick tier runs the seeds of the first file only).
SIFT_ORACLE_SHARD=b . "$(dirname "$0")/col_scope_oracle.sh"
