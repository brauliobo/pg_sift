#!/usr/bin/env bash
# A directory snapshot (core/store.h SiftDirSnapshot) holds as many entries as its directory does, in one allocation that sift_dir_read, sift_dir_read_segs, sift_dir_copy or
# sift_dir_alloc makes; its size is sift_dir_bytes. A declaration of one, a member of it in another type, sizeof of it and a copy of one into another by assignment size it for the
# header alone. Take the snapshot from the reader that makes it, or ask sift_dir_alloc for one with room for the entries to collect.
#
#   scripts/check-dir-snapshot.sh
set -euo pipefail
cd "$(dirname "$0")/.."
bad=$(git grep -nE 'sizeof\((struct )?SiftDirSnapshot\)|^\s*(const )?SiftDirSnapshot\s+[A-Za-z_][A-Za-z_0-9]*\s*(\[[^]]*\])?\s*(;|=)|\*[A-Za-z_][A-Za-z_0-9]*\s*=\s*\*(snap|dir|view|all|own)\b' -- src ':!src/core/store.h' | grep -vE '^[^:]+:[0-9]+:\s*(/?\*|//)' || true)
[[ -z $bad ]] || { echo "check-dir-snapshot: these lines declare, size or copy a SiftDirSnapshot by value; read it with sift_dir_read / sift_dir_read_segs, copy it with sift_dir_copy, collect entries into sift_dir_alloc(n) (a header-sized snapshot silently loses every entry past it, so do not patch the call site with a bigger buffer):"; echo "$bad"; exit 1; }
echo "check-dir-snapshot: ok"
