#!/usr/bin/env bash
# The seeds 9-16 of tbl_twin_fuzz.sh (a job of its own in the full tier; the quick tier runs the seeds of the first file only).
SIFT_FUZZ_SHARD=b . "$(dirname "$0")/tbl_twin_fuzz.sh"
