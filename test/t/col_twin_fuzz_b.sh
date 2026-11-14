#!/usr/bin/env bash
# The seeds 13-24 of col_twin_fuzz.sh (a job of its own in the full tier; the quick tier runs the seeds of the first file only).
SIFT_FUZZ_SHARD=b . "$(dirname "$0")/col_twin_fuzz.sh"
